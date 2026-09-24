/* clap_host.c -- see clap_host.h for what this is and why it is shaped
 * this way. Headless CLAP host: dlopen bundles, activate plugins at a
 * fixed block size, and run each once per tick from a clocked equation.
 *
 * Everything here is deliberately allocation-free after open: the buffers
 * and event lists are per-instance, sized by the compile-time maxima in the
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

/* One event per declared parameter, plus the four legacy clap_process_ctx(s)
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

typedef struct state {
    double handle;
    struct state *next;
    clap_host_t host;
    long restart_reqs, process_reqs, callback_reqs, out_ev_dropped;
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
     * nothing is open. What clap_expect_ctx(s) compares against. */
    long   open_index;

    float  in[CLAP_HOST_MAX_CHAN][CLAP_HOST_MAX_BLOCK];
    float  out[CLAP_HOST_MAX_CHAN][CLAP_HOST_MAX_BLOCK];
    float  sink[CLAP_HOST_MAX_BLOCK];   /* one discard buffer serves every
                                         * output channel past `chan` */
    float  aux_in[CLAP_HOST_MAX_BLOCK]; /* shared silence for non-main inputs */

    /* The plugin's declared audio-port layout and the buffers process()
     * hands it, built at open while the plugin is still deactivated. */
    long   n_in_ports, n_out_ports;
    long   n_in_chan, n_out_chan;       /* channel counts summed over ports */
    long   n_in_main, n_out_main;       /* channel count of port 0, 0 if none */
    long   n_in_aux;                    /* input channels past the main port */
    clap_audio_buffer_t in_bufs[CLAP_HOST_MAX_PORTS];
    clap_audio_buffer_t out_bufs[CLAP_HOST_MAX_PORTS];
    float *in_ptr[CLAP_HOST_MAX_PORT_CHAN];
    float *out_ptr[CLAP_HOST_MAX_PORT_CHAN];

    double in_token;      /* opaque; 0 means "no input block yet"     */
    double out_token;
    long   in_n;          /* frames actually in the input block       */
    long   out_n;
    long   n_process;
    int64_t steady;       /* frames processed, for clap_process.steady_time */

    clap_event_param_value_t ev[EV_MAX];
    uint32_t                 ev_n;
} state_t;

static state_t DEFAULT_STATE;
static state_t *INSTANCES;
static double NEXT_HANDLE, NEXT_TOKEN;

/* Handles are never reused. All host calls must be serialized by the caller. */
static state_t *instance(double handle) {
    for (state_t *s = INSTANCES; s; s = s->next)
        if (s->handle == handle) return s;
    set_err("invalid or closed CLAP instance %g", handle);
    return NULL;
}

/* Keep legacy tokens unchanged, including their reset-on-open behavior.
 * Managed tokens are negative, unique across instances and opens, and exact
 * in the scalar-double ABI. Never wrap into an old token. */
static double next_token(state_t *s, double previous) {
    if (s == &DEFAULT_STATE) return previous + 1;
    if (NEXT_TOKEN >= 9007199254740991.0) {
        set_err("CLAP block token space exhausted");
        return NAN;
    }
    return -(++NEXT_TOKEN);
}

/* Preserve the default API's rounded token comparisons. Independent
 * instances require the exact opaque token, including its namespace. */
static int token_matches(state_t *s, double dep, double token) {
    return (s == &DEFAULT_STATE ? trunc(dep + 0.5) : dep) == token;
}

static void clap_host_close_ctx(state_t *s);

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

static void host_request_restart(const clap_host_t *h)  { ((state_t *)h->host_data)->restart_reqs++; }
static void host_request_process(const clap_host_t *h)  { ((state_t *)h->host_data)->process_reqs++; }
static void host_request_callback(const clap_host_t *h) { ((state_t *)h->host_data)->callback_reqs++; }

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
    state_t *s = (state_t *)l->ctx;
    return s->ev_n;
}

static const clap_event_header_t *in_ev_get(const struct clap_input_events *l, uint32_t i) {
    state_t *s = (state_t *)l->ctx;
    return (i < s->ev_n) ? &s->ev[i].header : NULL;
}

static bool out_ev_push(const struct clap_output_events *l, const clap_event_header_t *e) {
    (void)e;
    ((state_t *)l->ctx)->out_ev_dropped++;
    return true;
}

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

typedef struct module_ref {
    const clap_plugin_entry_t *entry;
    unsigned refs;
    struct module_ref *next;
} module_ref_t;
static module_ref_t *MODULES;

static int acquire_entry(const clap_plugin_entry_t *entry, const char *path) {
    for (module_ref_t *m = MODULES; m; m = m->next) {
        if (m->entry == entry) { m->refs++; return 1; }
    }
    module_ref_t *m = (module_ref_t *)calloc(1, sizeof *m);
    if (!m) { set_err("out of memory loading CLAP module"); return 0; }
    if (entry->init && !entry->init(path)) {
        free(m);
        set_err("clap_entry->init failed for %s", path);
        return 0;
    }
    m->entry = entry;
    m->refs = 1;
    m->next = MODULES;
    MODULES = m;
    return 1;
}

static void unload(state_t *s) {
    if (s->entry_inited) {
        module_ref_t **link = &MODULES;
        while (*link && (*link)->entry != s->entry) link = &(*link)->next;
        if (*link && --(*link)->refs == 0) {
            module_ref_t *m = *link;
            if (s->entry->deinit) s->entry->deinit();
            *link = m->next;
            free(m);
        }
    }
    s->entry_inited = 0;
    if (s->dl) host_dlclose(s->dl);
    s->dl = NULL;
    s->entry = NULL;
    s->factory = NULL;
}

/* Not a cap on what a bundle may hold -- the cache grows to fit -- but a
 * bound on what a factory is believed when it answers get_plugin_count. A
 * module reporting millions has misread its own memory; refuse it rather
 * than allocate for it. */
#define CLAP_HOST_SCAN_SANITY 65536

/* Grow the cache to at least `n` entries. Never shrinks, so scanning a big
 * bundle and then a small one does not churn the allocation. */
static int grow_desc(state_t *s, long n) {
    if (n <= s->cap_desc) return 1;
    desc_t *p = (desc_t *)realloc(s->desc, (size_t)n * sizeof(desc_t));
    if (!p) return 0;
    s->desc = p;
    s->cap_desc = n;
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

static long clap_host_scan_ctx(state_t *s, const char *path) {
    ERR[0] = '\0';
    clap_host_close_ctx(s);
    /* Cleared here rather than just before the loop, so a scan that fails
     * leaves a count of 0 instead of the previous bundle's. clap_host_close
     * still preserves the cache, which is what a *failed open* wants. */
    s->n_desc = 0;

    if (!path || !path[0]) { set_err("no plugin path given"); return -1; }

    s->dl = open_bundle(path);
    if (!s->dl) { set_err("dlopen %s: %s", path, host_dlerror()); return -1; }

    const clap_plugin_entry_t *e = (const clap_plugin_entry_t *)host_dlsym(s->dl, "clap_entry");
    if (!e) {
        set_err("%s has no clap_entry symbol -- not a CLAP plugin", path);
        unload(s);
        return -1;
    }
    if (!clap_version_is_compatible(e->clap_version)) {
        set_err("%s targets CLAP %u.%u.%u, incompatible with the %u.%u.%u headers "
                "this host was built against",
                e->clap_version.major, e->clap_version.minor, e->clap_version.revision,
                (unsigned)CLAP_VERSION_MAJOR, (unsigned)CLAP_VERSION_MINOR,
                (unsigned)CLAP_VERSION_REVISION);
        unload(s);
        return -1;
    }
    if (!acquire_entry(e, path)) {
        unload(s);
        return -1;
    }
    s->entry = e;
    s->entry_inited = 1;

    const clap_plugin_factory_t *f =
        (const clap_plugin_factory_t *)e->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (!f) {
        set_err("%s offers no plugin factory", path);
        unload(s);
        return -1;
    }
    s->factory = f;

    uint32_t n = f->get_plugin_count(f);
    if (n > CLAP_HOST_SCAN_SANITY) {
        set_err("%s reports %u plugins, past the %d this host will believe",
                path, n, CLAP_HOST_SCAN_SANITY);
        unload(s);
        return -1;
    }
    if (!grow_desc(s, (long)n)) {
        set_err("out of memory caching %u descriptors from %s", n, path);
        unload(s);
        return -1;
    }
    for (uint32_t i = 0; i < n; i++) {
        const clap_plugin_descriptor_t *d = f->get_plugin_descriptor(f, i);
        if (!d) continue;
        copy_desc(&s->desc[s->n_desc], d);
        s->n_desc++;
    }
    if (s->n_desc == 0) {
        set_err("%s contains no usable plugin descriptors", path);
        unload(s);
        return -1;
    }
    return s->n_desc;
}

static long clap_host_scan_count_ctx(state_t *s) { return s->n_desc; }

#define SCAN_FIELD(field)                                       \
    return (i >= 0 && i < s->n_desc) ? s->desc[i].field : ""

static const char *clap_host_scan_id_ctx(state_t *s, long i)          { SCAN_FIELD(id); }
static const char *clap_host_scan_name_ctx(state_t *s, long i)        { SCAN_FIELD(name); }
static const char *clap_host_scan_vendor_ctx(state_t *s, long i)      { SCAN_FIELD(vendor); }
static const char *clap_host_scan_version_ctx(state_t *s, long i)     { SCAN_FIELD(version); }
static const char *clap_host_scan_description_ctx(state_t *s, long i) { SCAN_FIELD(description); }

#undef SCAN_FIELD

static long clap_host_scan_n_features_ctx(state_t *s, long i) {
    return (i >= 0 && i < s->n_desc) ? s->desc[i].n_features : 0;
}
static const char *clap_host_scan_feature_ctx(state_t *s, long i, long k) {
    if (i < 0 || i >= s->n_desc) return "";
    if (k < 0 || k >= s->desc[i].n_features) return "";
    return s->desc[i].features[k];
}

/* ---------------------------------------------------------------- *
 * 6. Open / close
 * ---------------------------------------------------------------- */

static void read_params(state_t *s) {
    s->n_params = 0;
    if (!s->params) return;
    uint32_t n = s->params->count(s->plugin);
    for (uint32_t i = 0; i < n && s->n_params < CLAP_HOST_MAX_PARAMS; i++) {
        clap_param_info_t info;
        memset(&info, 0, sizeof info);
        if (!s->params->get_info(s->plugin, i, &info)) continue;
        long k = s->n_params++;
        s->p_id[k]  = (double)info.id;
        s->p_min[k] = info.min_value;
        s->p_max[k] = info.max_value;
        s->p_def[k] = info.default_value;
        snprintf(s->p_name[k], sizeof s->p_name[0], "%s", info.name);
    }
}

/* The port scan and the buffer map in one place, called while the plugin
 * is still deactivated -- the only state in which the scan is legal. Only
 * the main port (index 0) is routed: non-main inputs read aux_in, non-main
 * outputs and main output channels past `chan` write sink. */
static int read_audio_ports(state_t *s, const clap_plugin_t *p, const char *want, long ch) {
    const clap_plugin_audio_ports_t *ap =
        (const clap_plugin_audio_ports_t *)p->get_extension(p, CLAP_EXT_AUDIO_PORTS);

    /* audio-ports.h: a plugin without the extension has no audio ports. */
    if (!ap) {
        s->n_in_ports = s->n_out_ports = 0;
        s->n_in_chan = s->n_out_chan = 0;
        s->n_in_main = s->n_out_main = s->n_in_aux = 0;
        return 0;
    }

    for (int dir = 0; dir < 2; dir++) {
        bool is_input = (dir == 0);
        const char *dir_name = is_input ? "input" : "output";
        uint32_t n = ap->count(p, is_input);
        if (n > CLAP_HOST_MAX_PORTS) {
            set_err("plugin '%s' declares %u %s ports, past the %d this host serves",
                    want, n, dir_name, CLAP_HOST_MAX_PORTS);
            return 1;
        }
        clap_audio_buffer_t *bufs = is_input ? s->in_bufs : s->out_bufs;
        float **ptr = is_input ? s->in_ptr : s->out_ptr;
        long flat = 0;
        for (uint32_t i = 0; i < n; i++) {
            clap_audio_port_info_t info;
            memset(&info, 0, sizeof info);
            if (!ap->get(p, i, is_input, &info)) {
                set_err("plugin '%s' refused to describe its %s port %u",
                        want, dir_name, i);
                return 1;
            }
            /* The spec puts a main port, when there is one, at index 0. */
            if ((info.flags & CLAP_AUDIO_PORT_IS_MAIN) && i != 0) {
                set_err("plugin '%s' declares %s port %u as main -- the spec "
                        "puts the main port at index 0", want, dir_name, i);
                return 1;
            }
            if (info.channel_count < 1) {
                set_err("plugin '%s' declares a zero-channel %s port %u",
                        want, dir_name, i);
                return 1;
            }
            if (flat + (long)info.channel_count > CLAP_HOST_MAX_PORT_CHAN) {
                set_err("plugin '%s' declares more than %d %s channels across "
                        "its ports, past what this host serves",
                        want, CLAP_HOST_MAX_PORT_CHAN, dir_name);
                return 1;
            }
            memset(&bufs[i], 0, sizeof bufs[i]);
            bufs[i].data32 = &ptr[flat];
            bufs[i].channel_count = info.channel_count;
            for (uint32_t c = 0; c < info.channel_count; c++, flat++)
                ptr[flat] = i == 0
                    ? (is_input ? s->in[flat < ch ? flat : ch - 1]
                                : (flat < ch ? s->out[flat] : s->sink))
                    : (is_input ? s->aux_in : s->sink);
        }
        if (is_input) {
            s->n_in_ports = (long)n;
            s->n_in_chan = flat;
            s->n_in_main = n ? (long)bufs[0].channel_count : 0;
            s->n_in_aux = flat - s->n_in_main;
        } else {
            s->n_out_ports = (long)n;
            s->n_out_chan = flat;
            s->n_out_main = n ? (long)bufs[0].channel_count : 0;
        }
    }

    /* A main input narrower than the host block would need a channel
     * invented for it -- refuse rather than guess. */
    if (s->n_in_ports && s->n_in_main < ch) {
        set_err("plugin '%s' declares %ld main input channel(s), fewer than "
                "the %ld channel(s) asked for", want, s->n_in_main, ch);
        return 1;
    }
    return 0;
}

static int clap_host_open_ctx(state_t *s, const char *path, const char *plugin_id,
                   double sample_rate, double block_size, double channels) {
    long n = clap_host_scan_ctx(s, path);      /* also clears state and sets ERR */
    if (n < 0) return 1;

    long blk = (long)(block_size + 0.5);
    long ch  = (long)(channels + 0.5);
    if (blk < 1 || blk > CLAP_HOST_MAX_BLOCK) {
        set_err("block_size %ld out of range 1..%d", blk, CLAP_HOST_MAX_BLOCK);
        clap_host_close_ctx(s);
        return 1;
    }
    if (ch < 1 || ch > CLAP_HOST_MAX_CHAN) {
        set_err("channels %ld out of range 1..%d", ch, CLAP_HOST_MAX_CHAN);
        clap_host_close_ctx(s);
        return 1;
    }
    if (!(sample_rate > 0.0)) {
        set_err("sample_rate must be positive, got %g", sample_rate);
        clap_host_close_ctx(s);
        return 1;
    }

    const char *want = (plugin_id && plugin_id[0]) ? plugin_id : s->desc[0].id;
    long which = -1;
    for (long i = 0; i < s->n_desc; i++)
        if (strcmp(s->desc[i].id, want) == 0) { which = i; break; }
    if (which < 0) {
        set_err("no plugin with id '%s' in %s (it has %ld: first is '%s')",
                want, path, s->n_desc, s->desc[0].id);
        clap_host_close_ctx(s);
        return 1;
    }

    s->host = HOST;
    s->host.host_data = s;
    const clap_plugin_t *p = s->factory->create_plugin(s->factory, &s->host, want);
    if (!p) {
        set_err("create_plugin('%s') returned nothing", want);
        clap_host_close_ctx(s);
        return 1;
    }
    if (!p->init(p)) {
        set_err("plugin '%s' failed to init", want);
        p->destroy(p);
        clap_host_close_ctx(s);
        return 1;
    }
    s->plugin = p;
    snprintf(s->plugin_name, sizeof s->plugin_name, "%s",
             (p->desc && p->desc->name) ? p->desc->name : want);

    /* The audio-port scan is only legal while the plugin is deactivated,
     * so it happens here: before activate, and before anything is asked
     * to run. A layout the host cannot serve fails the open. */
    if (read_audio_ports(s, p, want, ch) != 0) {
        p->destroy(p);
        s->plugin = NULL;
        clap_host_close_ctx(s);
        return 1;
    }

    /* Fixed block size: min == max, so a plugin that needs a variable
     * block fails here rather than at the first tick. */
    if (!p->activate(p, sample_rate, (uint32_t)blk, (uint32_t)blk)) {
        set_err("plugin '%s' refused activate(%g Hz, %ld..%ld frames)",
                want, sample_rate, blk, blk);
        p->destroy(p);
        s->plugin = NULL;
        clap_host_close_ctx(s);
        return 1;
    }
    if (!p->start_processing(p)) {
        set_err("plugin '%s' refused start_processing", want);
        p->deactivate(p);
        p->destroy(p);
        s->plugin = NULL;
        clap_host_close_ctx(s);
        return 1;
    }

    s->params  = (const clap_plugin_params_t *)p->get_extension(p, CLAP_EXT_PARAMS);
    s->latency = (const clap_plugin_latency_t *)p->get_extension(p, CLAP_EXT_LATENCY);
    read_params(s);

    s->sample_rate = sample_rate;
    s->block = blk;
    s->chan  = ch;
    for (int i = 0; i < CLAP_HOST_PARAM_SLOTS; i++) {
        s->slot_id[i]  = -1.0;
        s->slot_val[i] = NAN;
    }
    for (long i = 0; i < CLAP_HOST_MAX_PARAMS; i++) s->sent_val[i] = NAN;
    s->open_index = which;
    s->in_token = s->out_token = 0;
    s->in_n = s->out_n = 0;
    s->n_process = 0;
    s->steady = 0;
    s->open = 1;
    return 0;
}

static void clap_host_close_ctx(state_t *s) {
    if (s->plugin) {
        if (s->open) s->plugin->stop_processing(s->plugin);
        s->plugin->deactivate(s->plugin);
        s->plugin->destroy(s->plugin);
    }
    /* The descriptor cache survives deliberately: a scan that found
     * plugins is information a caller still wants after a failed open,
     * which is why the reset below is field-by-field rather than a
     * memset of the whole state. */
    s->plugin = NULL;
    s->params = NULL;
    s->latency = NULL;
    s->open = 0;
    s->n_params = 0;
    s->n_in_ports = s->n_out_ports = 0;
    s->n_in_chan = s->n_out_chan = 0;
    s->n_in_main = s->n_out_main = s->n_in_aux = 0;
    s->open_index = -1;
    s->in_token = s->out_token = 0;
    s->in_n = s->out_n = 0;
    s->n_process = 0;
    s->steady = 0;
    s->ev_n = 0;
    s->plugin_name[0] = '\0';
    unload(s);
}

static const char *clap_host_plugin_name_ctx(state_t *s) { return s->plugin_name; }

/* ---------------------------------------------------------------- *
 * 7. Parameter and configuration reporting
 * ---------------------------------------------------------------- */

static long clap_host_n_params_ctx(state_t *s) { return s->n_params; }

static int pidx(state_t *s, long i) { return (i >= 0 && i < s->n_params); }

static double clap_host_param_id_ctx(state_t *s, long i)      { return pidx(s, i) ? s->p_id[i]  : -1.0; }
static double clap_host_param_min_ctx(state_t *s, long i)     { return pidx(s, i) ? s->p_min[i] : NAN; }
static double clap_host_param_max_ctx(state_t *s, long i)     { return pidx(s, i) ? s->p_max[i] : NAN; }
static double clap_host_param_default_ctx(state_t *s, long i) { return pidx(s, i) ? s->p_def[i] : NAN; }
static const char *clap_host_param_name_ctx(state_t *s, long i) { return pidx(s, i) ? s->p_name[i] : ""; }

static double clap_host_param_value_ctx(state_t *s, double param_id) {
    if (!s->open || !s->params) return NAN;
    double out = NAN;
    if (!s->params->get_value(s->plugin, (clap_id)(uint32_t)param_id, &out)) return NAN;
    return out;
}

static double clap_host_latency_ctx(state_t *s) {
    if (!s->open || !s->latency) return 0.0;
    return (double)s->latency->get(s->plugin);
}

static double clap_host_sample_rate_ctx(state_t *s) { return s->open ? s->sample_rate : 0.0; }
static double clap_host_block_size_ctx(state_t *s)  { return s->open ? (double)s->block : 0.0; }
static double clap_host_channels_ctx(state_t *s)    { return s->open ? (double)s->chan : 0.0; }
static double clap_host_is_open_ctx(state_t *s)     { return s->open ? 1.0 : 0.0; }
static double clap_host_n_audio_in_ctx(state_t *s)  { return s->open ? (double)s->n_in_chan  : 0.0; }
static double clap_host_n_audio_out_ctx(state_t *s) { return s->open ? (double)s->n_out_chan : 0.0; }
static long clap_host_n_process_ctx(state_t *s)   { return s->n_process; }

static void clap_host_reset_counters_ctx(state_t *s) {
    s->n_process = 0;
    s->steady = 0;
    s->restart_reqs = s->process_reqs = s->callback_reqs = 0;
    s->out_ev_dropped = 0;
}

/* ---------------------------------------------------------------- *
 * 8. The input block
 * ---------------------------------------------------------------- */

static double clap_in_fill_ctx(state_t *s, const double *samples, long n, long channels) {
    if (!s->open) { set_err("no plugin is open"); return NAN; }
    if (!samples || n < 0) { set_err("clap_in_fill: no samples"); return NAN; }
    if (n > s->block) n = s->block;
    long ch = (channels < 1) ? 1 : (channels > s->chan ? s->chan : channels);
    for (long i = 0; i < n; i++)
        for (long c = 0; c < s->chan; c++) {
            long src = (c < ch) ? (i * ch + c) : (i * ch);   /* mono -> all */
            s->in[c][i] = (float)samples[src];
        }
    for (long i = n; i < s->block; i++)
        for (long c = 0; c < s->chan; c++) s->in[c][i] = 0.0f;
    s->in_n = n;
    s->ev_n = 0;                     /* a new block, so any pending chain is void */
    return (s->in_token = next_token(s, s->in_token));
}

static double wave_at(state_t *s, long k, int w, double freq, double amp) {
    double t = (double)k / s->sample_rate;
    switch (w) {
    case CLAP_WAVE_SINE:    return amp * sin(2.0 * M_PI * freq * t);
    case CLAP_WAVE_SQUARE:  return amp * (sin(2.0 * M_PI * freq * t) >= 0.0 ? 1.0 : -1.0);
    case CLAP_WAVE_RAMP:    return amp * (2.0 * fmod(freq * t, 1.0) - 1.0);
    case CLAP_WAVE_IMPULSE: return (k == 0) ? amp : 0.0;
    default:                return 0.0;
    }
}

static double clap_in_tone_ctx(state_t *s, double t, double waveform, double freq, double amp) {
    if (!s->open) { set_err("no plugin is open"); return NAN; }
    /* The block ENDING at t, so the frame is a pure function of the
     * arguments and two reads in one tick cannot disagree. */
    double end = t * s->sample_rate;
    long first = (long)(end + 0.5) - s->block;
    int w = (int)(waveform + 0.5);
    for (long i = 0; i < s->block; i++) {
        double v = wave_at(s, first + i, w, freq, amp);
        for (long c = 0; c < s->chan; c++) s->in[c][i] = (float)v;
    }
    s->in_n = s->block;
    s->ev_n = 0;                     /* a new block, so any pending chain is void */
    return (s->in_token = next_token(s, s->in_token));
}

static double clap_in_sample_ctx(state_t *s, double dep, double i, double ch) {
    if (!s->open) return NAN;
    if (!token_matches(s, dep, s->in_token)) return NAN;
    long k = (long)(i + 0.5), c = (long)(ch + 0.5);
    if (k < 0 || k >= s->in_n || c < 0 || c >= s->chan) return NAN;
    return (double)s->in[c][k];
}

/* ---------------------------------------------------------------- *
 * 9. process() -- the node-side operator
 * ---------------------------------------------------------------- */

static int queue_param(state_t *s, double id, double val) {
    if (!(id >= 0.0) || s->ev_n >= EV_MAX) return 0;
    if (isnan(val)) return 0;
    clap_event_param_value_t *e = &s->ev[s->ev_n];
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
    s->ev_n++;
    return 1;
}

/* Where a parameter sits in the cache read at open, or -1 for an id the
 * plugin does not declare. Linear over at most CLAP_HOST_MAX_PARAMS, once
 * per parameter per block: a hash would be more code than the loop costs. */
static long param_pos(state_t *s, double id) {
    for (long i = 0; i < s->n_params; i++)
        if (s->p_id[i] == id) return i;
    return -1;
}

static double clap_set_param_ctx(state_t *s, double dep, double id, double value) {
    if (!s->open) { set_err("no plugin is open"); return NAN; }
    if (!token_matches(s, dep, s->in_token) || s->in_token == 0) return NAN;
    long k = param_pos(s, id);
    if (k < 0) {
        set_err("plugin '%s' declares no parameter with id %g", s->plugin_name, id);
        return NAN;
    }
    if (isnan(value)) return NAN;
    /* Only what changed, the same rule clap_process_ctx(s)'s slots follow: a
     * held parameter is one event on the first block and none after. */
    if (isnan(s->sent_val[k]) || s->sent_val[k] != value) {
        if (!queue_param(s, id, value)) {
            set_err("parameter event queue full (%d) driving id %g", EV_MAX, id);
            return NAN;
        }
        s->sent_val[k] = value;
    }
    return dep;
}

static double clap_expect_ctx(state_t *s, double dep, double index) {
    if (isnan(dep)) return NAN;
    if (!s->open) { set_err("no plugin is open"); return NAN; }
    long want = (long)(index + 0.5);
    if (want != s->open_index) {
        set_err("expected plugin %ld of the bundle, but '%s' (%ld) is open",
                want, s->plugin_name, s->open_index);
        return NAN;
    }
    return dep;
}

static long clap_host_open_index_ctx(state_t *s) { return s->open ? s->open_index : -1; }

static double clap_process_ctx(state_t *s, double dep,
                    double id0, double v0, double id1, double v1,
                    double id2, double v2, double id3, double v3) {
    if (!s->open) { set_err("no plugin is open"); return NAN; }
    if (!token_matches(s, dep, s->in_token) || s->in_token == 0) return NAN;

    /* Only send what changed: a held parameter is one event on the first
     * block and none after. */
    const double ids[CLAP_HOST_PARAM_SLOTS]  = { id0, id1, id2, id3 };
    const double vals[CLAP_HOST_PARAM_SLOTS] = { v0,  v1,  v2,  v3  };
    for (int i = 0; i < CLAP_HOST_PARAM_SLOTS; i++) {
        if (!(ids[i] >= 0.0)) { s->slot_id[i] = -1.0; s->slot_val[i] = NAN; continue; }
        int changed = (s->slot_id[i] != ids[i]) || isnan(s->slot_val[i]) ||
                      (s->slot_val[i] != vals[i]);
        if (changed) {
            queue_param(s, ids[i], vals[i]);
            s->slot_id[i]  = ids[i];
            s->slot_val[i] = vals[i];
        }
    }

    if (s->n_in_aux) memset(s->aux_in, 0, (size_t)s->block * sizeof(float));

    clap_process_t pr;
    memset(&pr, 0, sizeof pr);
    pr.steady_time        = s->steady;
    pr.frames_count       = (uint32_t)s->block;
    pr.transport          = NULL;      /* free-running: no tempo, no bars */
    pr.audio_inputs       = s->n_in_ports ? s->in_bufs : NULL;
    pr.audio_outputs      = s->n_out_ports ? s->out_bufs : NULL;
    pr.audio_inputs_count = (uint32_t)s->n_in_ports;
    pr.audio_outputs_count = (uint32_t)s->n_out_ports;
    const clap_input_events_t in_ev = { .ctx = s, .size = in_ev_size, .get = in_ev_get };
    const clap_output_events_t out_ev = { .ctx = s, .try_push = out_ev_push };
    pr.in_events          = &in_ev;
    pr.out_events         = &out_ev;

    clap_process_status st = s->plugin->process(s->plugin, &pr);
    s->n_process++;
    s->steady += s->block;
    s->ev_n = 0;                     /* consumed; the next block starts empty */

    if (st == CLAP_PROCESS_ERROR) {
        set_err("plugin '%s' returned CLAP_PROCESS_ERROR", s->plugin_name);
        s->out_n = 0;
        return NAN;
    }

    /* Host channels the main output did not fill: duplicate a mono main
     * output onto channel 1, zero anything else (a plugin with no outputs
     * is silence rather than whatever out[][] last held). */
    for (long c = s->n_out_main; c < s->chan; c++) {
        if (c == 1 && s->n_out_main == 1)
            memcpy(s->out[1], s->out[0], (size_t)s->block * sizeof(float));
        else
            memset(s->out[c], 0, (size_t)s->block * sizeof(float));
    }
    s->out_n = s->block;
    return (s->out_token = next_token(s, s->out_token));
}

/* ---------------------------------------------------------------- *
 * 10. The output block
 * ---------------------------------------------------------------- */

static int out_ok(state_t *s, double dep) {
    return s->open && s->out_token != 0 && token_matches(s, dep, s->out_token);
}

static double clap_out_sample_ctx(state_t *s, double dep, double i, double ch) {
    if (!out_ok(s, dep)) return NAN;
    long k = (long)(i + 0.5), c = (long)(ch + 0.5);
    if (k < 0 || k >= s->out_n || c < 0 || c >= s->chan) return NAN;
    return (double)s->out[c][k];
}

static double clap_out_rms_ctx(state_t *s, double dep) {
    if (!out_ok(s, dep)) return NAN;
    if (s->out_n <= 0) return 0.0;
    double sum = 0.0;
    for (long c = 0; c < s->chan; c++)
        for (long i = 0; i < s->out_n; i++) sum += (double)s->out[c][i] * (double)s->out[c][i];
    return sqrt(sum / (double)(s->out_n * s->chan));
}

static double clap_out_peak_ctx(state_t *s, double dep) {
    if (!out_ok(s, dep)) return NAN;
    double m = 0.0;
    for (long c = 0; c < s->chan; c++)
        for (long i = 0; i < s->out_n; i++) {
            double a = fabs((double)s->out[c][i]);
            if (a > m) m = a;
        }
    return m;
}

static double clap_out_count_ctx(state_t *s, double dep) { return out_ok(s, dep) ? (double)s->out_n : NAN; }
static double clap_out_valid_ctx(state_t *s, double dep) { return out_ok(s, dep) ? 1.0 : 0.0; }

/* Public entry points: legacy calls address the default instance; _for
 * calls resolve an independent handle. */
long clap_host_scan(const char *path) { return clap_host_scan_ctx(&DEFAULT_STATE, path); }
long clap_host_scan_count(void) { return clap_host_scan_count_ctx(&DEFAULT_STATE); }
const char *clap_host_scan_id(long i) { return clap_host_scan_id_ctx(&DEFAULT_STATE, i); }
const char *clap_host_scan_name(long i) { return clap_host_scan_name_ctx(&DEFAULT_STATE, i); }
const char *clap_host_scan_vendor(long i) { return clap_host_scan_vendor_ctx(&DEFAULT_STATE, i); }
const char *clap_host_scan_version(long i) { return clap_host_scan_version_ctx(&DEFAULT_STATE, i); }
const char *clap_host_scan_description(long i) { return clap_host_scan_description_ctx(&DEFAULT_STATE, i); }
long clap_host_scan_n_features(long i) { return clap_host_scan_n_features_ctx(&DEFAULT_STATE, i); }
const char *clap_host_scan_feature(long i, long k) { return clap_host_scan_feature_ctx(&DEFAULT_STATE, i, k); }
int clap_host_open(const char *path, const char *plugin_id, double sample_rate, double block_size, double channels) {
    return clap_host_open_ctx(&DEFAULT_STATE, path, plugin_id, sample_rate, block_size, channels);
}
void clap_host_close(void) { clap_host_close_ctx(&DEFAULT_STATE); }
const char *clap_host_plugin_name(void) { return clap_host_plugin_name_ctx(&DEFAULT_STATE); }

const char *clap_host_plugin_name_for(double handle) {
    state_t *s = instance(handle);
    if (!s) return "";
    return clap_host_plugin_name_ctx(s);
}
long clap_host_open_index(void) { return clap_host_open_index_ctx(&DEFAULT_STATE); }

long clap_host_open_index_for(double handle) {
    state_t *s = instance(handle);
    if (!s) return -1;
    return clap_host_open_index_ctx(s);
}
long clap_host_n_params(void) { return clap_host_n_params_ctx(&DEFAULT_STATE); }

long clap_host_n_params_for(double handle) {
    state_t *s = instance(handle);
    if (!s) return -1;
    return clap_host_n_params_ctx(s);
}
double clap_host_param_id(long i) { return clap_host_param_id_ctx(&DEFAULT_STATE, i); }

double clap_host_param_id_for(double handle, long i) {
    state_t *s = instance(handle);
    if (!s) return NAN;
    return clap_host_param_id_ctx(s, i);
}
double clap_host_param_min(long i) { return clap_host_param_min_ctx(&DEFAULT_STATE, i); }

double clap_host_param_min_for(double handle, long i) {
    state_t *s = instance(handle);
    if (!s) return NAN;
    return clap_host_param_min_ctx(s, i);
}
double clap_host_param_max(long i) { return clap_host_param_max_ctx(&DEFAULT_STATE, i); }

double clap_host_param_max_for(double handle, long i) {
    state_t *s = instance(handle);
    if (!s) return NAN;
    return clap_host_param_max_ctx(s, i);
}
double clap_host_param_default(long i) { return clap_host_param_default_ctx(&DEFAULT_STATE, i); }

double clap_host_param_default_for(double handle, long i) {
    state_t *s = instance(handle);
    if (!s) return NAN;
    return clap_host_param_default_ctx(s, i);
}
const char *clap_host_param_name(long i) { return clap_host_param_name_ctx(&DEFAULT_STATE, i); }

const char *clap_host_param_name_for(double handle, long i) {
    state_t *s = instance(handle);
    if (!s) return "";
    return clap_host_param_name_ctx(s, i);
}
double clap_host_param_value(double param_id) { return clap_host_param_value_ctx(&DEFAULT_STATE, param_id); }

double clap_host_param_value_for(double handle, double param_id) {
    state_t *s = instance(handle);
    if (!s) return NAN;
    return clap_host_param_value_ctx(s, param_id);
}
double clap_host_latency(void) { return clap_host_latency_ctx(&DEFAULT_STATE); }

double clap_host_latency_for(double handle) {
    state_t *s = instance(handle);
    if (!s) return NAN;
    return clap_host_latency_ctx(s);
}
double clap_host_sample_rate(void) { return clap_host_sample_rate_ctx(&DEFAULT_STATE); }

double clap_host_sample_rate_for(double handle) {
    state_t *s = instance(handle);
    if (!s) return NAN;
    return clap_host_sample_rate_ctx(s);
}
double clap_host_block_size(void) { return clap_host_block_size_ctx(&DEFAULT_STATE); }

double clap_host_block_size_for(double handle) {
    state_t *s = instance(handle);
    if (!s) return NAN;
    return clap_host_block_size_ctx(s);
}
double clap_host_channels(void) { return clap_host_channels_ctx(&DEFAULT_STATE); }

double clap_host_channels_for(double handle) {
    state_t *s = instance(handle);
    if (!s) return NAN;
    return clap_host_channels_ctx(s);
}
double clap_host_is_open(void) { return clap_host_is_open_ctx(&DEFAULT_STATE); }

double clap_host_is_open_for(double handle) {
    state_t *s = instance(handle);
    if (!s) return 0;
    return clap_host_is_open_ctx(s);
}
double clap_host_n_audio_in(void) { return clap_host_n_audio_in_ctx(&DEFAULT_STATE); }

double clap_host_n_audio_in_for(double handle) {
    state_t *s = instance(handle);
    if (!s) return NAN;
    return clap_host_n_audio_in_ctx(s);
}
double clap_host_n_audio_out(void) { return clap_host_n_audio_out_ctx(&DEFAULT_STATE); }

double clap_host_n_audio_out_for(double handle) {
    state_t *s = instance(handle);
    if (!s) return NAN;
    return clap_host_n_audio_out_ctx(s);
}
long clap_host_n_process(void) { return clap_host_n_process_ctx(&DEFAULT_STATE); }

long clap_host_n_process_for(double handle) {
    state_t *s = instance(handle);
    if (!s) return -1;
    return clap_host_n_process_ctx(s);
}
void clap_host_reset_counters(void) { clap_host_reset_counters_ctx(&DEFAULT_STATE); }

void clap_host_reset_counters_for(double handle) {
    state_t *s = instance(handle);
    if (!s) return;
    clap_host_reset_counters_ctx(s);
}
double clap_in_fill(const double *samples, long n, long channels) {
    return clap_in_fill_ctx(&DEFAULT_STATE, samples, n, channels);
}

double clap_in_fill_for(double handle, const double *samples, long n, long channels) {
    state_t *s = instance(handle);
    if (!s) return NAN;
    return clap_in_fill_ctx(s, samples, n, channels);
}
double clap_in_tone(double t, double waveform, double freq, double amp) {
    return clap_in_tone_ctx(&DEFAULT_STATE, t, waveform, freq, amp);
}

double clap_in_tone_for(double handle, double t, double waveform, double freq, double amp) {
    state_t *s = instance(handle);
    if (!s) return NAN;
    return clap_in_tone_ctx(s, t, waveform, freq, amp);
}
double clap_in_sample(double dep, double i, double ch) { return clap_in_sample_ctx(&DEFAULT_STATE, dep, i, ch); }

double clap_in_sample_for(double handle, double dep, double i, double ch) {
    state_t *s = instance(handle);
    if (!s) return NAN;
    return clap_in_sample_ctx(s, dep, i, ch);
}
double clap_process(double dep, double id0, double v0, double id1, double v1, double id2, double v2, double id3, double v3) {
    return clap_process_ctx(&DEFAULT_STATE, dep, id0, v0, id1, v1, id2, v2, id3, v3);
}

double clap_process_for(double handle, double dep, double id0, double v0, double id1, double v1, double id2, double v2, double id3, double v3) {
    state_t *s = instance(handle);
    if (!s) return NAN;
    return clap_process_ctx(s, dep, id0, v0, id1, v1, id2, v2, id3, v3);
}
double clap_set_param(double dep, double id, double value) {
    return clap_set_param_ctx(&DEFAULT_STATE, dep, id, value);
}

double clap_set_param_for(double handle, double dep, double id, double value) {
    state_t *s = instance(handle);
    if (!s) return NAN;
    return clap_set_param_ctx(s, dep, id, value);
}
double clap_expect(double dep, double index) { return clap_expect_ctx(&DEFAULT_STATE, dep, index); }

double clap_expect_for(double handle, double dep, double index) {
    state_t *s = instance(handle);
    if (!s) return NAN;
    return clap_expect_ctx(s, dep, index);
}
double clap_out_sample(double dep, double i, double ch) { return clap_out_sample_ctx(&DEFAULT_STATE, dep, i, ch); }

double clap_out_sample_for(double handle, double dep, double i, double ch) {
    state_t *s = instance(handle);
    if (!s) return NAN;
    return clap_out_sample_ctx(s, dep, i, ch);
}
double clap_out_rms(double dep) { return clap_out_rms_ctx(&DEFAULT_STATE, dep); }

double clap_out_rms_for(double handle, double dep) {
    state_t *s = instance(handle);
    if (!s) return NAN;
    return clap_out_rms_ctx(s, dep);
}
double clap_out_peak(double dep) { return clap_out_peak_ctx(&DEFAULT_STATE, dep); }

double clap_out_peak_for(double handle, double dep) {
    state_t *s = instance(handle);
    if (!s) return NAN;
    return clap_out_peak_ctx(s, dep);
}
double clap_out_count(double dep) { return clap_out_count_ctx(&DEFAULT_STATE, dep); }

double clap_out_count_for(double handle, double dep) {
    state_t *s = instance(handle);
    if (!s) return NAN;
    return clap_out_count_ctx(s, dep);
}
double clap_out_valid(double dep) { return clap_out_valid_ctx(&DEFAULT_STATE, dep); }

double clap_out_valid_for(double handle, double dep) {
    state_t *s = instance(handle);
    if (!s) return 0;
    return clap_out_valid_ctx(s, dep);
}

/* Allocate only at open, never on the audio path. */
double clap_host_open_instance(const char *path, const char *plugin_id,
                               double sample_rate, double block_size, double channels) {
    if (!isfinite(sample_rate) || sample_rate <= 0 ||
        !isfinite(block_size) || block_size < 1 || block_size > CLAP_HOST_MAX_BLOCK ||
        floor(block_size) != block_size || !isfinite(channels) || channels < 1 ||
        channels > CLAP_HOST_MAX_CHAN || floor(channels) != channels) {
        set_err("invalid CLAP instance sample rate, block size or channel count");
        return NAN;
    }
    if (NEXT_HANDLE >= 9007199254740991.0) {
        set_err("CLAP instance handle space exhausted");
        return NAN;
    }
    state_t *s = (state_t *)calloc(1, sizeof *s);
    if (!s) { set_err("out of memory opening CLAP instance"); return NAN; }
    if (clap_host_open_ctx(s, path, plugin_id, sample_rate, block_size, channels)) {
        free(s->desc);
        free(s);
        return NAN;
    }
    s->handle = ++NEXT_HANDLE;
    s->next = INSTANCES;
    INSTANCES = s;
    return s->handle;
}

void clap_host_close_instance(double handle) {
    state_t **link = &INSTANCES;
    while (*link && (*link)->handle != handle) link = &(*link)->next;
    if (!*link) return; /* idempotent; an old handle never closes a new instance */
    state_t *s = *link;
    *link = s->next;
    clap_host_close_ctx(s);
    free(s->desc);
    free(s);
}

double clap_in_copy_for(double destination, double source, double dep) {
    state_t *dst = instance(destination), *src = instance(source);
    if (!dst || !src || !out_ok(src, dep)) return NAN;
    if (dst->block != src->block || dst->chan != src->chan ||
        dst->sample_rate != src->sample_rate) {
        set_err("CLAP chain requires equal block size, channels and sample rate");
        return NAN;
    }
    for (long c = 0; c < dst->chan; c++)
        memcpy(dst->in[c], src->out[c], (size_t)dst->block * sizeof(float));
    dst->in_n = dst->block;
    dst->ev_n = 0;
    return (dst->in_token = next_token(dst, dst->in_token));
}
