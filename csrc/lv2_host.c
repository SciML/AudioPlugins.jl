/* lv2_host.c -- see lv2_host.h. Allocation-free after open, like
 * clap_host.c: lilv allocates during scan and open, which is driver-side.
 */

#include "lv2_host.h"
/* Through the include path rather than by relative path, so that lilv.h
 * (which includes <lv2/core/lv2.h> itself) and this file resolve to the
 * same copy: build with -I csrc/vendor to use the vendored headers. */
#include <lv2/core/lv2.h>
#include <lv2/urid/urid.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/resize-port/resize-port.h>

#include <lilv/lilv.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 1. Error reporting ---- */

static char ERR[512];

static void set_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ERR, sizeof ERR, fmt, ap);
    va_end(ap);
}

const char *lv2_host_last_error(void) { return ERR; }

/* ---- 2. The host features we offer ----
 *
 * urid:map / urid:unmap because nearly every modern plugin requires
 * them; the two buf-size flags because they are true of this host. A
 * plugin that maps more than LV2_HOST_MAX_URIDS URIs gets 0 back, which
 * the spec allows for "cannot map".
 */

#define LV2_HOST_MAX_URIDS 1024

/* isSideChain postdates the vendored lv2.h, and port-groups ships no
 * header; the URIs are stable, so write them out. */
#define LV2_CORE__isSideChain  LV2_CORE_PREFIX "isSideChain"
#define LV2_PG_PREFIX          "http://lv2plug.in/ns/ext/port-groups#"
#define LV2_PG__group          LV2_PG_PREFIX "group"
#define LV2_PG__mainInput      LV2_PG_PREFIX "mainInput"
#define LV2_PG__mainOutput     LV2_PG_PREFIX "mainOutput"
#define LV2_PG__sideChainOf    LV2_PG_PREFIX "sideChainOf"

typedef struct {
    char *uris[LV2_HOST_MAX_URIDS];
    uint32_t count;
} urids_t;

static LV2_URID urid_map(LV2_URID_Map_Handle h, const char *uri) {
    urids_t *map = (urids_t *)h;
    if (!uri) return 0;
    for (uint32_t i = 0; i < map->count; i++)
        if (strcmp(map->uris[i], uri) == 0) return i + 1;
    if (map->count >= LV2_HOST_MAX_URIDS) return 0;
    map->uris[map->count] = strdup(uri);
    if (!map->uris[map->count]) return 0;
    return ++map->count;
}

static const char *urid_unmap(LV2_URID_Unmap_Handle h, LV2_URID urid) {
    urids_t *map = (urids_t *)h;
    return (urid >= 1 && urid <= map->count) ? map->uris[urid - 1] : NULL;
}

static const LV2_Feature F_FIXED   = { LV2_BUF_SIZE__fixedBlockLength, NULL };
static const LV2_Feature F_BOUNDED = { LV2_BUF_SIZE__boundedBlockLength, NULL };

static int feature_supported(const char *uri) {
    return !strcmp(uri, LV2_URID__map) || !strcmp(uri, LV2_URID__unmap) ||
           !strcmp(uri, LV2_BUF_SIZE__fixedBlockLength) || !strcmp(uri, LV2_BUF_SIZE__boundedBlockLength);
}

/* ---- 3. State ---- */

/* Copied, not pointed at: lilv's nodes belong to the world the next scan
 * frees. */
typedef struct {
    char uri[512];
    char name[256];
} desc_t;

typedef struct state {
    urids_t urids;
    LV2_URID_Map map;
    LV2_URID_Unmap unmap;
    LV2_Feature f_map, f_unmap;
    const LV2_Feature *features[5];
    LilvWorld        *world;
    const LilvPlugins *plugins;      /* owned by the world */
    const LilvPlugin *plugin;        /* owned by the world */
    LilvInstance     *inst;

    struct state *next;
    double handle;
    long open_index;
    double pending[LV2_HOST_MAX_PORTS];
    unsigned char queued[LV2_HOST_MAX_PORTS];
    int    open;
    double sample_rate;
    long   block, chan;

    /* Descriptor cache from the last scan; allocated at scan time, never
     * in the processing path. */
    long    n_desc;
    long    cap_desc;
    desc_t *desc;
    char   plugin_name[256];
    char   plugin_uri[512];
    char   lv2_path[2048];           /* the path the world was loaded with */

    long   n_ports;
    long   n_audio_in, n_audio_out;
    long   latency_port;             /* -1 when the plugin reports none */

    long   n_params;                    /* parameters = control input ports */
    long   p_port[LV2_HOST_MAX_PARAMS];
    double p_min[LV2_HOST_MAX_PARAMS], p_max[LV2_HOST_MAX_PARAMS], p_def[LV2_HOST_MAX_PARAMS];
    char   p_name[LV2_HOST_MAX_PARAMS][128];
    char   p_sym[LV2_HOST_MAX_PARAMS][128];

    /* One float per port: what a control port is connected to. Audio ports
     * are connected to the block buffers below, so their entry is unused. */
    float  ctrl[LV2_HOST_MAX_PORTS];
    signed char is_ctrl_in[LV2_HOST_MAX_PORTS];
    signed char is_ctrl_out[LV2_HOST_MAX_PORTS];

    /* Each atom:AtomPort gets one buffer carved out of `atom_arena`, a
     * single allocation made at open and freed at close. Slots are
     * numbered in port order; `atom_slot` maps a port to its slot. */
    long    n_atom;
    long    atom_port[LV2_HOST_MAX_ATOM_PORTS];    /* port index of slot i */
    long    atom_slot[LV2_HOST_MAX_PORTS];         /* port index -> slot */
    size_t  atom_off[LV2_HOST_MAX_ATOM_PORTS];     /* byte offset in arena */
    size_t  atom_cap[LV2_HOST_MAX_ATOM_PORTS];     /* buffer bytes per slot */
    long    atom_nev[LV2_HOST_MAX_ATOM_PORTS];     /* events queued (input) */
    int64_t atom_last[LV2_HOST_MAX_ATOM_PORTS];    /* last queued frame */
    signed char atom_in[LV2_HOST_MAX_ATOM_PORTS];  /* 1 input, 0 output */
    signed char atom_midi[LV2_HOST_MAX_ATOM_PORTS];/* 1 declared midi:MidiEvent */
    uint8_t *atom_arena;
    LV2_URID u_sequence, u_frametime, u_midievent, u_chunk;

    float  in[LV2_HOST_MAX_CHAN][LV2_HOST_MAX_BLOCK];
    float  out[LV2_HOST_MAX_CHAN][LV2_HOST_MAX_BLOCK];
    float  sink[LV2_HOST_MAX_BLOCK];   /* outputs beyond `chan` go here */
    float  side_in[LV2_HOST_MAX_BLOCK];/* non-main audio inputs read silence */

    double in_token, out_token;
    int    in_armed;    /* 1 between fill/tone and the process that consumes it */
    long   in_n, out_n;
    long   n_process;
    int    midi_bad;    /* a refused MIDI event poisons the armed block */
} state_t;

static state_t DEFAULT_STATE;
static state_t *INSTANCES;
static double NEXT_HANDLE, NEXT_TOKEN;

static state_t *instance(double handle) {
    for (state_t *st = INSTANCES; st; st = st->next)
        if (st->handle == handle) return st;
    set_err("invalid or closed LV2 instance %g", handle);
    return NULL;
}
static double next_token(state_t *st, double previous) {
    if (!st->handle) return previous + 1;
    if (NEXT_TOKEN >= 9007199254740991.0) return NAN;
    return -(++NEXT_TOKEN);
}
static int token_matches(state_t *st, double dep, double token) {
    if (!isfinite(dep) || token == 0) return 0;
    return st->handle ? dep == token : (dep >= 0 && dep < 9007199254740991.0 && floor(dep + 0.5) == token);
}
static double lv2_out_valid_ctx(state_t *st, double dep);
static double lv2_out_count_ctx(state_t *st, double dep);
static double lv2_out_peak_ctx(state_t *st, double dep);
static double lv2_out_rms_ctx(state_t *st, double dep);
static double lv2_out_sample_ctx(state_t *st, double dep, double i, double ch);
static double lv2_process_ctx(state_t *st, double dep,
                   double id0, double v0, double id1, double v1,
                   double id2, double v2, double id3, double v3);
static double lv2_in_midi_ctx(state_t *st, double dep, double port, double frame,
                   double b0, double b1, double b2);
static double lv2_in_sample_ctx(state_t *st, double dep, double i, double ch);
static double lv2_in_tone_ctx(state_t *st, double t, double waveform, double freq, double amp);
static double lv2_in_fill_ctx(state_t *st, const double *samples, long n, long channels);
static void   lv2_host_reset_counters_ctx(state_t *st);
static long   lv2_host_n_process_ctx(state_t *st);
static double lv2_host_is_open_ctx(state_t *st);
static double lv2_host_channels_ctx(state_t *st);
static double lv2_host_block_size_ctx(state_t *st);
static double lv2_host_sample_rate_ctx(state_t *st);
static double lv2_host_n_audio_out_ctx(state_t *st);
static double lv2_host_n_audio_in_ctx(state_t *st);
static double lv2_host_latency_ctx(state_t *st);
static double lv2_host_atom_port_size_ctx(state_t *st, double i);
static double lv2_host_atom_port_midi_ctx(state_t *st, double i);
static double lv2_host_atom_port_is_input_ctx(state_t *st, double i);
static double lv2_host_atom_port_index_ctx(state_t *st, double i);
static double lv2_host_n_atom_ports_ctx(state_t *st);
static double lv2_host_port_value_ctx(state_t *st, double port_index);
static double lv2_host_param_value_ctx(state_t *st, double port_index);
static const char *lv2_host_param_symbol_ctx(state_t *st, long i);
static const char *lv2_host_param_name_ctx(state_t *st, long i);
static double lv2_host_param_default_ctx(state_t *st, long i);
static double lv2_host_param_max_ctx(state_t *st, long i);
static double lv2_host_param_min_ctx(state_t *st, long i);
static double lv2_host_param_id_ctx(state_t *st, long i);
static long lv2_host_n_params_ctx(state_t *st);
static const char *lv2_host_plugin_uri_ctx(state_t *st);
static const char *lv2_host_plugin_name_ctx(state_t *st);
static void lv2_host_close_ctx(state_t *st);
static int lv2_host_open_ctx(state_t *st, const char *lv2_path, const char *uri,
                  double sample_rate, double block_size, double channels);
static const char *lv2_host_scan_name_ctx(state_t *st, long i);
static const char *lv2_host_scan_uri_ctx(state_t *st, long i);
static long lv2_host_scan_ctx(state_t *st, const char *lv2_path);
/* ---- 4. Discovery ---- */

static void free_world_ctx(state_t *st) {
    if (st->inst) {
        if (st->open) lilv_instance_deactivate(st->inst);
        lilv_instance_free(st->inst);
        st->inst = NULL;
    }
    free(st->atom_arena);
    st->atom_arena = NULL;
    st->plugin = NULL;
    st->plugins = NULL;
    if (st->world) lilv_world_free(st->world);
    st->world = NULL;
}

static const char *node_str(const LilvNode *n) {
    return n ? lilv_node_as_string(n) : "";
}

/* The point past which an enumeration is a runaway (the cache itself grows
 * to fit). */
#define LV2_HOST_SCAN_SANITY 65536

static int grow_desc_ctx(state_t *st, long n) {
    if (n <= st->cap_desc) return 1;
    long cap = st->cap_desc ? st->cap_desc : 64;
    while (cap < n) cap *= 2;
    desc_t *p = (desc_t *)realloc(st->desc, (size_t)cap * sizeof(desc_t));
    if (!p) return 0;
    st->desc = p;
    st->cap_desc = cap;
    return 1;
}

static long lv2_host_scan_ctx(state_t *st, const char *lv2_path) {
    ERR[0] = '\0';
    lv2_host_close_ctx(st);
    st->n_desc = 0;   /* a failed scan leaves 0, not the previous path's count */

    st->world = lilv_world_new();
    if (!st->world) { set_err("lilv_world_new failed"); return -1; }

    if (lv2_path && lv2_path[0]) {
        LilvNode *p = lilv_new_string(st->world, lv2_path);
        lilv_world_set_option(st->world, LILV_OPTION_LV2_PATH, p);
        lilv_node_free(p);
        snprintf(st->lv2_path, sizeof st->lv2_path, "%s", lv2_path);
    } else {
        st->lv2_path[0] = '\0';
    }
    const char *where = st->lv2_path[0] ? st->lv2_path : "(default LV2_PATH)";
    lilv_world_load_all(st->world);
    st->plugins = lilv_world_get_all_plugins(st->world);

    LILV_FOREACH (plugins, i, st->plugins) {
        if (st->n_desc >= LV2_HOST_SCAN_SANITY) {
            set_err("more than %d LV2 plugins under '%s'; refusing to enumerate further",
                    LV2_HOST_SCAN_SANITY, where);
            free_world_ctx(st);
            st->n_desc = 0;
            return -1;
        }
        if (!grow_desc_ctx(st, st->n_desc + 1)) {
            set_err("out of memory caching %ld descriptors from '%s'", st->n_desc + 1, where);
            free_world_ctx(st);
            st->n_desc = 0;
            return -1;
        }
        const LilvPlugin *p = lilv_plugins_get(st->plugins, i);
        snprintf(st->desc[st->n_desc].uri, sizeof st->desc[0].uri, "%s",
                 node_str(lilv_plugin_get_uri(p)));
        LilvNode *name = lilv_plugin_get_name(p);
        snprintf(st->desc[st->n_desc].name, sizeof st->desc[0].name, "%s", node_str(name));
        lilv_node_free(name);
        st->n_desc++;
    }
    if (st->n_desc == 0) {
        set_err("no LV2 plugins found under '%s'", where);
        free_world_ctx(st);
        return -1;
    }
    return st->n_desc;
}

static const char *lv2_host_scan_uri_ctx(state_t *st, long i) { return (i >= 0 && i < st->n_desc) ? st->desc[i].uri  : ""; }
static const char *lv2_host_scan_name_ctx(state_t *st, long i) { return (i >= 0 && i < st->n_desc) ? st->desc[i].name : ""; }

/* ---- 5. Open / close ---- */

static int port_is_ctx(state_t *st, const LilvPlugin *p, const LilvPort *port, const char *cls) {
    LilvNode *n = lilv_new_uri(st->world, cls);
    int r = lilv_port_is_a(p, port, n);
    lilv_node_free(n);
    return r;
}

static int port_has_ctx(state_t *st, const LilvPlugin *p, const LilvPort *port, const char *prop) {
    LilvNode *n = lilv_new_uri(st->world, prop);
    int r = lilv_port_has_property(p, port, n);
    lilv_node_free(n);
    return r;
}

/* Connectable when atom:Sequence is among the port's atom:bufferType
 * values (or it declares none, Sequence being the default). `other`
 * points at a static copy of the first non-Sequence value, for the error
 * message when none fit. */
static void atom_buffer_type_ctx(state_t *st, const LilvPlugin *p, const LilvPort *port,
                             int *seq_ok, const char **other) {
    static char other_uri[512];
    LilvNode *pred = lilv_new_uri(st->world, LV2_ATOM__bufferType);
    LilvNodes *vals = lilv_port_get_value(p, port, pred);
    lilv_node_free(pred);
    int saw_seq = 0, any = 0;
    *other = NULL;
    LILV_FOREACH (nodes, i, vals) {
        const char *v = node_str(lilv_nodes_get(vals, i));
        any = 1;
        if (strcmp(v, LV2_ATOM__Sequence) == 0) saw_seq = 1;
        else if (!*other) {
            snprintf(other_uri, sizeof other_uri, "%s", v);
            *other = other_uri;
        }
    }
    lilv_nodes_free(vals);
    *seq_ok = saw_seq || !any;
}

/* Declared rsz:minimumSize in bytes, or 0 when undeclared. A double so an
 * overflowing value is refused, not truncated. */
static double atom_min_size_ctx(state_t *st, const LilvPlugin *p, const LilvPort *port) {
    LilvNode *pred = lilv_new_uri(st->world, LV2_RESIZE_PORT__minimumSize);
    LilvNodes *vals = lilv_port_get_value(p, port, pred);
    lilv_node_free(pred);
    double r = 0.0;
    if (vals) {
        const LilvNode *v = lilv_nodes_get_first(vals);
        if (v) r = lilv_node_is_int(v) ? (double)lilv_node_as_int(v)
                                      : lilv_node_as_float(v);
        lilv_nodes_free(vals);
    }
    return r;
}

/* 1 when an audio port is on the plugin's main signal path. A port
 * marked lv2:isSideChain, or carrying (or grouped under) pg:sideChainOf,
 * is not main. When the plugin names a pg:mainInput / pg:mainOutput
 * group, a port is main exactly when its pg:group is that group. With
 * no sidechain or port-group information a port is main. */
static int port_main_ctx(state_t *st, const LilvPlugin *p, const LilvPort *port, int is_in) {
    if (port_has_ctx(st, p, port, LV2_CORE__isSideChain)) return 0;
    LilvNode *pred = lilv_new_uri(st->world, LV2_PG__sideChainOf);
    LilvNodes *sc = lilv_port_get_value(p, port, pred);
    lilv_node_free(pred);
    if (sc) { lilv_nodes_free(sc); return 0; }

    pred = lilv_new_uri(st->world, is_in ? LV2_PG__mainInput : LV2_PG__mainOutput);
    LilvNodes *mains =
        lilv_world_get(st->world, lilv_plugin_get_uri(p), pred, NULL);
    lilv_node_free(pred);
    if (!mains) return 1;

    int main = 0;
    pred = lilv_new_uri(st->world, LV2_PG__group);
    LilvNodes *groups = lilv_port_get_value(p, port, pred);
    lilv_node_free(pred);
    LILV_FOREACH (nodes, i, groups) {
        const LilvNode *g = lilv_nodes_get(groups, i);
        pred = lilv_new_uri(st->world, LV2_PG__sideChainOf);
        LilvNodes *gsc = lilv_world_get(st->world, g, pred, NULL);
        lilv_node_free(pred);
        int gside = gsc != NULL;
        lilv_nodes_free(gsc);
        if (gside) continue;
        LILV_FOREACH (nodes, j, mains)
            if (lilv_node_equals(lilv_nodes_get(mains, j), g)) main = 1;
    }
    lilv_nodes_free(groups);
    lilv_nodes_free(mains);
    return main;
}

/* MIDI only when atom:supports names midi:MidiEvent, as jalv does. */
static int atom_midi_ok_ctx(state_t *st, const LilvPlugin *p, const LilvPort *port) {
    LilvNode *pred = lilv_new_uri(st->world, LV2_ATOM__supports);
    LilvNodes *vals = lilv_port_get_value(p, port, pred);
    lilv_node_free(pred);
    int midi = 0;
    LILV_FOREACH (nodes, i, vals)
        if (strcmp(node_str(lilv_nodes_get(vals, i)), LV2_MIDI__MidiEvent) == 0)
            midi = 1;
    lilv_nodes_free(vals);
    return midi;
}

static void fail_open_ctx(state_t *st) { lv2_host_close_ctx(st); }
static void head_atom_seq_ctx(state_t *st, long s);
static void head_atom_out_ctx(state_t *st, long s);

static int lv2_host_open_ctx(state_t *st, const char *lv2_path, const char *uri,
                  double sample_rate, double block_size, double channels) {
    long n = lv2_host_scan_ctx(st, lv2_path);      /* also clears state and sets ERR */
    if (n < 0) return 1;

    if (!isfinite(block_size) || !isfinite(channels)) {
        set_err("block_size and channels must be finite, got %g and %g",
                block_size, channels);
        fail_open_ctx(st);
        return 1;
    }
    long blk = (long)(block_size + 0.5);
    long ch  = (long)(channels + 0.5);
    if (blk < 1 || blk > LV2_HOST_MAX_BLOCK) {
        set_err("block_size %ld out of range 1..%d", blk, LV2_HOST_MAX_BLOCK);
        fail_open_ctx(st);
        return 1;
    }
    if (ch < 1 || ch > LV2_HOST_MAX_CHAN) {
        set_err("channels %ld out of range 1..%d", ch, LV2_HOST_MAX_CHAN);
        fail_open_ctx(st);
        return 1;
    }
    if (!(sample_rate > 0.0)) {
        set_err("sample_rate must be positive, got %g", sample_rate);
        fail_open_ctx(st);
        return 1;
    }

    const char *want = (uri && uri[0]) ? uri : st->desc[0].uri;
    LilvNode *want_node = lilv_new_uri(st->world, want);
    const LilvPlugin *p = want_node ? lilv_plugins_get_by_uri(st->plugins, want_node) : NULL;
    lilv_node_free(want_node);
    if (!p) {
        set_err("no plugin with URI '%s' under '%s' (found %ld: first is '%s')",
                want, st->lv2_path[0] ? st->lv2_path : "(default LV2_PATH)",
                st->n_desc, st->desc[0].uri);
        fail_open_ctx(st);
        return 1;
    }
    st->plugin = p;
    snprintf(st->plugin_uri, sizeof st->plugin_uri, "%s", node_str(lilv_plugin_get_uri(p)));
    {
        LilvNode *name = lilv_plugin_get_name(p);
        snprintf(st->plugin_name, sizeof st->plugin_name, "%s", node_str(name));
        lilv_node_free(name);
    }

    LilvNodes *req = lilv_plugin_get_required_features(p);
    LILV_FOREACH (nodes, i, req) {
        const char *f = node_str(lilv_nodes_get(req, i));
        if (!feature_supported(f)) {
            set_err("plugin '%s' requires host feature <%s>, which this host does not provide",
                    st->plugin_uri, f);
            lilv_nodes_free(req);
            fail_open_ctx(st);
            return 1;
        }
    }
    lilv_nodes_free(req);

    st->n_ports = (long)lilv_plugin_get_num_ports(p);
    if (st->n_ports > LV2_HOST_MAX_PORTS) {
        set_err("plugin '%s' has %ld ports, more than this host's %d",
                st->plugin_uri, st->n_ports, LV2_HOST_MAX_PORTS);
        fail_open_ctx(st);
        return 1;
    }
    st->n_audio_in = st->n_audio_out = 0;
    st->n_params = 0;
    st->latency_port = -1;
    memset(st->is_ctrl_in, 0, sizeof st->is_ctrl_in);
    memset(st->is_ctrl_out, 0, sizeof st->is_ctrl_out);
    st->n_atom = 0;
    memset(st->atom_slot, -1, sizeof st->atom_slot);

    float *mins = (float *)calloc((size_t)st->n_ports, sizeof(float));
    float *maxs = (float *)calloc((size_t)st->n_ports, sizeof(float));
    float *defs = (float *)calloc((size_t)st->n_ports, sizeof(float));
    if (!mins || !maxs || !defs) {
        free(mins); free(maxs); free(defs);
        set_err("out of memory");
        fail_open_ctx(st);
        return 1;
    }
    lilv_plugin_get_port_ranges_float(p, mins, maxs, defs);

    typedef enum {
        K_AUDIO_IN, K_AUDIO_OUT, K_AUDIO_IN_SIDE, K_AUDIO_OUT_SIDE,
        K_CTRL_IN, K_CTRL_OUT, K_ATOM_IN, K_ATOM_OUT, K_NONE
    } kind_t;
    kind_t kind[LV2_HOST_MAX_PORTS];
    size_t arena_bytes = 0;

    for (long k = 0; k < st->n_ports; k++) {
        const LilvPort *port = lilv_plugin_get_port_by_index(p, (uint32_t)k);
        const char *sym = node_str(lilv_port_get_symbol(p, port));
        int is_in  = port_is_ctx(st, p, port, LILV_URI_INPUT_PORT);
        int is_out = port_is_ctx(st, p, port, LILV_URI_OUTPUT_PORT);
        int audio  = port_is_ctx(st, p, port, LILV_URI_AUDIO_PORT);
        int ctrl   = port_is_ctx(st, p, port, LILV_URI_CONTROL_PORT);
        int atom   = port_is_ctx(st, p, port, LILV_URI_ATOM_PORT);

        if (audio && is_in) {
            if (port_main_ctx(st, p, port, 1)) { kind[k] = K_AUDIO_IN;      st->n_audio_in++; }
            else                       { kind[k] = K_AUDIO_IN_SIDE; }
        }
        else if (audio && is_out) {
            if (port_main_ctx(st, p, port, 0)) { kind[k] = K_AUDIO_OUT;      st->n_audio_out++; }
            else                       { kind[k] = K_AUDIO_OUT_SIDE; }
        }
        else if (ctrl && is_in) {
            kind[k] = K_CTRL_IN;
            st->is_ctrl_in[k] = 1;
            /* An untouched control port is its declared default. */
            st->ctrl[k] = isnan(defs[k]) ? 0.0f : defs[k];
            if (st->n_params < LV2_HOST_MAX_PARAMS) {
                long j = st->n_params++;
                st->p_port[j] = k;
                st->p_min[j]  = mins[k];
                st->p_max[j]  = maxs[k];
                st->p_def[j]  = defs[k];
                LilvNode *nm = lilv_port_get_name(p, port);
                snprintf(st->p_name[j], sizeof st->p_name[0], "%s", node_str(nm));
                lilv_node_free(nm);
                snprintf(st->p_sym[j], sizeof st->p_sym[0], "%s", sym);
            }
        }
        else if (ctrl && is_out) { kind[k] = K_CTRL_OUT; st->is_ctrl_out[k] = 1; st->ctrl[k] = 0.0f; }
        else if (atom && (is_in || is_out)) {
            /* A declared bufferType other than atom:Sequence is only an
             * error when the port is required -- then it is named. */
            int seq_ok; const char *other;
            atom_buffer_type_ctx(st, p, port, &seq_ok, &other);
            if (!seq_ok) {
                if (port_has_ctx(st, p, port, LV2_CORE__connectionOptional)) { kind[k] = K_NONE; }
                else {
                    set_err("plugin '%s' port %ld ('%s') is a required atom port with "
                            "bufferType <%s>, which this host cannot connect "
                            "(only atom:Sequence is supported)",
                            st->plugin_uri, k, sym, other);
                    free(mins); free(maxs); free(defs);
                    fail_open_ctx(st);
                    return 1;
                }
            } else if (st->n_atom >= LV2_HOST_MAX_ATOM_PORTS) {
                set_err("plugin '%s' has more than %d atom ports, which this host "
                        "cannot connect", st->plugin_uri, LV2_HOST_MAX_ATOM_PORTS);
                free(mins); free(maxs); free(defs);
                fail_open_ctx(st);
                return 1;
            } else {
                double msz = atom_min_size_ctx(st, p, port);
                if (msz > (double)UINT32_MAX) {
                    set_err("plugin '%s' port %ld ('%s') declares "
                            "rsz:minimumSize %.0f, more than %lu bytes",
                            st->plugin_uri, k, sym, msz, (unsigned long)UINT32_MAX);
                    free(mins); free(maxs); free(defs);
                    fail_open_ctx(st);
                    return 1;
                }
                long s = st->n_atom++;
                kind[k] = is_in ? K_ATOM_IN : K_ATOM_OUT;
                st->atom_slot[k] = s;
                st->atom_port[s] = k;
                st->atom_in[s]   = is_in;
                st->atom_midi[s] = (signed char)atom_midi_ok_ctx(st, p, port);
                st->atom_cap[s]  = (size_t)(msz > LV2_HOST_ATOM_BUF ? msz : LV2_HOST_ATOM_BUF);
                st->atom_cap[s]  = (st->atom_cap[s] + 7u) & ~(size_t)7u;
                st->atom_off[s]  = arena_bytes;
                arena_bytes   += st->atom_cap[s];
                if (arena_bytes > LV2_HOST_ATOM_ARENA_MAX) {
                    set_err("plugin '%s' asks for more than %u bytes of atom "
                            "buffers, which this host will not allocate",
                            st->plugin_uri, (unsigned)LV2_HOST_ATOM_ARENA_MAX);
                    free(mins); free(maxs); free(defs);
                    fail_open_ctx(st);
                    return 1;
                }
                st->atom_nev[s]  = 0;
                st->atom_last[s] = -1;
            }
        }
        else if (port_has_ctx(st, p, port, LV2_CORE__connectionOptional)) { kind[k] = K_NONE; }
        else {
            /* CV, event, or something newer: a required port of a class
             * this host cannot feed. Say which. */
            const char *cls = port_is_ctx(st, p, port, LILV_URI_CV_PORT)     ? "CV"    :
                              port_is_ctx(st, p, port, LILV_URI_EVENT_PORT)  ? "event" :
                              port_is_ctx(st, p, port, LILV_URI_ATOM_PORT)   ? "atom"  : "unknown-class";
            set_err("plugin '%s' port %ld ('%s') is a required %s port, which this host "
                    "cannot connect", st->plugin_uri, k, sym, cls);
            free(mins); free(maxs); free(defs);
            fail_open_ctx(st);
            return 1;
        }
    }
    free(mins); free(maxs); free(defs);
    /* More host channels than main audio inputs would silently drop a
     * channel. */
    if (st->n_audio_in > 0 && st->n_audio_in < ch) {
        set_err("plugin '%s' has %ld main audio input port%s but the host was asked "
                "for %ld channels; host channel %ld would feed no port",
                st->plugin_uri, st->n_audio_in, st->n_audio_in == 1 ? "" : "s",
                ch, st->n_audio_in);
        fail_open_ctx(st);
        return 1;
    }
    if (lilv_plugin_has_latency(p))
        st->latency_port = (long)lilv_plugin_get_latency_port_index(p);

    if (st->n_atom > 0) {
        st->atom_arena = (uint8_t *)malloc(arena_bytes);
        if (!st->atom_arena) {
            set_err("out of memory allocating %zu bytes of atom buffers for '%s'",
                    arena_bytes, st->plugin_uri);
            fail_open_ctx(st);
            return 1;
        }
        /* The atom URIDs the host itself stamps on buffers and events;
         * 0 means the map table is full. */
        st->u_sequence  = urid_map(&st->urids, LV2_ATOM__Sequence);
        st->u_frametime = urid_map(&st->urids, LV2_ATOM__frameTime);
        st->u_midievent = urid_map(&st->urids, LV2_MIDI__MidiEvent);
        st->u_chunk     = urid_map(&st->urids, LV2_ATOM__Chunk);
        if (!st->u_sequence || !st->u_frametime || !st->u_midievent || !st->u_chunk) {
            set_err("URID map table full while mapping atom types for '%s'", st->plugin_uri);
            fail_open_ctx(st);
            return 1;
        }
    }

    st->map.handle = &st->urids; st->map.map = urid_map;
    st->unmap.handle = &st->urids; st->unmap.unmap = urid_unmap;
    st->f_map.URI = LV2_URID__map; st->f_map.data = &st->map;
    st->f_unmap.URI = LV2_URID__unmap; st->f_unmap.data = &st->unmap;
    st->features[0] = &st->f_map; st->features[1] = &st->f_unmap;
    st->features[2] = &F_FIXED; st->features[3] = &F_BOUNDED; st->features[4] = NULL;
    st->inst = lilv_plugin_instantiate(p, sample_rate, st->features);
    if (!st->inst) {
        set_err("plugin '%s' refused to instantiate at %g Hz", st->plugin_uri, sample_rate);
        fail_open_ctx(st);
        return 1;
    }

    /* Audio: k-th main input port <- host channel min(k, ch-1); k-th
     * main output port -> host channel k, or the sink when k >= ch.
     * Non-main inputs read a zeroed buffer, non-main outputs the sink. */
    memset(st->side_in, 0, sizeof st->side_in);
    long ai = 0, ao = 0;
    for (long k = 0; k < st->n_ports; k++) {
        switch (kind[k]) {
        case K_AUDIO_IN:
            lilv_instance_connect_port(st->inst, (uint32_t)k, st->in[ai < ch ? ai : ch - 1]);
            ai++;
            break;
        case K_AUDIO_IN_SIDE:
            lilv_instance_connect_port(st->inst, (uint32_t)k, st->side_in);
            break;
        case K_AUDIO_OUT:
            lilv_instance_connect_port(st->inst, (uint32_t)k, ao < ch ? st->out[ao] : st->sink);
            ao++;
            break;
        case K_AUDIO_OUT_SIDE:
            lilv_instance_connect_port(st->inst, (uint32_t)k, st->sink);
            break;
        case K_CTRL_IN:
        case K_CTRL_OUT:
            lilv_instance_connect_port(st->inst, (uint32_t)k, &st->ctrl[k]);
            break;
        case K_ATOM_IN:
        case K_ATOM_OUT: {
            long s = st->atom_slot[k];
            lilv_instance_connect_port(st->inst, (uint32_t)k,
                                       st->atom_arena + st->atom_off[s]);
            if (st->atom_in[s]) head_atom_seq_ctx(st, s);
            else              head_atom_out_ctx(st, s);
            break;
        }
        case K_NONE:
            lilv_instance_connect_port(st->inst, (uint32_t)k, NULL);
            break;
        }
    }

    lilv_instance_activate(st->inst);

    st->sample_rate = sample_rate;
    st->block = blk;
    st->chan  = ch;
    memset(st->in, 0, sizeof st->in);
    memset(st->out, 0, sizeof st->out);
    st->in_token = st->out_token = 0;
    st->in_n = st->out_n = 0;
    st->n_process = 0;
    st->open_index = -1;
    for (long i = 0; i < st->n_desc; i++)
        if (!strcmp(st->desc[i].uri, st->plugin_uri)) st->open_index = i;
    st->open = 1;
    return 0;
}

static void lv2_host_close_ctx(state_t *st) {
    free_world_ctx(st);
    for (uint32_t i = 0; i < st->urids.count; i++) free(st->urids.uris[i]);
    st->urids.count = 0;
    memset(st->queued, 0, sizeof st->queued);
    st->in_armed = 0;
    st->open_index = -1;
    st->open = 0;
    st->n_params = 0;
    st->n_ports = 0;
    st->n_audio_in = st->n_audio_out = 0;
    st->latency_port = -1;
    st->n_atom = 0;
    memset(st->atom_slot, -1, sizeof st->atom_slot);
    st->u_sequence = st->u_frametime = st->u_midievent = st->u_chunk = 0;
    st->in_token = st->out_token = 0;
    st->in_armed = 0;
    st->in_n = st->out_n = 0;
    st->n_process = 0;
    st->midi_bad = 0;
    st->plugin_name[0] = '\0';
    st->plugin_uri[0] = '\0';
    /* The descriptor cache survives a failed open, as in clap_host.c. */
}

static const char *lv2_host_plugin_name_ctx(state_t *st) { return st->plugin_name; }
static const char *lv2_host_plugin_uri_ctx(state_t *st) { return st->plugin_uri; }

/* ---- 6. Parameter and configuration reporting ---- */

static long lv2_host_n_params_ctx(state_t *st) { return st->n_params; }

static int pidx_ctx(state_t *st, long i) { return (i >= 0 && i < st->n_params); }

static double lv2_host_param_id_ctx(state_t *st, long i) { return pidx_ctx(st, i) ? (double)st->p_port[i] : -1.0; }
static double lv2_host_param_min_ctx(state_t *st, long i) { return pidx_ctx(st, i) ? st->p_min[i] : NAN; }
static double lv2_host_param_max_ctx(state_t *st, long i) { return pidx_ctx(st, i) ? st->p_max[i] : NAN; }
static double lv2_host_param_default_ctx(state_t *st, long i) { return pidx_ctx(st, i) ? st->p_def[i] : NAN; }
static const char *lv2_host_param_name_ctx(state_t *st, long i) { return pidx_ctx(st, i) ? st->p_name[i] : ""; }
static const char *lv2_host_param_symbol_ctx(state_t *st, long i) { return pidx_ctx(st, i) ? st->p_sym[i] : ""; }

static double lv2_host_param_value_ctx(state_t *st, double port_index) {
    if (!st->open || !isfinite(port_index)) return NAN;
    long k = (long)(port_index + 0.5);
    if (k < 0 || k >= st->n_ports || !st->is_ctrl_in[k]) return NAN;
    return (double)st->ctrl[k];
}

static double lv2_host_port_value_ctx(state_t *st, double port_index) {
    if (!st->open || !isfinite(port_index)) return NAN;
    long k = (long)(port_index + 0.5);
    if (k < 0 || k >= st->n_ports || (!st->is_ctrl_in[k] && !st->is_ctrl_out[k])) return NAN;
    return (double)st->ctrl[k];
}

static int aidx_ctx(state_t *st, long i) { return (i >= 0 && i < st->n_atom); }

static double lv2_host_n_atom_ports_ctx(state_t *st) { return st->open ? (double)st->n_atom : 0.0; }

static int aslot_ctx(state_t *st, double i, long *s) {
    if (!st->open || !isfinite(i)) return 0;
    *s = (long)(i + 0.5);
    return aidx_ctx(st, *s);
}

static double lv2_host_atom_port_index_ctx(state_t *st, double i) {
    long s;
    return aslot_ctx(st, i, &s) ? (double)st->atom_port[s] : NAN;
}

static double lv2_host_atom_port_is_input_ctx(state_t *st, double i) {
    long s;
    return aslot_ctx(st, i, &s) ? (double)st->atom_in[s] : NAN;
}

static double lv2_host_atom_port_midi_ctx(state_t *st, double i) {
    long s;
    return aslot_ctx(st, i, &s) ? (double)st->atom_midi[s] : NAN;
}

static double lv2_host_atom_port_size_ctx(state_t *st, double i) {
    long s;
    return aslot_ctx(st, i, &s) ? (double)st->atom_cap[s] : NAN;
}

static double lv2_host_latency_ctx(state_t *st) {
    if (!st->open || st->latency_port < 0) return 0.0;
    return (double)st->ctrl[st->latency_port];
}

static double lv2_host_n_audio_in_ctx(state_t *st) { return st->open ? (double)st->n_audio_in  : 0.0; }
static double lv2_host_n_audio_out_ctx(state_t *st) { return st->open ? (double)st->n_audio_out : 0.0; }
static double lv2_host_sample_rate_ctx(state_t *st) { return st->open ? st->sample_rate : 0.0; }
static double lv2_host_block_size_ctx(state_t *st) { return st->open ? (double)st->block : 0.0; }
static double lv2_host_channels_ctx(state_t *st) { return st->open ? (double)st->chan : 0.0; }
static double lv2_host_is_open_ctx(state_t *st) { return st->open ? 1.0 : 0.0; }
static long   lv2_host_n_process_ctx(state_t *st) { return st->n_process; }
static void   lv2_host_reset_counters_ctx(state_t *st) { st->n_process = 0; }

/* ---- 7. The input block ---- */

/* Inputs are headed with an empty atom:Sequence in frame time; outputs
 * with an atom:Chunk whose size is the space after the atom header --
 * what atom.meta.ttl requires of a variable-size output. */
static void head_atom_seq_ctx(state_t *st, long s) {
    LV2_Atom_Sequence *seq = (LV2_Atom_Sequence *)(st->atom_arena + st->atom_off[s]);
    seq->atom.type = st->u_sequence;
    seq->atom.size = (uint32_t)sizeof(LV2_Atom_Sequence_Body);
    seq->body.unit = st->u_frametime;
    seq->body.pad  = 0;
}

static void head_atom_out_ctx(state_t *st, long s) {
    LV2_Atom *a = (LV2_Atom *)(st->atom_arena + st->atom_off[s]);
    a->type = st->u_chunk;
    a->size = (uint32_t)(st->atom_cap[s] - sizeof(LV2_Atom));
}

/* A new input block carries no events and clears a refused event. */
static void rehead_atom_inputs_ctx(state_t *st) {
    for (long s = 0; s < st->n_atom; s++) {
        if (!st->atom_in[s]) continue;
        head_atom_seq_ctx(st, s);
        st->atom_nev[s]  = 0;
        st->atom_last[s] = -1;
    }
    st->midi_bad = 0;
}

static double lv2_in_fill_ctx(state_t *st, const double *samples, long n, long channels) {
    if (!st->open) { set_err("no plugin is open"); return NAN; }
    if (!samples || n < 0) { set_err("lv2_in_fill: no samples"); return NAN; }
    if (n > st->block) n = st->block;
    long ch = (channels < 1) ? 1 : (channels > st->chan ? st->chan : channels);
    for (long i = 0; i < n; i++)
        for (long c = 0; c < st->chan; c++) {
            long src = (c < ch) ? (i * ch + c) : (i * ch);   /* mono -> all */
            st->in[c][i] = (float)samples[src];
        }
    for (long i = n; i < st->block; i++)
        for (long c = 0; c < st->chan; c++) st->in[c][i] = 0.0f;
    rehead_atom_inputs_ctx(st);
    memset(st->queued, 0, sizeof st->queued);
    st->in_armed = 1;
    st->in_n = n;
    st->in_armed = 1;
    return (st->in_token = next_token(st, st->in_token));
}

static double wave_at_ctx(state_t *st, long k, int w, double freq, double amp) {
    double t = (double)k / st->sample_rate;
    switch (w) {
    case LV2_WAVE_SINE:    return amp * sin(2.0 * M_PI * freq * t);
    case LV2_WAVE_SQUARE:  return amp * (sin(2.0 * M_PI * freq * t) >= 0.0 ? 1.0 : -1.0);
    case LV2_WAVE_RAMP:    return amp * (2.0 * fmod(freq * t, 1.0) - 1.0);
    case LV2_WAVE_IMPULSE: return (k == 0) ? amp : 0.0;
    default:               return 0.0;
    }
}

static double lv2_in_tone_ctx(state_t *st, double t, double waveform, double freq, double amp) {
    if (!st->open) { set_err("no plugin is open"); return NAN; }
    double end = t * st->sample_rate;
    if (!isfinite(end) || !isfinite(waveform)) {
        set_err("lv2_in_tone: time and waveform must be finite");
        return NAN;
    }
    long first = (long)(end + 0.5) - st->block;
    int w = (int)(waveform + 0.5);
    for (long i = 0; i < st->block; i++) {
        double v = wave_at_ctx(st, first + i, w, freq, amp);
        for (long c = 0; c < st->chan; c++) st->in[c][i] = (float)v;
    }
    rehead_atom_inputs_ctx(st);
    memset(st->queued, 0, sizeof st->queued);
    st->in_armed = 1;
    st->in_n = st->block;
    st->in_armed = 1;
    return (st->in_token = next_token(st, st->in_token));
}

static double lv2_in_sample_ctx(state_t *st, double dep, double i, double ch) {
    if (!st->open || !isfinite(dep) || !isfinite(i) || !isfinite(ch)) return NAN;
    if (!st->in_armed || !token_matches(st, dep, st->in_token)) return NAN;
    long k = (long)(i + 0.5), c = (long)(ch + 0.5);
    if (k < 0 || k >= st->in_n || c < 0 || c >= st->chan) return NAN;
    return (double)st->in[c][k];
}

/* Bytes a complete message with this status carries: channel messages 3
 * (2 for 0xC0-0xDF program change / channel pressure), the listed system
 * messages, -1 for variable-length sysex this call cannot carry, 0 for
 * undefined statuses. */
static int midi_msg_len(uint8_t st) {
    if (st < 0xC0)  return 3;
    if (st < 0xE0)  return 2;
    if (st < 0xF0)  return 3;
    if (st >= 0xF8) return 1;
    switch (st) {
    case 0xF1: case 0xF3: return 2;
    case 0xF2: return 3;
    case 0xF6: return 1;
    default:  return (st == 0xF0 || st == 0xF7) ? -1 : 0;
    }
}

/* dep is the input token, returned so a chain orders before lv2_process.
 * A refusal after the token check poisons the armed block. */
static double midi_refused_ctx(state_t *st, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ERR, sizeof ERR, fmt, ap);
    va_end(ap);
    st->midi_bad = 1;
    return NAN;
}

static double lv2_in_midi_ctx(state_t *st, double dep, double port, double frame,
                   double b0, double b1, double b2) {
    if (!st->open) { set_err("no plugin is open"); return NAN; }
    if (!isfinite(dep) || !st->in_armed || !token_matches(st, dep, st->in_token)) {
        set_err("lv2_in_midi: no input block armed for this token; call "
                "lv2_in_fill or lv2_in_tone first");
        return NAN;
    }
    if (!isfinite(port)) return midi_refused_ctx(st, "lv2_in_midi: port is not a number");
    long k = (long)(port + 0.5);
    if (k < 0 || k >= st->n_ports || st->atom_slot[k] < 0)
        return midi_refused_ctx(st, "lv2_in_midi: port %ld is not an atom port", k);
    long s = st->atom_slot[k];
    if (!st->atom_in[s])
        return midi_refused_ctx(st, "lv2_in_midi: port %ld is an atom output, not an input", k);
    if (!st->atom_midi[s])
        return midi_refused_ctx(st, "lv2_in_midi: port %ld does not declare "
                            "atom:supports midi:MidiEvent", k);
    if (!isfinite(frame))
        return midi_refused_ctx(st, "lv2_in_midi: frame is not a number");
    long f = (long)(frame + 0.5);
    if (f < 0 || f >= st->block)
        return midi_refused_ctx(st, "lv2_in_midi: frame %ld is outside the block (0..%ld)",
                            f, st->block - 1);
    if (f < st->atom_last[s])
        return midi_refused_ctx(st, "lv2_in_midi: event at frame %ld is out of order "
                            "after frame %lld (events must be queued in "
                            "non-decreasing frame order)", f, (long long)st->atom_last[s]);

    const double bytes[3] = { b0, b1, b2 };
    uint8_t msg[3];
    int nb = 0, ended = 0;
    for (int i = 0; i < 3; i++) {
        double x = bytes[i];
        if (!isfinite(x) || x < 0) { ended = 1; continue; }
        if (ended)
            return midi_refused_ctx(st, "lv2_in_midi: byte %d follows a gap in the "
                                "message", i);
        long b = (long)(x + 0.5);
        if (b > 0xFF)
            return midi_refused_ctx(st, "lv2_in_midi: byte %d is %ld, not a MIDI byte",
                                i, b);
        if (i == 0 && b < 0x80)
            return midi_refused_ctx(st, "lv2_in_midi: byte 0 is %ld, not a status byte "
                                "(0x80..0xFF)", b);
        if (i > 0 && b > 0x7F)
            return midi_refused_ctx(st, "lv2_in_midi: byte %d is %ld, not a data byte "
                                "(0x00..0x7F)", i, b);
        msg[nb++] = (uint8_t)b;
    }
    if (nb == 0)
        return midi_refused_ctx(st, "lv2_in_midi: a MIDI event needs a status byte");
    int want = midi_msg_len(msg[0]);
    if (want < 0)
        return midi_refused_ctx(st, "lv2_in_midi: status 0x%02lX is variable-length; "
                            "this call carries complete 1-3 byte messages only",
                            (unsigned long)msg[0]);
    if (want == 0)
        return midi_refused_ctx(st, "lv2_in_midi: status 0x%02lX is undefined",
                            (unsigned long)msg[0]);
    if (nb != want)
        return midi_refused_ctx(st, "lv2_in_midi: status 0x%02lX is a %d-byte message, "
                            "got %d", (unsigned long)msg[0], want, nb);

    LV2_Atom_Sequence *seq =
        (LV2_Atom_Sequence *)(st->atom_arena + st->atom_off[s]);
    /* atom.size counts the bytes after the atom header, so capacity is
     * the buffer less that header. */
    const uint32_t cap   = (uint32_t)(st->atom_cap[s] - sizeof(LV2_Atom));
    const uint32_t total = (uint32_t)sizeof(LV2_Atom_Event) + (uint32_t)nb;
    if (cap - seq->atom.size < total)
        return midi_refused_ctx(st, "lv2_in_midi: atom input sequence on port %ld is "
                            "full (%u bytes capacity)", k, cap);
    LV2_Atom_Event *e = lv2_atom_sequence_end(&seq->body, seq->atom.size);
    e->time.frames = (int64_t)f;
    e->body.size   = (uint32_t)nb;
    e->body.type   = st->u_midievent;
    memcpy(LV2_ATOM_BODY(&e->body), msg, (size_t)nb);
    seq->atom.size += lv2_atom_pad_size(total);
    st->atom_last[s] = f;
    st->atom_nev[s]++;
    return dep;
}

/* ---- 8. run() -- the node-side operator ---- */

static double lv2_process_ctx(state_t *st, double dep,
                   double id0, double v0, double id1, double v1,
                   double id2, double v2, double id3, double v3) {
    if (!st->open) { set_err("no plugin is open"); return NAN; }
    if (!isfinite(dep) || !st->in_armed || !token_matches(st, dep, st->in_token))
        return NAN;
    if (st->midi_bad) {
        set_err("lv2_process: the armed block carries a refused MIDI event; "
                "fill a fresh input block");
        return NAN;
    }
    /* One block, one run: a second lv2_process on this dep refuses
     * instead of replaying queued events. */
    st->in_armed = 0;

    for (long k = 0; k < LV2_HOST_MAX_PORTS; k++) {
        if (st->queued[k]) st->ctrl[k] = (float)st->pending[k];
        st->queued[k] = 0;
    }

    const double ids[LV2_HOST_PARAM_SLOTS]  = { id0, id1, id2, id3 };
    const double vals[LV2_HOST_PARAM_SLOTS] = { v0,  v1,  v2,  v3  };
    for (int i = 0; i < LV2_HOST_PARAM_SLOTS; i++) {
        if (!isfinite(ids[i]) || !(ids[i] >= 0.0) || isnan(vals[i])) continue;
        long k = (long)(ids[i] + 0.5);
        if (k < st->n_ports && st->is_ctrl_in[k]) st->ctrl[k] = (float)vals[i];
    }

    for (long s = 0; s < st->n_atom; s++)
        if (!st->atom_in[s]) head_atom_out_ctx(st, s);
    memset(st->side_in, 0, sizeof st->side_in);

    lilv_instance_run(st->inst, (uint32_t)st->block);
    st->n_process++;

    /* Fewer outputs than channels: repeat the last output (a mono plugin
     * is centre-panned). Zero outputs: the channels keep open()'s silence. */
    for (long c = st->n_audio_out; c < st->chan && st->n_audio_out > 0; c++)
        memcpy(st->out[c], st->out[st->n_audio_out - 1],
               (size_t)st->block * sizeof(float));

    st->out_n = st->block;
    return (st->out_token = next_token(st, st->out_token));
}

/* ---- 9. The output block ---- */

static int out_ok_ctx(state_t *st, double dep) {
    return st->open && isfinite(dep) && st->out_token != 0 &&
           token_matches(st, dep, st->out_token);
}

static double lv2_out_sample_ctx(state_t *st, double dep, double i, double ch) {
    if (!out_ok_ctx(st, dep) || !isfinite(i) || !isfinite(ch)) return NAN;
    long k = (long)(i + 0.5), c = (long)(ch + 0.5);
    if (k < 0 || k >= st->out_n || c < 0 || c >= st->chan) return NAN;
    return (double)st->out[c][k];
}

static double lv2_out_rms_ctx(state_t *st, double dep) {
    if (!out_ok_ctx(st, dep)) return NAN;
    if (st->out_n <= 0) return 0.0;
    double s = 0.0;
    for (long c = 0; c < st->chan; c++)
        for (long i = 0; i < st->out_n; i++) s += (double)st->out[c][i] * (double)st->out[c][i];
    return sqrt(s / (double)(st->out_n * st->chan));
}

static double lv2_out_peak_ctx(state_t *st, double dep) {
    if (!out_ok_ctx(st, dep)) return NAN;
    double m = 0.0;
    for (long c = 0; c < st->chan; c++)
        for (long i = 0; i < st->out_n; i++) {
            double a = fabs((double)st->out[c][i]);
            if (a > m) m = a;
        }
    return m;
}

static double lv2_out_count_ctx(state_t *st, double dep) { return out_ok_ctx(st, dep) ? (double)st->out_n : NAN; }
static double lv2_out_valid_ctx(state_t *st, double dep) { return out_ok_ctx(st, dep) ? 1.0 : 0.0; }

long lv2_host_scan(const char *lv2_path) { return lv2_host_scan_ctx(&DEFAULT_STATE, lv2_path); }

const char *lv2_host_scan_uri(long i) { return lv2_host_scan_uri_ctx(&DEFAULT_STATE, i); }

const char *lv2_host_scan_name(long i) { return lv2_host_scan_name_ctx(&DEFAULT_STATE, i); }

int lv2_host_open(const char *lv2_path, const char *uri,
                  double sample_rate, double block_size, double channels) { return lv2_host_open_ctx(&DEFAULT_STATE, lv2_path, uri, sample_rate, block_size, channels); }

void lv2_host_close(void) { lv2_host_close_ctx(&DEFAULT_STATE); }

const char *lv2_host_plugin_name(void) { return lv2_host_plugin_name_ctx(&DEFAULT_STATE); }

const char *lv2_host_plugin_name_for(double handle) {
    state_t *st = instance(handle);
    if (!st) return "";
    return lv2_host_plugin_name_ctx(st);
}

const char *lv2_host_plugin_uri(void) { return lv2_host_plugin_uri_ctx(&DEFAULT_STATE); }

const char *lv2_host_plugin_uri_for(double handle) {
    state_t *st = instance(handle);
    if (!st) return "";
    return lv2_host_plugin_uri_ctx(st);
}

long lv2_host_n_params(void) { return lv2_host_n_params_ctx(&DEFAULT_STATE); }

long lv2_host_n_params_for(double handle) {
    state_t *st = instance(handle);
    if (!st) return 0;
    return lv2_host_n_params_ctx(st);
}

double lv2_host_param_id(long i) { return lv2_host_param_id_ctx(&DEFAULT_STATE, i); }

double lv2_host_param_id_for(double handle, long i) {
    state_t *st = instance(handle);
    if (!st) return NAN;
    return lv2_host_param_id_ctx(st, i);
}

double lv2_host_param_min(long i) { return lv2_host_param_min_ctx(&DEFAULT_STATE, i); }

double lv2_host_param_min_for(double handle, long i) {
    state_t *st = instance(handle);
    if (!st) return NAN;
    return lv2_host_param_min_ctx(st, i);
}

double lv2_host_param_max(long i) { return lv2_host_param_max_ctx(&DEFAULT_STATE, i); }

double lv2_host_param_max_for(double handle, long i) {
    state_t *st = instance(handle);
    if (!st) return NAN;
    return lv2_host_param_max_ctx(st, i);
}

double lv2_host_param_default(long i) { return lv2_host_param_default_ctx(&DEFAULT_STATE, i); }

double lv2_host_param_default_for(double handle, long i) {
    state_t *st = instance(handle);
    if (!st) return NAN;
    return lv2_host_param_default_ctx(st, i);
}

const char *lv2_host_param_name(long i) { return lv2_host_param_name_ctx(&DEFAULT_STATE, i); }

const char *lv2_host_param_name_for(double handle, long i) {
    state_t *st = instance(handle);
    if (!st) return "";
    return lv2_host_param_name_ctx(st, i);
}

const char *lv2_host_param_symbol(long i) { return lv2_host_param_symbol_ctx(&DEFAULT_STATE, i); }

const char *lv2_host_param_symbol_for(double handle, long i) {
    state_t *st = instance(handle);
    if (!st) return "";
    return lv2_host_param_symbol_ctx(st, i);
}

double lv2_host_param_value(double port_index) { return lv2_host_param_value_ctx(&DEFAULT_STATE, port_index); }

double lv2_host_param_value_for(double handle, double port_index) {
    state_t *st = instance(handle);
    if (!st) return NAN;
    return lv2_host_param_value_ctx(st, port_index);
}

double lv2_host_port_value(double port_index) { return lv2_host_port_value_ctx(&DEFAULT_STATE, port_index); }

double lv2_host_port_value_for(double handle, double port_index) {
    state_t *st = instance(handle);
    if (!st) return NAN;
    return lv2_host_port_value_ctx(st, port_index);
}

double lv2_host_n_atom_ports(void) { return lv2_host_n_atom_ports_ctx(&DEFAULT_STATE); }

double lv2_host_n_atom_ports_for(double handle) {
    state_t *st = instance(handle);
    if (!st) return NAN;
    return lv2_host_n_atom_ports_ctx(st);
}

double lv2_host_atom_port_index(double i) { return lv2_host_atom_port_index_ctx(&DEFAULT_STATE, i); }

double lv2_host_atom_port_index_for(double handle, double i) {
    state_t *st = instance(handle);
    if (!st) return NAN;
    return lv2_host_atom_port_index_ctx(st, i);
}

double lv2_host_atom_port_is_input(double i) { return lv2_host_atom_port_is_input_ctx(&DEFAULT_STATE, i); }

double lv2_host_atom_port_is_input_for(double handle, double i) {
    state_t *st = instance(handle);
    if (!st) return NAN;
    return lv2_host_atom_port_is_input_ctx(st, i);
}

double lv2_host_atom_port_midi(double i) { return lv2_host_atom_port_midi_ctx(&DEFAULT_STATE, i); }

double lv2_host_atom_port_midi_for(double handle, double i) {
    state_t *st = instance(handle);
    if (!st) return NAN;
    return lv2_host_atom_port_midi_ctx(st, i);
}

double lv2_host_atom_port_size(double i) { return lv2_host_atom_port_size_ctx(&DEFAULT_STATE, i); }

double lv2_host_atom_port_size_for(double handle, double i) {
    state_t *st = instance(handle);
    if (!st) return NAN;
    return lv2_host_atom_port_size_ctx(st, i);
}

double lv2_host_latency(void) { return lv2_host_latency_ctx(&DEFAULT_STATE); }

double lv2_host_latency_for(double handle) {
    state_t *st = instance(handle);
    if (!st) return NAN;
    return lv2_host_latency_ctx(st);
}

double lv2_host_n_audio_in(void) { return lv2_host_n_audio_in_ctx(&DEFAULT_STATE); }

double lv2_host_n_audio_in_for(double handle) {
    state_t *st = instance(handle);
    if (!st) return NAN;
    return lv2_host_n_audio_in_ctx(st);
}

double lv2_host_n_audio_out(void) { return lv2_host_n_audio_out_ctx(&DEFAULT_STATE); }

double lv2_host_n_audio_out_for(double handle) {
    state_t *st = instance(handle);
    if (!st) return NAN;
    return lv2_host_n_audio_out_ctx(st);
}

double lv2_host_sample_rate(void) { return lv2_host_sample_rate_ctx(&DEFAULT_STATE); }

double lv2_host_sample_rate_for(double handle) {
    state_t *st = instance(handle);
    if (!st) return NAN;
    return lv2_host_sample_rate_ctx(st);
}

double lv2_host_block_size(void) { return lv2_host_block_size_ctx(&DEFAULT_STATE); }

double lv2_host_block_size_for(double handle) {
    state_t *st = instance(handle);
    if (!st) return NAN;
    return lv2_host_block_size_ctx(st);
}

double lv2_host_channels(void) { return lv2_host_channels_ctx(&DEFAULT_STATE); }

double lv2_host_channels_for(double handle) {
    state_t *st = instance(handle);
    if (!st) return NAN;
    return lv2_host_channels_ctx(st);
}

double lv2_host_is_open(void) { return lv2_host_is_open_ctx(&DEFAULT_STATE); }

double lv2_host_is_open_for(double handle) {
    state_t *st = instance(handle);
    if (!st) return 0;
    return lv2_host_is_open_ctx(st);
}

long   lv2_host_n_process(void) { return lv2_host_n_process_ctx(&DEFAULT_STATE); }

long   lv2_host_n_process_for(double handle) {
    state_t *st = instance(handle);
    if (!st) return 0;
    return lv2_host_n_process_ctx(st);
}

void   lv2_host_reset_counters(void) { lv2_host_reset_counters_ctx(&DEFAULT_STATE); }

void   lv2_host_reset_counters_for(double handle) {
    state_t *st = instance(handle);
    if (!st) return;
    lv2_host_reset_counters_ctx(st);
}

double lv2_in_fill(const double *samples, long n, long channels) { return lv2_in_fill_ctx(&DEFAULT_STATE, samples, n, channels); }

double lv2_in_fill_for(double handle, const double *samples, long n, long channels) {
    state_t *st = instance(handle);
    if (!st) return NAN;
    return lv2_in_fill_ctx(st, samples, n, channels);
}

double lv2_in_tone(double t, double waveform, double freq, double amp) { return lv2_in_tone_ctx(&DEFAULT_STATE, t, waveform, freq, amp); }

double lv2_in_tone_for(double handle, double t, double waveform, double freq, double amp) {
    state_t *st = instance(handle);
    if (!st) return NAN;
    return lv2_in_tone_ctx(st, t, waveform, freq, amp);
}

double lv2_in_sample(double dep, double i, double ch) { return lv2_in_sample_ctx(&DEFAULT_STATE, dep, i, ch); }

double lv2_in_sample_for(double handle, double dep, double i, double ch) {
    state_t *st = instance(handle);
    if (!st) return NAN;
    return lv2_in_sample_ctx(st, dep, i, ch);
}

double lv2_in_midi(double dep, double port, double frame,
                   double b0, double b1, double b2) { return lv2_in_midi_ctx(&DEFAULT_STATE, dep, port, frame, b0, b1, b2); }

double lv2_in_midi_for(double handle, double dep, double port, double frame,
                   double b0, double b1, double b2) {
    state_t *st = instance(handle);
    if (!st) return NAN;
    return lv2_in_midi_ctx(st, dep, port, frame, b0, b1, b2);
}

double lv2_process(double dep,
                   double id0, double v0, double id1, double v1,
                   double id2, double v2, double id3, double v3) { return lv2_process_ctx(&DEFAULT_STATE, dep, id0, v0, id1, v1, id2, v2, id3, v3); }

double lv2_process_for(double handle, double dep,
                   double id0, double v0, double id1, double v1,
                   double id2, double v2, double id3, double v3) {
    state_t *st = instance(handle);
    if (!st) return NAN;
    return lv2_process_ctx(st, dep, id0, v0, id1, v1, id2, v2, id3, v3);
}

double lv2_out_sample(double dep, double i, double ch) { return lv2_out_sample_ctx(&DEFAULT_STATE, dep, i, ch); }

double lv2_out_sample_for(double handle, double dep, double i, double ch) {
    state_t *st = instance(handle);
    if (!st) return NAN;
    return lv2_out_sample_ctx(st, dep, i, ch);
}

double lv2_out_rms(double dep) { return lv2_out_rms_ctx(&DEFAULT_STATE, dep); }

double lv2_out_rms_for(double handle, double dep) {
    state_t *st = instance(handle);
    if (!st) return NAN;
    return lv2_out_rms_ctx(st, dep);
}

double lv2_out_peak(double dep) { return lv2_out_peak_ctx(&DEFAULT_STATE, dep); }

double lv2_out_peak_for(double handle, double dep) {
    state_t *st = instance(handle);
    if (!st) return NAN;
    return lv2_out_peak_ctx(st, dep);
}

double lv2_out_count(double dep) { return lv2_out_count_ctx(&DEFAULT_STATE, dep); }

double lv2_out_count_for(double handle, double dep) {
    state_t *st = instance(handle);
    if (!st) return NAN;
    return lv2_out_count_ctx(st, dep);
}

double lv2_out_valid(double dep) { return lv2_out_valid_ctx(&DEFAULT_STATE, dep); }

double lv2_out_valid_for(double handle, double dep) {
    state_t *st = instance(handle);
    if (!st) return 0;
    return lv2_out_valid_ctx(st, dep);
}

long lv2_host_open_index_for(double handle) {
    state_t *st = instance(handle);
    return st && st->open ? st->open_index : -1;
}

double lv2_host_open_instance(const char *path, const char *id,
                              double rate, double block, double channels) {
    if (!isfinite(rate) || rate <= 0 || !isfinite(block) || block < 1 ||
        block > LV2_HOST_MAX_BLOCK || floor(block) != block ||
        !isfinite(channels) || channels < 1 || channels > LV2_HOST_MAX_CHAN || floor(channels) != channels ||
        NEXT_HANDLE >= 9007199254740991.0) {
        set_err("invalid LV2 instance configuration"); return NAN;
    }
    state_t *st = (state_t *)calloc(1, sizeof(state_t));
    if (!st) { set_err("out of memory opening LV2 instance"); return NAN; }
    st->handle = ++NEXT_HANDLE;
    if (lv2_host_open_ctx(st, path, id, rate, block, channels)) {
        lv2_host_close_ctx(st);
        free(st->desc); free(st);
        return NAN;
    }
    st->next = INSTANCES;
    INSTANCES = st;
    return st->handle;
}

void lv2_host_close_instance(double handle) {
    state_t **link = &INSTANCES;
    while (*link && (*link)->handle != handle) link = &(*link)->next;
    if (!*link) return;
    state_t *st = *link;
    *link = st->next;
    lv2_host_close_ctx(st);
    free(st->desc); free(st);
}

static double lv2_set_param_ctx(state_t *st, double dep, double id, double value) {
    if (!st->open || !st->in_armed || !token_matches(st, dep, st->in_token) || !isfinite(value)) return NAN;
    if (!isfinite(id) || id < 0 || id >= st->n_ports || floor(id) != id || !st->is_ctrl_in[(long)id]) return NAN;
    long k = (long)id;
    st->pending[k] = value;
    st->queued[k] = 1;
    return dep;
}
double lv2_set_param(double dep, double id, double value) {
    return lv2_set_param_ctx(&DEFAULT_STATE, dep, id, value);
}
double lv2_set_param_for(double handle, double dep, double id, double value) {
    state_t *st = instance(handle);
    return st ? lv2_set_param_ctx(st, dep, id, value) : NAN;
}

static double lv2_expect_ctx(state_t *st, double dep, double index) {
    if (!st->open || !isfinite(dep) || !isfinite(index) || index < 0 || floor(index) != index || index != st->open_index) return NAN;
    return dep;
}
double lv2_expect(double dep, double index) { return lv2_expect_ctx(&DEFAULT_STATE, dep, index); }
double lv2_expect_for(double handle, double dep, double index) {
    state_t *st = instance(handle);
    return st ? lv2_expect_ctx(st, dep, index) : NAN;
}

double lv2_in_copy_for(double destination, double source, double dep) {
    state_t *dst = instance(destination), *src = instance(source);
    if (!dst || !src || !dst->open || !out_ok_ctx(src, dep)) return NAN;
    if (dst->block != src->block || dst->chan != src->chan || dst->sample_rate != src->sample_rate) {
        set_err("LV2 block copy configuration mismatch"); return NAN;
    }
    for (long c = 0; c < dst->chan; c++)
        memcpy(dst->in[c], src->out[c], sizeof(float) * (size_t)dst->block);
    memset(dst->queued, 0, sizeof dst->queued);
    rehead_atom_inputs_ctx(dst);
    dst->in_n = src->out_n;
    dst->in_armed = 1;
    return (dst->in_token = next_token(dst, dst->in_token));
}
