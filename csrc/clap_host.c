/* clap_host.c -- see clap_host.h for what this is and why it is shaped
 * this way. Headless CLAP host: dlopen a bundle, activate one plugin at a
 * fixed block size, and run it once per tick from a clocked equation.
 *
 * Everything here is deliberately allocation-free after open: the buffers
 * and the event list are static, sized by the compile-time maxima in the
 * header. A host that allocated per block would be breaking the one
 * discipline every plugin author is asked to keep, while asking plugins to
 * keep it.
 */

#include "clap_host.h"
#include "vendor/clap/clap.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- *
 * 1. Error reporting
 *
 * Every failure path sets this and returns a failing value. A host that
 * degraded quietly would be worse than useless: a plugin that did not
 * load would look like a plugin that does nothing, which is a perfectly
 * plausible thing for an audio effect to do.
 * ---------------------------------------------------------------- */

static char ERR[512];

static void set_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ERR, sizeof ERR, fmt, ap);
    va_end(ap);
}

const char *clap_host_last_error(void) { return ERR; }

/* ---------------------------------------------------------------- *
 * 2. State
 * ---------------------------------------------------------------- */

/* One event per declared parameter, plus the four legacy clap_process()
 * slots, so a block that drives every parameter of the widest plugin this
 * host will cache still cannot overflow the queue. */
#define EV_MAX (CLAP_HOST_MAX_PARAMS + CLAP_HOST_PARAM_SLOTS)

/* One plugin's descriptor, copied rather than pointed at: the strings belong
 * to the module, and a scan of a bundle we do not go on to open closes it. */
typedef struct {
    char id[CLAP_HOST_DESC_STR];
    char name[CLAP_HOST_DESC_STR];
    char vendor[CLAP_HOST_DESC_STR];
    char version[CLAP_HOST_DESC_STR];
    char description[CLAP_HOST_DESC_STR];
    long n_features;
    char features[CLAP_HOST_MAX_FEATURES][CLAP_HOST_FEATURE_STR];
} desc_t;

typedef struct {
    void                          *dl;
    const clap_plugin_entry_t     *entry;
    const clap_plugin_factory_t   *factory;
    const clap_plugin_t           *plugin;
    const clap_plugin_params_t    *params;
    const clap_plugin_latency_t   *latency;

    int    open;          /* plugin instantiated, activated and processing */
    int    entry_inited;  /* entry->init() succeeded, so deinit() is owed  */

    double sample_rate;
    long   block;         /* frames per process() call, exactly           */
    long   chan;

    /* Descriptor cache from the last scan, grown to fit whatever the bundle
     * holds -- the one thing the host cannot size in advance. Allocated at
     * scan time, never in the processing path. */
    long    n_desc;
    long    cap_desc;
    desc_t *desc;
    char    plugin_name[256];

    /* Parameter cache, read once at open. */
    long   n_params;
    double p_id[CLAP_HOST_MAX_PARAMS];
    double p_min[CLAP_HOST_MAX_PARAMS];
    double p_max[CLAP_HOST_MAX_PARAMS];
    double p_def[CLAP_HOST_MAX_PARAMS];
    char   p_name[CLAP_HOST_MAX_PARAMS][CLAP_NAME_SIZE];

    /* Last value sent per slot, so a held parameter costs one event and
     * not one per block. NaN means "nothing sent yet". */
    double slot_id[CLAP_HOST_PARAM_SLOTS];
    double slot_val[CLAP_HOST_PARAM_SLOTS];

    /* The same, for clap_set_param, keyed by the parameter's position in
     * the cache rather than by a slot: a chain names a parameter once and
     * the position is what makes "has this one changed" answerable without
     * the caller holding a slot open for it. */
    double sent_val[CLAP_HOST_MAX_PARAMS];

    /* Descriptor index of the open plugin within the last scan, -1 when
     * nothing is open. What clap_expect() compares against. */
    long   open_index;

    float  in[CLAP_HOST_MAX_CHAN][CLAP_HOST_MAX_BLOCK];
    float  out[CLAP_HOST_MAX_CHAN][CLAP_HOST_MAX_BLOCK];
    float *in_p[CLAP_HOST_MAX_CHAN];
    float *out_p[CLAP_HOST_MAX_CHAN];

    long   in_token;      /* monotonic; 0 means "no input block yet"  */
    long   out_token;
    long   in_n;          /* frames actually in the input block       */
    long   out_n;
    long   n_process;
    int64_t steady;       /* frames processed, for clap_process.steady_time */

    clap_event_param_value_t ev[EV_MAX];
    uint32_t                 ev_n;
} state_t;

static state_t S;

/* ---------------------------------------------------------------- *
 * 3. The host we present to the plugin
 *
 * Only the four mandatory callbacks plus get_extension. Every host
 * extension returns NULL: a stub that claimed to support, say,
 * clap.thread-check and then lied about which thread we were on would be
 * worse than an honest refusal, because a plugin is entitled to trust it.
 * ---------------------------------------------------------------- */

static const void *host_get_extension(const clap_host_t *h, const char *id) {
    (void)h; (void)id;
    return NULL;   /* honest: we support no host extensions */
}

/* The plugin asking to be restarted, or for process() to be called, or for
 * a main-thread callback. A synchronous model has no scheduler to ask, so
 * these are recorded and otherwise ignored -- the plugin is driven by the
 * clock, which is the whole point. */
static long HOST_RESTART_REQS, HOST_PROCESS_REQS, HOST_CALLBACK_REQS;

static void host_request_restart(const clap_host_t *h)  { (void)h; HOST_RESTART_REQS++; }
static void host_request_process(const clap_host_t *h)  { (void)h; HOST_PROCESS_REQS++; }
static void host_request_callback(const clap_host_t *h) { (void)h; HOST_CALLBACK_REQS++; }

static const clap_host_t HOST = {
    .clap_version = CLAP_VERSION_INIT,
    .host_data = NULL,
    .name = "DyadClapHost",
    .vendor = "JuliaHub",
    .url = "https://juliahub.com",
    .version = "0.1.0",
    .get_extension = host_get_extension,
    .request_restart = host_request_restart,
    .request_process = host_request_process,
    .request_callback = host_request_callback,
};

/* ---------------------------------------------------------------- *
 * 4. Event lists
 *
 * The input list is the parameter changes for this block. The output list
 * discards -- a plugin may emit parameter changes of its own (a
 * compressor reporting gain reduction, say), and reading those back is a
 * feature this host does not have.
 * ---------------------------------------------------------------- */

static uint32_t in_ev_size(const struct clap_input_events *l) {
    (void)l;
    return S.ev_n;
}

static const clap_event_header_t *in_ev_get(const struct clap_input_events *l, uint32_t i) {
    (void)l;
    return (i < S.ev_n) ? &S.ev[i].header : NULL;
}

static long OUT_EV_DROPPED;

static bool out_ev_push(const struct clap_output_events *l, const clap_event_header_t *e) {
    (void)l; (void)e;
    OUT_EV_DROPPED++;
    return true;
}

static const clap_input_events_t  IN_EV  = { .ctx = NULL, .size = in_ev_size, .get = in_ev_get };
static const clap_output_events_t OUT_EV = { .ctx = NULL, .try_push = out_ev_push };

/* ---------------------------------------------------------------- *
 * 5. Discovery
 * ---------------------------------------------------------------- */

/* dlopen(RTLD_LOCAL | RTLD_NOW) and friends, spelled with LoadLibrary on
 * Windows, where those two flags describe the only behaviour the loader has. */
#ifdef _WIN32

static char    DLERR[256];
static wchar_t WPATH[4096], WFULL[4096];

static void set_dlerr_from_last_error(void) {
    DWORD code = GetLastError();
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL, code, 0, DLERR, sizeof DLERR, NULL);
    while (n > 0 && (DLERR[n - 1] == '\r' || DLERR[n - 1] == '\n' || DLERR[n - 1] == ' '))
        DLERR[--n] = '\0';
    if (n == 0) snprintf(DLERR, sizeof DLERR, "Windows error %lu", (unsigned long)code);
}

/* Paths arrive as UTF-8, which only the wide API accepts in full.
 * LOAD_WITH_ALTERED_SEARCH_PATH lets a plugin find sibling DLLs next to
 * itself, as a DAW would, and is defined only for a full path. */
static void *host_dlopen(const char *path) {
    DLERR[0] = '\0';
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                                WPATH, (int)(sizeof WPATH / sizeof WPATH[0]));
    if (n <= 0) { set_dlerr_from_last_error(); return NULL; }
    DWORD m = GetFullPathNameW(WPATH, (DWORD)(sizeof WFULL / sizeof WFULL[0]), WFULL, NULL);
    if (m == 0 || m >= sizeof WFULL / sizeof WFULL[0]) { set_dlerr_from_last_error(); return NULL; }
    HMODULE h = LoadLibraryExW(WFULL, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) set_dlerr_from_last_error();
    return (void *)h;
}

static void *host_dlsym(void *dl, const char *name) {
    void *p = (void *)GetProcAddress((HMODULE)dl, name);
    if (!p) set_dlerr_from_last_error();
    return p;
}

static void host_dlclose(void *dl) { FreeLibrary((HMODULE)dl); }

static const char *host_dlerror(void) { return DLERR; }

#else

static void *host_dlopen(const char *path) { return dlopen(path, RTLD_LOCAL | RTLD_NOW); }
static void *host_dlsym(void *dl, const char *name) { return dlsym(dl, name); }
static void host_dlclose(void *dl) { dlclose(dl); }
static const char *host_dlerror(void) { return dlerror(); }

#endif

/* A .clap bundle is a shared object (a DLL renamed .clap on Windows); on
 * macOS it is a directory bundle whose binary lives at
 * Contents/MacOS/<name>. Resolve both shapes. */
static void *open_bundle(const char *path) {
    void *dl = host_dlopen(path);
    if (dl) return dl;
#ifdef __APPLE__
    char inner[1024];
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char stem[512];
    snprintf(stem, sizeof stem, "%s", base);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = '\0';
    snprintf(inner, sizeof inner, "%s/Contents/MacOS/%s", path, stem);
    dl = host_dlopen(inner);
    if (dl) return dl;
#endif
    return NULL;
}

static void unload(void) {
    if (S.entry_inited && S.entry && S.entry->deinit) S.entry->deinit();
    S.entry_inited = 0;
    if (S.dl) host_dlclose(S.dl);
    S.dl = NULL;
    S.entry = NULL;
    S.factory = NULL;
}

/* Not a cap on what a bundle may hold -- the cache grows to fit -- but a
 * bound on what a factory is believed when it answers get_plugin_count. A
 * module reporting millions has misread its own memory; refuse it rather
 * than allocate for it. */
#define CLAP_HOST_SCAN_SANITY 65536

/* Grow the cache to at least `n` entries. Never shrinks, so scanning a big
 * bundle and then a small one does not churn the allocation. */
static int grow_desc(long n) {
    if (n <= S.cap_desc) return 1;
    desc_t *p = (desc_t *)realloc(S.desc, (size_t)n * sizeof(desc_t));
    if (!p) return 0;
    S.desc = p;
    S.cap_desc = n;
    return 1;
}

/* Copy one factory descriptor into the cache. Every optional field becomes
 * "" when the plugin left it NULL, so a reader never has to distinguish
 * "unset" from "empty" -- CLAP itself says the two mean the same thing. */
static void copy_desc(desc_t *out, const clap_plugin_descriptor_t *d) {
    memset(out, 0, sizeof *out);
    snprintf(out->id, sizeof out->id, "%s", d->id ? d->id : "");
    snprintf(out->name, sizeof out->name, "%s", d->name ? d->name : "");
    snprintf(out->vendor, sizeof out->vendor, "%s", d->vendor ? d->vendor : "");
    snprintf(out->version, sizeof out->version, "%s", d->version ? d->version : "");
    snprintf(out->description, sizeof out->description, "%s",
             d->description ? d->description : "");
    out->n_features = 0;
    if (!d->features) return;
    for (const char *const *f = d->features;
         *f && out->n_features < CLAP_HOST_MAX_FEATURES; f++) {
        snprintf(out->features[out->n_features], CLAP_HOST_FEATURE_STR, "%s", *f);
        out->n_features++;
    }
}

long clap_host_scan(const char *path) {
    ERR[0] = '\0';
    clap_host_close();
    /* Cleared here rather than just before the loop, so a scan that fails
     * leaves a count of 0 instead of the previous bundle's. clap_host_close
     * still preserves the cache, which is what a *failed open* wants. */
    S.n_desc = 0;

    if (!path || !path[0]) { set_err("no plugin path given"); return -1; }

    S.dl = open_bundle(path);
    if (!S.dl) { set_err("dlopen %s: %s", path, host_dlerror()); return -1; }

    const clap_plugin_entry_t *e = (const clap_plugin_entry_t *)host_dlsym(S.dl, "clap_entry");
    if (!e) {
        set_err("%s has no clap_entry symbol -- not a CLAP plugin", path);
        unload();
        return -1;
    }
    if (!clap_version_is_compatible(e->clap_version)) {
        set_err("%s targets CLAP %u.%u.%u, incompatible with the %u.%u.%u headers "
                "this host was built against",
                e->clap_version.major, e->clap_version.minor, e->clap_version.revision,
                (unsigned)CLAP_VERSION_MAJOR, (unsigned)CLAP_VERSION_MINOR,
                (unsigned)CLAP_VERSION_REVISION);
        unload();
        return -1;
    }
    if (e->init && !e->init(path)) {
        set_err("clap_entry->init failed for %s", path);
        unload();
        return -1;
    }
    S.entry = e;
    S.entry_inited = 1;

    const clap_plugin_factory_t *f =
        (const clap_plugin_factory_t *)e->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (!f) {
        set_err("%s offers no plugin factory", path);
        unload();
        return -1;
    }
    S.factory = f;

    uint32_t n = f->get_plugin_count(f);
    if (n > CLAP_HOST_SCAN_SANITY) {
        set_err("%s reports %u plugins, past the %d this host will believe",
                path, n, CLAP_HOST_SCAN_SANITY);
        unload();
        return -1;
    }
    if (!grow_desc((long)n)) {
        set_err("out of memory caching %u descriptors from %s", n, path);
        unload();
        return -1;
    }
    for (uint32_t i = 0; i < n; i++) {
        const clap_plugin_descriptor_t *d = f->get_plugin_descriptor(f, i);
        if (!d) continue;
        copy_desc(&S.desc[S.n_desc], d);
        S.n_desc++;
    }
    if (S.n_desc == 0) {
        set_err("%s contains no usable plugin descriptors", path);
        unload();
        return -1;
    }
    return S.n_desc;
}

long clap_host_scan_count(void) { return S.n_desc; }

#define SCAN_FIELD(field)                                       \
    return (i >= 0 && i < S.n_desc) ? S.desc[i].field : ""

const char *clap_host_scan_id(long i)          { SCAN_FIELD(id); }
const char *clap_host_scan_name(long i)        { SCAN_FIELD(name); }
const char *clap_host_scan_vendor(long i)      { SCAN_FIELD(vendor); }
const char *clap_host_scan_version(long i)     { SCAN_FIELD(version); }
const char *clap_host_scan_description(long i) { SCAN_FIELD(description); }

#undef SCAN_FIELD

long clap_host_scan_n_features(long i) {
    return (i >= 0 && i < S.n_desc) ? S.desc[i].n_features : 0;
}
const char *clap_host_scan_feature(long i, long k) {
    if (i < 0 || i >= S.n_desc) return "";
    if (k < 0 || k >= S.desc[i].n_features) return "";
    return S.desc[i].features[k];
}

/* ---------------------------------------------------------------- *
 * 6. Open / close
 * ---------------------------------------------------------------- */

static void read_params(void) {
    S.n_params = 0;
    if (!S.params) return;
    uint32_t n = S.params->count(S.plugin);
    for (uint32_t i = 0; i < n && S.n_params < CLAP_HOST_MAX_PARAMS; i++) {
        clap_param_info_t info;
        memset(&info, 0, sizeof info);
        if (!S.params->get_info(S.plugin, i, &info)) continue;
        long k = S.n_params++;
        S.p_id[k]  = (double)info.id;
        S.p_min[k] = info.min_value;
        S.p_max[k] = info.max_value;
        S.p_def[k] = info.default_value;
        snprintf(S.p_name[k], sizeof S.p_name[0], "%s", info.name);
    }
}

int clap_host_open(const char *path, const char *plugin_id,
                   double sample_rate, double block_size, double channels) {
    long n = clap_host_scan(path);      /* also clears state and sets ERR */
    if (n < 0) return 1;

    long blk = (long)(block_size + 0.5);
    long ch  = (long)(channels + 0.5);
    if (blk < 1 || blk > CLAP_HOST_MAX_BLOCK) {
        set_err("block_size %ld out of range 1..%d", blk, CLAP_HOST_MAX_BLOCK);
        clap_host_close();
        return 1;
    }
    if (ch < 1 || ch > CLAP_HOST_MAX_CHAN) {
        set_err("channels %ld out of range 1..%d", ch, CLAP_HOST_MAX_CHAN);
        clap_host_close();
        return 1;
    }
    if (!(sample_rate > 0.0)) {
        set_err("sample_rate must be positive, got %g", sample_rate);
        clap_host_close();
        return 1;
    }

    const char *want = (plugin_id && plugin_id[0]) ? plugin_id : S.desc[0].id;
    long which = -1;
    for (long i = 0; i < S.n_desc; i++)
        if (strcmp(S.desc[i].id, want) == 0) { which = i; break; }
    if (which < 0) {
        set_err("no plugin with id '%s' in %s (it has %ld: first is '%s')",
                want, path, S.n_desc, S.desc[0].id);
        clap_host_close();
        return 1;
    }

    const clap_plugin_t *p = S.factory->create_plugin(S.factory, &HOST, want);
    if (!p) {
        set_err("create_plugin('%s') returned nothing", want);
        clap_host_close();
        return 1;
    }
    if (!p->init(p)) {
        set_err("plugin '%s' failed to init", want);
        p->destroy(p);
        clap_host_close();
        return 1;
    }
    S.plugin = p;
    snprintf(S.plugin_name, sizeof S.plugin_name, "%s",
             (p->desc && p->desc->name) ? p->desc->name : want);

    /* Fixed block size: min == max, so a plugin that needs a variable
     * block fails here rather than at the first tick. */
    if (!p->activate(p, sample_rate, (uint32_t)blk, (uint32_t)blk)) {
        set_err("plugin '%s' refused activate(%g Hz, %ld..%ld frames)",
                want, sample_rate, blk, blk);
        p->destroy(p);
        S.plugin = NULL;
        clap_host_close();
        return 1;
    }
    if (!p->start_processing(p)) {
        set_err("plugin '%s' refused start_processing", want);
        p->deactivate(p);
        p->destroy(p);
        S.plugin = NULL;
        clap_host_close();
        return 1;
    }

    S.params  = (const clap_plugin_params_t *)p->get_extension(p, CLAP_EXT_PARAMS);
    S.latency = (const clap_plugin_latency_t *)p->get_extension(p, CLAP_EXT_LATENCY);
    read_params();

    S.sample_rate = sample_rate;
    S.block = blk;
    S.chan  = ch;
    for (long c = 0; c < CLAP_HOST_MAX_CHAN; c++) {
        S.in_p[c]  = S.in[c];
        S.out_p[c] = S.out[c];
    }
    for (int i = 0; i < CLAP_HOST_PARAM_SLOTS; i++) {
        S.slot_id[i]  = -1.0;
        S.slot_val[i] = NAN;
    }
    for (long i = 0; i < CLAP_HOST_MAX_PARAMS; i++) S.sent_val[i] = NAN;
    S.open_index = which;
    S.in_token = S.out_token = 0;
    S.in_n = S.out_n = 0;
    S.n_process = 0;
    S.steady = 0;
    S.open = 1;
    return 0;
}

void clap_host_close(void) {
    if (S.plugin) {
        if (S.open) S.plugin->stop_processing(S.plugin);
        S.plugin->deactivate(S.plugin);
        S.plugin->destroy(S.plugin);
    }
    /* The descriptor cache survives deliberately: a scan that found
     * plugins is information a caller still wants after a failed open,
     * which is why the reset below is field-by-field rather than a
     * memset of the whole state. */
    S.plugin = NULL;
    S.params = NULL;
    S.latency = NULL;
    S.open = 0;
    S.n_params = 0;
    S.open_index = -1;
    S.in_token = S.out_token = 0;
    S.in_n = S.out_n = 0;
    S.n_process = 0;
    S.steady = 0;
    S.ev_n = 0;
    S.plugin_name[0] = '\0';
    unload();
}

const char *clap_host_plugin_name(void) { return S.plugin_name; }

/* ---------------------------------------------------------------- *
 * 7. Parameter and configuration reporting
 * ---------------------------------------------------------------- */

long   clap_host_n_params(void) { return S.n_params; }

static int pidx(long i) { return (i >= 0 && i < S.n_params); }

double clap_host_param_id(long i)      { return pidx(i) ? S.p_id[i]  : -1.0; }
double clap_host_param_min(long i)     { return pidx(i) ? S.p_min[i] : NAN; }
double clap_host_param_max(long i)     { return pidx(i) ? S.p_max[i] : NAN; }
double clap_host_param_default(long i) { return pidx(i) ? S.p_def[i] : NAN; }
const char *clap_host_param_name(long i) { return pidx(i) ? S.p_name[i] : ""; }

double clap_host_param_value(double param_id) {
    if (!S.open || !S.params) return NAN;
    double out = NAN;
    if (!S.params->get_value(S.plugin, (clap_id)(uint32_t)param_id, &out)) return NAN;
    return out;
}

double clap_host_latency(void) {
    if (!S.open || !S.latency) return 0.0;
    return (double)S.latency->get(S.plugin);
}

double clap_host_sample_rate(void) { return S.open ? S.sample_rate : 0.0; }
double clap_host_block_size(void)  { return S.open ? (double)S.block : 0.0; }
double clap_host_channels(void)    { return S.open ? (double)S.chan : 0.0; }
double clap_host_is_open(void)     { return S.open ? 1.0 : 0.0; }
long   clap_host_n_process(void)   { return S.n_process; }

void clap_host_reset_counters(void) {
    S.n_process = 0;
    S.steady = 0;
    HOST_RESTART_REQS = HOST_PROCESS_REQS = HOST_CALLBACK_REQS = 0;
    OUT_EV_DROPPED = 0;
}

/* ---------------------------------------------------------------- *
 * 8. The input block
 * ---------------------------------------------------------------- */

double clap_in_fill(const double *samples, long n, long channels) {
    if (!S.open) { set_err("no plugin is open"); return NAN; }
    if (!samples || n < 0) { set_err("clap_in_fill: no samples"); return NAN; }
    if (n > S.block) n = S.block;
    long ch = (channels < 1) ? 1 : (channels > S.chan ? S.chan : channels);
    for (long i = 0; i < n; i++)
        for (long c = 0; c < S.chan; c++) {
            long src = (c < ch) ? (i * ch + c) : (i * ch);   /* mono -> all */
            S.in[c][i] = (float)samples[src];
        }
    for (long i = n; i < S.block; i++)
        for (long c = 0; c < S.chan; c++) S.in[c][i] = 0.0f;
    S.in_n = n;
    S.ev_n = 0;                     /* a new block, so any pending chain is void */
    return (double)(++S.in_token);
}

static double wave_at(long k, int w, double freq, double amp) {
    double t = (double)k / S.sample_rate;
    switch (w) {
    case CLAP_WAVE_SINE:    return amp * sin(2.0 * M_PI * freq * t);
    case CLAP_WAVE_SQUARE:  return amp * (sin(2.0 * M_PI * freq * t) >= 0.0 ? 1.0 : -1.0);
    case CLAP_WAVE_RAMP:    return amp * (2.0 * fmod(freq * t, 1.0) - 1.0);
    case CLAP_WAVE_IMPULSE: return (k == 0) ? amp : 0.0;
    default:                return 0.0;
    }
}

double clap_in_tone(double t, double waveform, double freq, double amp) {
    if (!S.open) { set_err("no plugin is open"); return NAN; }
    /* The block ENDING at t, so the frame is a pure function of the
     * arguments and two reads in one tick cannot disagree. */
    double end = t * S.sample_rate;
    long first = (long)(end + 0.5) - S.block;
    int w = (int)(waveform + 0.5);
    for (long i = 0; i < S.block; i++) {
        double v = wave_at(first + i, w, freq, amp);
        for (long c = 0; c < S.chan; c++) S.in[c][i] = (float)v;
    }
    S.in_n = S.block;
    S.ev_n = 0;                     /* a new block, so any pending chain is void */
    return (double)(++S.in_token);
}

double clap_in_sample(double dep, double i, double ch) {
    if (!S.open) return NAN;
    if ((long)(dep + 0.5) != S.in_token) return NAN;
    long k = (long)(i + 0.5), c = (long)(ch + 0.5);
    if (k < 0 || k >= S.in_n || c < 0 || c >= S.chan) return NAN;
    return (double)S.in[c][k];
}

/* ---------------------------------------------------------------- *
 * 9. process() -- the node-side operator
 * ---------------------------------------------------------------- */

static int queue_param(double id, double val) {
    if (!(id >= 0.0) || S.ev_n >= EV_MAX) return 0;
    if (isnan(val)) return 0;
    clap_event_param_value_t *e = &S.ev[S.ev_n];
    memset(e, 0, sizeof *e);
    e->header.size     = sizeof *e;
    e->header.time     = 0;                       /* at the block boundary */
    e->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e->header.type     = CLAP_EVENT_PARAM_VALUE;
    e->header.flags    = 0;
    e->param_id        = (clap_id)(uint32_t)id;
    e->cookie          = NULL;
    e->note_id         = -1;
    e->port_index      = -1;
    e->channel         = -1;
    e->key             = -1;
    e->value           = val;
    S.ev_n++;
    return 1;
}

/* Where a parameter sits in the cache read at open, or -1 for an id the
 * plugin does not declare. Linear over at most CLAP_HOST_MAX_PARAMS, once
 * per parameter per block: a hash would be more code than the loop costs. */
static long param_pos(double id) {
    for (long i = 0; i < S.n_params; i++)
        if (S.p_id[i] == id) return i;
    return -1;
}

double clap_set_param(double dep, double id, double value) {
    if (!S.open) { set_err("no plugin is open"); return NAN; }
    if ((long)(dep + 0.5) != S.in_token || S.in_token == 0) return NAN;
    long k = param_pos(id);
    if (k < 0) {
        set_err("plugin '%s' declares no parameter with id %g", S.plugin_name, id);
        return NAN;
    }
    if (isnan(value)) return NAN;
    /* Only what changed, the same rule clap_process()'s slots follow: a
     * held parameter is one event on the first block and none after. */
    if (isnan(S.sent_val[k]) || S.sent_val[k] != value) {
        if (!queue_param(id, value)) {
            set_err("parameter event queue full (%d) driving id %g", EV_MAX, id);
            return NAN;
        }
        S.sent_val[k] = value;
    }
    return dep;
}

double clap_expect(double dep, double index) {
    if (isnan(dep)) return NAN;
    if (!S.open) { set_err("no plugin is open"); return NAN; }
    long want = (long)(index + 0.5);
    if (want != S.open_index) {
        set_err("expected plugin %ld of the bundle, but '%s' (%ld) is open",
                want, S.plugin_name, S.open_index);
        return NAN;
    }
    return dep;
}

long clap_host_open_index(void) { return S.open ? S.open_index : -1; }

double clap_process(double dep,
                    double id0, double v0, double id1, double v1,
                    double id2, double v2, double id3, double v3) {
    if (!S.open) { set_err("no plugin is open"); return NAN; }
    if ((long)(dep + 0.5) != S.in_token || S.in_token == 0) return NAN;

    /* Only send what changed: a held parameter is one event on the first
     * block and none after. */
    const double ids[CLAP_HOST_PARAM_SLOTS]  = { id0, id1, id2, id3 };
    const double vals[CLAP_HOST_PARAM_SLOTS] = { v0,  v1,  v2,  v3  };
    for (int i = 0; i < CLAP_HOST_PARAM_SLOTS; i++) {
        if (!(ids[i] >= 0.0)) { S.slot_id[i] = -1.0; S.slot_val[i] = NAN; continue; }
        int changed = (S.slot_id[i] != ids[i]) || isnan(S.slot_val[i]) ||
                      (S.slot_val[i] != vals[i]);
        if (changed) {
            queue_param(ids[i], vals[i]);
            S.slot_id[i]  = ids[i];
            S.slot_val[i] = vals[i];
        }
    }

    clap_audio_buffer_t abin, about;
    memset(&abin, 0, sizeof abin);
    memset(&about, 0, sizeof about);
    abin.data32  = S.in_p;
    abin.channel_count = (uint32_t)S.chan;
    about.data32 = S.out_p;
    about.channel_count = (uint32_t)S.chan;

    clap_process_t pr;
    memset(&pr, 0, sizeof pr);
    pr.steady_time        = S.steady;
    pr.frames_count       = (uint32_t)S.block;
    pr.transport          = NULL;      /* free-running: no tempo, no bars */
    pr.audio_inputs       = &abin;
    pr.audio_outputs      = &about;
    pr.audio_inputs_count = 1;
    pr.audio_outputs_count = 1;
    pr.in_events          = &IN_EV;
    pr.out_events         = &OUT_EV;

    clap_process_status st = S.plugin->process(S.plugin, &pr);
    S.n_process++;
    S.steady += S.block;
    S.ev_n = 0;                     /* consumed; the next block starts empty */

    if (st == CLAP_PROCESS_ERROR) {
        set_err("plugin '%s' returned CLAP_PROCESS_ERROR", S.plugin_name);
        S.out_n = 0;
        return NAN;
    }
    S.out_n = S.block;
    return (double)(++S.out_token);
}

/* ---------------------------------------------------------------- *
 * 10. The output block
 * ---------------------------------------------------------------- */

static int out_ok(double dep) {
    return S.open && S.out_token != 0 && (long)(dep + 0.5) == S.out_token;
}

double clap_out_sample(double dep, double i, double ch) {
    if (!out_ok(dep)) return NAN;
    long k = (long)(i + 0.5), c = (long)(ch + 0.5);
    if (k < 0 || k >= S.out_n || c < 0 || c >= S.chan) return NAN;
    return (double)S.out[c][k];
}

double clap_out_rms(double dep) {
    if (!out_ok(dep)) return NAN;
    if (S.out_n <= 0) return 0.0;
    double s = 0.0;
    for (long c = 0; c < S.chan; c++)
        for (long i = 0; i < S.out_n; i++) s += (double)S.out[c][i] * (double)S.out[c][i];
    return sqrt(s / (double)(S.out_n * S.chan));
}

double clap_out_peak(double dep) {
    if (!out_ok(dep)) return NAN;
    double m = 0.0;
    for (long c = 0; c < S.chan; c++)
        for (long i = 0; i < S.out_n; i++) {
            double a = fabs((double)S.out[c][i]);
            if (a > m) m = a;
        }
    return m;
}

double clap_out_count(double dep) { return out_ok(dep) ? (double)S.out_n : NAN; }
double clap_out_valid(double dep) { return out_ok(dep) ? 1.0 : 0.0; }
