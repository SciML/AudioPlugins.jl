/* lv2_host.c -- see lv2_host.h for what this is and why it is shaped this
 * way. Headless LV2 host on top of lilv: load a world, pick a plugin by
 * URI, connect every port from the manifest's description, activate at a
 * fixed block size, and run it once per tick from a clocked equation.
 *
 * Allocation-free after open, like clap_host.c: the audio and control
 * buffers are static, sized by the compile-time maxima in the header.
 * lilv itself allocates during scan and open, which is driver-side.
 *
 * Built against lilv (ISC; https://gitlab.com/lv2/lilv), which brings
 * serd, sord, sratom and zix, and against the LV2 headers. The three LV2
 * headers this file needs are vendored under vendor/lv2 so a standalone
 * C build only has to find lilv.
 */

#include "lv2_host.h"
/* Through the include path rather than by relative path, so that lilv.h
 * (which includes <lv2/core/lv2.h> itself) and this file resolve to the
 * same copy: build with -I csrc/vendor to use the vendored headers. */
#include <lv2/core/lv2.h>
#include <lv2/urid/urid.h>
#include <lv2/buf-size/buf-size.h>

#include <lilv/lilv.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- *
 * 1. Error reporting
 * ---------------------------------------------------------------- */

static char ERR[512];

static void set_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ERR, sizeof ERR, fmt, ap);
    va_end(ap);
}

const char *lv2_host_last_error(void) { return ERR; }

/* ---------------------------------------------------------------- *
 * 2. The host features we offer
 *
 * urid:map / urid:unmap because nearly every modern plugin requires
 * them; the two buf-size flags because they are true of this host and
 * a plugin that requires a fixed block size is entitled to know. The
 * URID table is a fixed array: a plugin that maps more than
 * LV2_HOST_MAX_URIDS distinct URIs at instantiation gets 0 back, which
 * the spec allows for "cannot map".
 * ---------------------------------------------------------------- */

#define LV2_HOST_MAX_URIDS 1024

static char    *URIS[LV2_HOST_MAX_URIDS];
static uint32_t N_URIS;

static LV2_URID urid_map(LV2_URID_Map_Handle h, const char *uri) {
    (void)h;
    if (!uri) return 0;
    for (uint32_t i = 0; i < N_URIS; i++)
        if (strcmp(URIS[i], uri) == 0) return i + 1;
    if (N_URIS >= LV2_HOST_MAX_URIDS) return 0;
    URIS[N_URIS] = strdup(uri);
    if (!URIS[N_URIS]) return 0;
    return ++N_URIS;
}

static const char *urid_unmap(LV2_URID_Unmap_Handle h, LV2_URID urid) {
    (void)h;
    return (urid >= 1 && urid <= N_URIS) ? URIS[urid - 1] : NULL;
}

static LV2_URID_Map   MAP   = { NULL, urid_map };
static LV2_URID_Unmap UNMAP = { NULL, urid_unmap };

static const LV2_Feature F_MAP     = { LV2_URID__map,   &MAP };
static const LV2_Feature F_UNMAP   = { LV2_URID__unmap, &UNMAP };
static const LV2_Feature F_FIXED   = { LV2_BUF_SIZE__fixedBlockLength,   NULL };
static const LV2_Feature F_BOUNDED = { LV2_BUF_SIZE__boundedBlockLength, NULL };

static const LV2_Feature *const FEATURES[] = {
    &F_MAP, &F_UNMAP, &F_FIXED, &F_BOUNDED, NULL
};

static int feature_supported(const char *uri) {
    for (const LV2_Feature *const *f = FEATURES; *f; f++)
        if (strcmp((*f)->URI, uri) == 0) return 1;
    return 0;
}

/* ---------------------------------------------------------------- *
 * 3. State
 * ---------------------------------------------------------------- */

typedef struct {
    LilvWorld        *world;
    const LilvPlugins *plugins;      /* owned by the world */
    const LilvPlugin *plugin;        /* owned by the world */
    LilvInstance     *inst;

    int    open;
    double sample_rate;
    long   block, chan;

    /* Descriptor cache from the last scan. */
    long   n_desc;
    char   desc_uri[256][512];
    char   desc_name[256][256];
    char   plugin_name[256];
    char   plugin_uri[512];
    char   lv2_path[2048];           /* the path the world was loaded with */

    /* Port census. */
    long   n_ports;
    long   n_audio_in, n_audio_out;
    long   latency_port;             /* -1 when the plugin reports none */

    /* Parameters = control input ports. */
    long   n_params;
    long   p_port[LV2_HOST_MAX_PARAMS];
    double p_min[LV2_HOST_MAX_PARAMS], p_max[LV2_HOST_MAX_PARAMS], p_def[LV2_HOST_MAX_PARAMS];
    char   p_name[LV2_HOST_MAX_PARAMS][128];
    char   p_sym[LV2_HOST_MAX_PARAMS][128];

    /* One float per port: what a control port is connected to. Audio ports
     * are connected to the block buffers below, so their entry is unused. */
    float  ctrl[LV2_HOST_MAX_PORTS];
    signed char is_ctrl_in[LV2_HOST_MAX_PORTS];

    float  in[LV2_HOST_MAX_CHAN][LV2_HOST_MAX_BLOCK];
    float  out[LV2_HOST_MAX_CHAN][LV2_HOST_MAX_BLOCK];
    float  sink[LV2_HOST_MAX_BLOCK];   /* outputs beyond `chan` go here */

    long   in_token, out_token;
    long   in_n, out_n;
    long   n_process;
} state_t;

static state_t S;

/* ---------------------------------------------------------------- *
 * 4. Discovery
 * ---------------------------------------------------------------- */

static void free_world(void) {
    if (S.inst) {
        if (S.open) lilv_instance_deactivate(S.inst);
        lilv_instance_free(S.inst);
        S.inst = NULL;
    }
    S.plugin = NULL;
    S.plugins = NULL;
    if (S.world) lilv_world_free(S.world);
    S.world = NULL;
}

static const char *node_str(const LilvNode *n) {
    return n ? lilv_node_as_string(n) : "";
}

long lv2_host_scan(const char *lv2_path) {
    ERR[0] = '\0';
    lv2_host_close();

    S.world = lilv_world_new();
    if (!S.world) { set_err("lilv_world_new failed"); return -1; }

    if (lv2_path && lv2_path[0]) {
        LilvNode *p = lilv_new_string(S.world, lv2_path);
        lilv_world_set_option(S.world, LILV_OPTION_LV2_PATH, p);
        lilv_node_free(p);
        snprintf(S.lv2_path, sizeof S.lv2_path, "%s", lv2_path);
    } else {
        S.lv2_path[0] = '\0';
    }
    lilv_world_load_all(S.world);
    S.plugins = lilv_world_get_all_plugins(S.world);

    S.n_desc = 0;
    LILV_FOREACH (plugins, i, S.plugins) {
        if (S.n_desc >= 256) break;
        const LilvPlugin *p = lilv_plugins_get(S.plugins, i);
        snprintf(S.desc_uri[S.n_desc], sizeof S.desc_uri[0], "%s",
                 node_str(lilv_plugin_get_uri(p)));
        LilvNode *name = lilv_plugin_get_name(p);
        snprintf(S.desc_name[S.n_desc], sizeof S.desc_name[0], "%s", node_str(name));
        lilv_node_free(name);
        S.n_desc++;
    }
    if (S.n_desc == 0) {
        set_err("no LV2 plugins found under '%s'",
                (lv2_path && lv2_path[0]) ? lv2_path : "(default LV2_PATH)");
        free_world();
        return -1;
    }
    return S.n_desc;
}

const char *lv2_host_scan_uri(long i)  { return (i >= 0 && i < S.n_desc) ? S.desc_uri[i]  : ""; }
const char *lv2_host_scan_name(long i) { return (i >= 0 && i < S.n_desc) ? S.desc_name[i] : ""; }

/* ---------------------------------------------------------------- *
 * 5. Open / close
 * ---------------------------------------------------------------- */

static int port_is(const LilvPlugin *p, const LilvPort *port, const char *cls) {
    LilvNode *n = lilv_new_uri(S.world, cls);
    int r = lilv_port_is_a(p, port, n);
    lilv_node_free(n);
    return r;
}

static int port_has(const LilvPlugin *p, const LilvPort *port, const char *prop) {
    LilvNode *n = lilv_new_uri(S.world, prop);
    int r = lilv_port_has_property(p, port, n);
    lilv_node_free(n);
    return r;
}

static void fail_open(void) { lv2_host_close(); }

int lv2_host_open(const char *lv2_path, const char *uri,
                  double sample_rate, double block_size, double channels) {
    long n = lv2_host_scan(lv2_path);      /* also clears state and sets ERR */
    if (n < 0) return 1;

    long blk = (long)(block_size + 0.5);
    long ch  = (long)(channels + 0.5);
    if (blk < 1 || blk > LV2_HOST_MAX_BLOCK) {
        set_err("block_size %ld out of range 1..%d", blk, LV2_HOST_MAX_BLOCK);
        fail_open();
        return 1;
    }
    if (ch < 1 || ch > LV2_HOST_MAX_CHAN) {
        set_err("channels %ld out of range 1..%d", ch, LV2_HOST_MAX_CHAN);
        fail_open();
        return 1;
    }
    if (!(sample_rate > 0.0)) {
        set_err("sample_rate must be positive, got %g", sample_rate);
        fail_open();
        return 1;
    }

    const char *want = (uri && uri[0]) ? uri : S.desc_uri[0];
    LilvNode *want_node = lilv_new_uri(S.world, want);
    const LilvPlugin *p = want_node ? lilv_plugins_get_by_uri(S.plugins, want_node) : NULL;
    lilv_node_free(want_node);
    if (!p) {
        set_err("no plugin with URI '%s' under '%s' (found %ld: first is '%s')",
                want, S.lv2_path[0] ? S.lv2_path : "(default LV2_PATH)",
                S.n_desc, S.desc_uri[0]);
        fail_open();
        return 1;
    }
    S.plugin = p;
    snprintf(S.plugin_uri, sizeof S.plugin_uri, "%s", node_str(lilv_plugin_get_uri(p)));
    {
        LilvNode *name = lilv_plugin_get_name(p);
        snprintf(S.plugin_name, sizeof S.plugin_name, "%s", node_str(name));
        lilv_node_free(name);
    }

    /* Required features we do not provide: refuse before instantiating,
     * naming the feature, rather than let the plugin refuse silently. */
    LilvNodes *req = lilv_plugin_get_required_features(p);
    LILV_FOREACH (nodes, i, req) {
        const char *f = node_str(lilv_nodes_get(req, i));
        if (!feature_supported(f)) {
            set_err("plugin '%s' requires host feature <%s>, which this host does not provide",
                    S.plugin_uri, f);
            lilv_nodes_free(req);
            fail_open();
            return 1;
        }
    }
    lilv_nodes_free(req);

    /* Port census from the manifest. */
    S.n_ports = (long)lilv_plugin_get_num_ports(p);
    if (S.n_ports > LV2_HOST_MAX_PORTS) {
        set_err("plugin '%s' has %ld ports, more than this host's %d",
                S.plugin_uri, S.n_ports, LV2_HOST_MAX_PORTS);
        fail_open();
        return 1;
    }
    S.n_audio_in = S.n_audio_out = 0;
    S.n_params = 0;
    S.latency_port = -1;
    memset(S.is_ctrl_in, 0, sizeof S.is_ctrl_in);

    /* Ranges for every port in one call (lilv's recommended way). */
    float *mins = (float *)calloc((size_t)S.n_ports, sizeof(float));
    float *maxs = (float *)calloc((size_t)S.n_ports, sizeof(float));
    float *defs = (float *)calloc((size_t)S.n_ports, sizeof(float));
    if (!mins || !maxs || !defs) {
        free(mins); free(maxs); free(defs);
        set_err("out of memory");
        fail_open();
        return 1;
    }
    lilv_plugin_get_port_ranges_float(p, mins, maxs, defs);

    typedef enum { K_AUDIO_IN, K_AUDIO_OUT, K_CTRL_IN, K_CTRL_OUT, K_NONE } kind_t;
    kind_t kind[LV2_HOST_MAX_PORTS];

    for (long k = 0; k < S.n_ports; k++) {
        const LilvPort *port = lilv_plugin_get_port_by_index(p, (uint32_t)k);
        const char *sym = node_str(lilv_port_get_symbol(p, port));
        int is_in  = port_is(p, port, LILV_URI_INPUT_PORT);
        int is_out = port_is(p, port, LILV_URI_OUTPUT_PORT);
        int audio  = port_is(p, port, LILV_URI_AUDIO_PORT);
        int ctrl   = port_is(p, port, LILV_URI_CONTROL_PORT);

        if (audio && is_in)       { kind[k] = K_AUDIO_IN;  S.n_audio_in++;  }
        else if (audio && is_out) { kind[k] = K_AUDIO_OUT; S.n_audio_out++; }
        else if (ctrl && is_in) {
            kind[k] = K_CTRL_IN;
            S.is_ctrl_in[k] = 1;
            /* Start every control at its default so an untouched port
             * is what the manifest says, not zero. */
            S.ctrl[k] = isnan(defs[k]) ? 0.0f : defs[k];
            if (S.n_params < LV2_HOST_MAX_PARAMS) {
                long j = S.n_params++;
                S.p_port[j] = k;
                S.p_min[j]  = mins[k];
                S.p_max[j]  = maxs[k];
                S.p_def[j]  = defs[k];
                LilvNode *nm = lilv_port_get_name(p, port);
                snprintf(S.p_name[j], sizeof S.p_name[0], "%s", node_str(nm));
                lilv_node_free(nm);
                snprintf(S.p_sym[j], sizeof S.p_sym[0], "%s", sym);
            }
        }
        else if (ctrl && is_out) { kind[k] = K_CTRL_OUT; S.ctrl[k] = 0.0f; }
        else if (port_has(p, port, LV2_CORE__connectionOptional)) { kind[k] = K_NONE; }
        else {
            /* Atom, CV, event, or something newer: a required port of a
             * class this host cannot feed. Say which. */
            const char *cls = port_is(p, port, LILV_URI_ATOM_PORT) ? "atom" :
                              port_is(p, port, LILV_URI_CV_PORT)   ? "CV"   :
                              port_is(p, port, LILV_URI_EVENT_PORT) ? "event" : "unknown-class";
            set_err("plugin '%s' port %ld ('%s') is a required %s port, which this host "
                    "cannot connect (only audio and control ports are supported)",
                    S.plugin_uri, k, sym, cls);
            free(mins); free(maxs); free(defs);
            fail_open();
            return 1;
        }
    }
    free(mins); free(maxs); free(defs);

    if (S.n_audio_out < ch) {
        set_err("plugin '%s' has %ld audio output port(s), fewer than the %ld channel(s) asked for",
                S.plugin_uri, S.n_audio_out, ch);
        fail_open();
        return 1;
    }
    if (lilv_plugin_has_latency(p))
        S.latency_port = (long)lilv_plugin_get_latency_port_index(p);

    S.inst = lilv_plugin_instantiate(p, sample_rate, FEATURES);
    if (!S.inst) {
        set_err("plugin '%s' refused to instantiate at %g Hz", S.plugin_uri, sample_rate);
        fail_open();
        return 1;
    }

    /* Connect everything once; the plugin re-reads the pointers on every
     * run(). Audio: k-th input port <- host channel min(k, ch-1); k-th
     * output port -> host channel k, or the sink when k >= ch. */
    long ai = 0, ao = 0;
    for (long k = 0; k < S.n_ports; k++) {
        switch (kind[k]) {
        case K_AUDIO_IN:
            lilv_instance_connect_port(S.inst, (uint32_t)k, S.in[ai < ch ? ai : ch - 1]);
            ai++;
            break;
        case K_AUDIO_OUT:
            lilv_instance_connect_port(S.inst, (uint32_t)k, ao < ch ? S.out[ao] : S.sink);
            ao++;
            break;
        case K_CTRL_IN:
        case K_CTRL_OUT:
            lilv_instance_connect_port(S.inst, (uint32_t)k, &S.ctrl[k]);
            break;
        case K_NONE:
            lilv_instance_connect_port(S.inst, (uint32_t)k, NULL);
            break;
        }
    }

    lilv_instance_activate(S.inst);

    S.sample_rate = sample_rate;
    S.block = blk;
    S.chan  = ch;
    memset(S.in, 0, sizeof S.in);
    memset(S.out, 0, sizeof S.out);
    S.in_token = S.out_token = 0;
    S.in_n = S.out_n = 0;
    S.n_process = 0;
    S.open = 1;
    return 0;
}

void lv2_host_close(void) {
    free_world();
    S.open = 0;
    S.n_params = 0;
    S.n_ports = 0;
    S.n_audio_in = S.n_audio_out = 0;
    S.latency_port = -1;
    S.in_token = S.out_token = 0;
    S.in_n = S.out_n = 0;
    S.n_process = 0;
    S.plugin_name[0] = '\0';
    S.plugin_uri[0] = '\0';
    /* The descriptor cache survives, as in clap_host.c: a scan that found
     * plugins is information a caller still wants after a failed open. */
}

const char *lv2_host_plugin_name(void) { return S.plugin_name; }
const char *lv2_host_plugin_uri(void)  { return S.plugin_uri; }

/* ---------------------------------------------------------------- *
 * 6. Parameter and configuration reporting
 * ---------------------------------------------------------------- */

long lv2_host_n_params(void) { return S.n_params; }

static int pidx(long i) { return (i >= 0 && i < S.n_params); }

double lv2_host_param_id(long i)        { return pidx(i) ? (double)S.p_port[i] : -1.0; }
double lv2_host_param_min(long i)       { return pidx(i) ? S.p_min[i] : NAN; }
double lv2_host_param_max(long i)       { return pidx(i) ? S.p_max[i] : NAN; }
double lv2_host_param_default(long i)   { return pidx(i) ? S.p_def[i] : NAN; }
const char *lv2_host_param_name(long i)   { return pidx(i) ? S.p_name[i] : ""; }
const char *lv2_host_param_symbol(long i) { return pidx(i) ? S.p_sym[i] : ""; }

double lv2_host_param_value(double port_index) {
    if (!S.open) return NAN;
    long k = (long)(port_index + 0.5);
    if (k < 0 || k >= S.n_ports || !S.is_ctrl_in[k]) return NAN;
    return (double)S.ctrl[k];
}

double lv2_host_latency(void) {
    if (!S.open || S.latency_port < 0) return 0.0;
    return (double)S.ctrl[S.latency_port];
}

double lv2_host_n_audio_in(void)  { return S.open ? (double)S.n_audio_in  : 0.0; }
double lv2_host_n_audio_out(void) { return S.open ? (double)S.n_audio_out : 0.0; }
double lv2_host_sample_rate(void) { return S.open ? S.sample_rate : 0.0; }
double lv2_host_block_size(void)  { return S.open ? (double)S.block : 0.0; }
double lv2_host_channels(void)    { return S.open ? (double)S.chan : 0.0; }
double lv2_host_is_open(void)     { return S.open ? 1.0 : 0.0; }
long   lv2_host_n_process(void)   { return S.n_process; }
void   lv2_host_reset_counters(void) { S.n_process = 0; }

/* ---------------------------------------------------------------- *
 * 7. The input block
 * ---------------------------------------------------------------- */

double lv2_in_fill(const double *samples, long n, long channels) {
    if (!S.open) { set_err("no plugin is open"); return NAN; }
    if (!samples || n < 0) { set_err("lv2_in_fill: no samples"); return NAN; }
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
    return (double)(++S.in_token);
}

static double wave_at(long k, int w, double freq, double amp) {
    double t = (double)k / S.sample_rate;
    switch (w) {
    case LV2_WAVE_SINE:    return amp * sin(2.0 * M_PI * freq * t);
    case LV2_WAVE_SQUARE:  return amp * (sin(2.0 * M_PI * freq * t) >= 0.0 ? 1.0 : -1.0);
    case LV2_WAVE_RAMP:    return amp * (2.0 * fmod(freq * t, 1.0) - 1.0);
    case LV2_WAVE_IMPULSE: return (k == 0) ? amp : 0.0;
    default:               return 0.0;
    }
}

double lv2_in_tone(double t, double waveform, double freq, double amp) {
    if (!S.open) { set_err("no plugin is open"); return NAN; }
    double end = t * S.sample_rate;
    long first = (long)(end + 0.5) - S.block;
    int w = (int)(waveform + 0.5);
    for (long i = 0; i < S.block; i++) {
        double v = wave_at(first + i, w, freq, amp);
        for (long c = 0; c < S.chan; c++) S.in[c][i] = (float)v;
    }
    S.in_n = S.block;
    return (double)(++S.in_token);
}

double lv2_in_sample(double dep, double i, double ch) {
    if (!S.open) return NAN;
    if ((long)(dep + 0.5) != S.in_token) return NAN;
    long k = (long)(i + 0.5), c = (long)(ch + 0.5);
    if (k < 0 || k >= S.in_n || c < 0 || c >= S.chan) return NAN;
    return (double)S.in[c][k];
}

/* ---------------------------------------------------------------- *
 * 8. run() -- the node-side operator
 * ---------------------------------------------------------------- */

double lv2_process(double dep,
                   double id0, double v0, double id1, double v1,
                   double id2, double v2, double id3, double v3) {
    if (!S.open) { set_err("no plugin is open"); return NAN; }
    if ((long)(dep + 0.5) != S.in_token || S.in_token == 0) return NAN;

    /* A control port is one float the plugin reads at run(): writing it
     * here is exactly "this value on this block". Only control INPUTS are
     * writable; anything else is ignored rather than corrupted. */
    const double ids[LV2_HOST_PARAM_SLOTS]  = { id0, id1, id2, id3 };
    const double vals[LV2_HOST_PARAM_SLOTS] = { v0,  v1,  v2,  v3  };
    for (int i = 0; i < LV2_HOST_PARAM_SLOTS; i++) {
        if (!(ids[i] >= 0.0) || isnan(vals[i])) continue;
        long k = (long)(ids[i] + 0.5);
        if (k < S.n_ports && S.is_ctrl_in[k]) S.ctrl[k] = (float)vals[i];
    }

    lilv_instance_run(S.inst, (uint32_t)S.block);
    S.n_process++;
    S.out_n = S.block;
    return (double)(++S.out_token);
}

/* ---------------------------------------------------------------- *
 * 9. The output block
 * ---------------------------------------------------------------- */

static int out_ok(double dep) {
    return S.open && S.out_token != 0 && (long)(dep + 0.5) == S.out_token;
}

double lv2_out_sample(double dep, double i, double ch) {
    if (!out_ok(dep)) return NAN;
    long k = (long)(i + 0.5), c = (long)(ch + 0.5);
    if (k < 0 || k >= S.out_n || c < 0 || c >= S.chan) return NAN;
    return (double)S.out[c][k];
}

double lv2_out_rms(double dep) {
    if (!out_ok(dep)) return NAN;
    if (S.out_n <= 0) return 0.0;
    double s = 0.0;
    for (long c = 0; c < S.chan; c++)
        for (long i = 0; i < S.out_n; i++) s += (double)S.out[c][i] * (double)S.out[c][i];
    return sqrt(s / (double)(S.out_n * S.chan));
}

double lv2_out_peak(double dep) {
    if (!out_ok(dep)) return NAN;
    double m = 0.0;
    for (long c = 0; c < S.chan; c++)
        for (long i = 0; i < S.out_n; i++) {
            double a = fabs((double)S.out[c][i]);
            if (a > m) m = a;
        }
    return m;
}

double lv2_out_count(double dep) { return out_ok(dep) ? (double)S.out_n : NAN; }
double lv2_out_valid(double dep) { return out_ok(dep) ? 1.0 : 0.0; }
