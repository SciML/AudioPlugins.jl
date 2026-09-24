/* ap_test_plugins.c -- a real CLAP bundle, ours, for testing the host.
 *
 * Hosting is only proved by hosting something, and depending on a
 * third-party plugin would make the suite depend on a binary we do not
 * control, cannot check the arithmetic of, and may not be able to fetch.
 * So the suite hosts these: three plugins whose output is analytically
 * known, in one bundle, which also exercises factory enumeration and
 * selection by id.
 *
 *   dyad.gain       out = in * gain           -- parameter arithmetic
 *   dyad.onepole    y += a * (x - y)          -- state across blocks
 *   dyad.lookahead  out = in delayed by 16    -- reported latency
 *
 * The last two are the interesting ones: a plugin whose state does NOT
 * persist across process() calls is just a distortion unit, and a plugin
 * whose reported latency is ignored misaligns the stream. Both are
 * properties of the host that only a stateful plugin can demonstrate.
 */

#include "../../csrc/vendor/clap/clap.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOOKAHEAD_SAMPLES 16
#define MAX_CHAN 2

/* Assert module lifetime as well as audio arithmetic. A host must share
 * init/deinit across its instances and destroy them before unloading. */
static unsigned entry_refs, live_instances;

/* ---------------------------------------------------------------- *
 * Descriptors
 * ---------------------------------------------------------------- */

static const char *const FEATURES[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL };

static const clap_plugin_descriptor_t DESC_GAIN = {
    .clap_version = CLAP_VERSION_INIT, .id = "ap.gain", .name = "AudioPlugins Test Gain",
    .vendor = "JuliaHub", .url = "", .manual_url = "", .support_url = "",
    .version = "0.1.0", .description = "out = in * gain", .features = FEATURES,
};
static const clap_plugin_descriptor_t DESC_ONEPOLE = {
    .clap_version = CLAP_VERSION_INIT, .id = "ap.onepole", .name = "AudioPlugins Test One Pole",
    .vendor = "JuliaHub", .url = "", .manual_url = "", .support_url = "",
    .version = "0.1.0", .description = "y += a * (x - y), state across blocks",
    .features = FEATURES,
};
static const clap_plugin_descriptor_t DESC_LOOKAHEAD = {
    .clap_version = CLAP_VERSION_INIT, .id = "ap.lookahead", .name = "AudioPlugins Test Lookahead",
    .vendor = "JuliaHub", .url = "", .manual_url = "", .support_url = "",
    .version = "0.1.0", .description = "16-sample delay, reports its latency",
    .features = FEATURES,
};

/* ---------------------------------------------------------------- *
 * Instance state
 * ---------------------------------------------------------------- */

typedef enum { KIND_GAIN, KIND_ONEPOLE, KIND_LOOKAHEAD } kind_t;

typedef struct {
    clap_plugin_t plugin;      /* must be first: the host holds this       */
    kind_t kind;
    double param;              /* gain, or the one-pole coefficient        */
    double y[MAX_CHAN];        /* one-pole state, across blocks            */
    float  hist[MAX_CHAN][LOOKAHEAD_SAMPLES];
    long   hist_pos;
    int    activated;
    int    processing;
} inst_t;

static double param_default(kind_t k) { return (k == KIND_ONEPOLE) ? 0.25 : 1.0; }
static double param_min(kind_t k)     { return (k == KIND_ONEPOLE) ? 0.0 : 0.0; }
static double param_max(kind_t k)     { return (k == KIND_ONEPOLE) ? 1.0 : 4.0; }
static const char *param_name(kind_t k) { return (k == KIND_ONEPOLE) ? "Coefficient" : "Gain"; }

/* ---------------------------------------------------------------- *
 * clap.params
 * ---------------------------------------------------------------- */

static uint32_t params_count(const clap_plugin_t *p) {
    inst_t *s = p->plugin_data;
    return (s->kind == KIND_LOOKAHEAD) ? 0u : 1u;   /* lookahead has none */
}

static bool params_get_info(const clap_plugin_t *p, uint32_t index, clap_param_info_t *info) {
    inst_t *s = p->plugin_data;
    if (index != 0 || s->kind == KIND_LOOKAHEAD) return false;
    memset(info, 0, sizeof *info);
    info->id = 0;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    info->cookie = NULL;
    snprintf(info->name, sizeof info->name, "%s", param_name(s->kind));
    snprintf(info->module, sizeof info->module, "%s", "");
    info->min_value = param_min(s->kind);
    info->max_value = param_max(s->kind);
    info->default_value = param_default(s->kind);
    return true;
}

static bool params_get_value(const clap_plugin_t *p, clap_id id, double *out) {
    inst_t *s = p->plugin_data;
    if (id != 0 || s->kind == KIND_LOOKAHEAD) return false;
    *out = s->param;
    return true;
}

static bool params_value_to_text(const clap_plugin_t *p, clap_id id, double v,
                                 char *buf, uint32_t cap) {
    (void)p;
    if (id != 0) return false;
    snprintf(buf, cap, "%.4f", v);
    return true;
}

static bool params_text_to_value(const clap_plugin_t *p, clap_id id,
                                 const char *txt, double *out) {
    (void)p;
    if (id != 0 || !txt) return false;
    *out = atof(txt);
    return true;
}

static void apply_events(inst_t *s, const clap_input_events_t *in) {
    if (!in) return;
    uint32_t n = in->size(in);
    for (uint32_t i = 0; i < n; i++) {
        const clap_event_header_t *h = in->get(in, i);
        if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
        if (h->type != CLAP_EVENT_PARAM_VALUE) continue;
        const clap_event_param_value_t *e = (const clap_event_param_value_t *)h;
        if (e->param_id == 0) s->param = e->value;
    }
}

static void params_flush(const clap_plugin_t *p, const clap_input_events_t *in,
                         const clap_output_events_t *out) {
    (void)out;
    apply_events(p->plugin_data, in);
}

static const clap_plugin_params_t PARAMS_EXT = {
    .count = params_count,
    .get_info = params_get_info,
    .get_value = params_get_value,
    .value_to_text = params_value_to_text,
    .text_to_value = params_text_to_value,
    .flush = params_flush,
};

/* ---------------------------------------------------------------- *
 * clap.latency -- only the lookahead plugin has any
 * ---------------------------------------------------------------- */

static uint32_t latency_get(const clap_plugin_t *p) {
    inst_t *s = p->plugin_data;
    return (s->kind == KIND_LOOKAHEAD) ? (uint32_t)LOOKAHEAD_SAMPLES : 0u;
}
static const clap_plugin_latency_t LATENCY_EXT = { .get = latency_get };

/* ---------------------------------------------------------------- *
 * clap.audio-ports -- one stereo in, one stereo out
 * ---------------------------------------------------------------- */

static uint32_t ports_count(const clap_plugin_t *p, bool is_input) {
    (void)p; (void)is_input;
    return 1;
}
static bool ports_get(const clap_plugin_t *p, uint32_t index, bool is_input,
                      clap_audio_port_info_t *info) {
    (void)p; (void)is_input;
    if (index != 0) return false;
    memset(info, 0, sizeof *info);
    info->id = 0;
    snprintf(info->name, sizeof info->name, "%s", is_input ? "In" : "Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
static const clap_plugin_audio_ports_t PORTS_EXT = { .count = ports_count, .get = ports_get };

/* ---------------------------------------------------------------- *
 * Plugin lifecycle
 * ---------------------------------------------------------------- */

static bool plug_init(const clap_plugin_t *p) { (void)p; return true; }

static void plug_destroy(const clap_plugin_t *p) {
    assert(entry_refs > 0 && live_instances > 0);
    live_instances--;
    free(p->plugin_data);
}

static bool plug_activate(const clap_plugin_t *p, double sr, uint32_t minf, uint32_t maxf) {
    (void)sr;
    inst_t *s = p->plugin_data;
    if (minf < 1 || maxf > 8192) return false;
    s->activated = 1;
    memset(s->y, 0, sizeof s->y);
    memset(s->hist, 0, sizeof s->hist);
    s->hist_pos = 0;
    return true;
}
static void plug_deactivate(const clap_plugin_t *p) {
    ((inst_t *)p->plugin_data)->activated = 0;
}
static bool plug_start(const clap_plugin_t *p) {
    ((inst_t *)p->plugin_data)->processing = 1;
    return true;
}
static void plug_stop(const clap_plugin_t *p) {
    ((inst_t *)p->plugin_data)->processing = 0;
}
static void plug_reset(const clap_plugin_t *p) {
    inst_t *s = p->plugin_data;
    memset(s->y, 0, sizeof s->y);
    memset(s->hist, 0, sizeof s->hist);
    s->hist_pos = 0;
}

static clap_process_status plug_process(const clap_plugin_t *p, const clap_process_t *pr) {
    inst_t *s = p->plugin_data;
    if (!pr || pr->audio_inputs_count < 1 || pr->audio_outputs_count < 1)
        return CLAP_PROCESS_ERROR;

    apply_events(s, pr->in_events);

    const clap_audio_buffer_t *in = &pr->audio_inputs[0];
    clap_audio_buffer_t *out = &pr->audio_outputs[0];
    if (!in->data32 || !out->data32) return CLAP_PROCESS_ERROR;

    uint32_t nch = in->channel_count < out->channel_count
                 ? in->channel_count : out->channel_count;
    if (nch > MAX_CHAN) nch = MAX_CHAN;
    uint32_t n = pr->frames_count;

    for (uint32_t c = 0; c < nch; c++) {
        const float *x = in->data32[c];
        float *y = out->data32[c];
        switch (s->kind) {
        case KIND_GAIN:
            for (uint32_t i = 0; i < n; i++) y[i] = (float)(x[i] * s->param);
            break;
        case KIND_ONEPOLE: {
            double st = s->y[c];
            for (uint32_t i = 0; i < n; i++) {
                st += s->param * ((double)x[i] - st);
                y[i] = (float)st;
            }
            s->y[c] = st;                     /* carried to the next block */
            break;
        }
        case KIND_LOOKAHEAD: {
            long pos = s->hist_pos;
            for (uint32_t i = 0; i < n; i++) {
                float old = s->hist[c][pos];
                s->hist[c][pos] = x[i];
                y[i] = old;
                pos = (pos + 1) % LOOKAHEAD_SAMPLES;
            }
            if (c == nch - 1) s->hist_pos = pos;
            break;
        }
        }
    }
    return CLAP_PROCESS_CONTINUE;
}

static const void *plug_get_extension(const clap_plugin_t *p, const char *id) {
    inst_t *s = p->plugin_data;
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &PORTS_EXT;
    if (strcmp(id, CLAP_EXT_PARAMS) == 0 && s->kind != KIND_LOOKAHEAD) return &PARAMS_EXT;
    if (strcmp(id, CLAP_EXT_LATENCY) == 0 && s->kind == KIND_LOOKAHEAD) return &LATENCY_EXT;
    return NULL;
}

static void plug_on_main_thread(const clap_plugin_t *p) { (void)p; }

static const clap_plugin_t *make(kind_t kind, const clap_plugin_descriptor_t *desc,
                                 const clap_host_t *host) {
    (void)host;
    inst_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    assert(entry_refs > 0);
    live_instances++;
    s->kind = kind;
    s->param = param_default(kind);
    s->plugin.desc = desc;
    s->plugin.plugin_data = s;
    s->plugin.init = plug_init;
    s->plugin.destroy = plug_destroy;
    s->plugin.activate = plug_activate;
    s->plugin.deactivate = plug_deactivate;
    s->plugin.start_processing = plug_start;
    s->plugin.stop_processing = plug_stop;
    s->plugin.reset = plug_reset;
    s->plugin.process = plug_process;
    s->plugin.get_extension = plug_get_extension;
    s->plugin.on_main_thread = plug_on_main_thread;
    return &s->plugin;
}

/* ---------------------------------------------------------------- *
 * Factory and entry
 * ---------------------------------------------------------------- */

static const clap_plugin_descriptor_t *const DESCS[] = {
    &DESC_GAIN, &DESC_ONEPOLE, &DESC_LOOKAHEAD,
};
#define N_DESCS (sizeof DESCS / sizeof DESCS[0])

static uint32_t factory_count(const clap_plugin_factory_t *f) { (void)f; return N_DESCS; }

static const clap_plugin_descriptor_t *factory_desc(const clap_plugin_factory_t *f,
                                                    uint32_t index) {
    (void)f;
    return (index < N_DESCS) ? DESCS[index] : NULL;
}

static const clap_plugin_t *factory_create(const clap_plugin_factory_t *f,
                                           const clap_host_t *host, const char *id) {
    (void)f;
    if (!id) return NULL;
    if (strcmp(id, DESC_GAIN.id) == 0)      return make(KIND_GAIN, &DESC_GAIN, host);
    if (strcmp(id, DESC_ONEPOLE.id) == 0)   return make(KIND_ONEPOLE, &DESC_ONEPOLE, host);
    if (strcmp(id, DESC_LOOKAHEAD.id) == 0) return make(KIND_LOOKAHEAD, &DESC_LOOKAHEAD, host);
    return NULL;
}

static const clap_plugin_factory_t FACTORY = {
    .get_plugin_count = factory_count,
    .get_plugin_descriptor = factory_desc,
    .create_plugin = factory_create,
};

static bool entry_init(const char *path) {
    (void)path;
    /* Reentrant initialization is required by CLAP 1.2 (a plugin may wrap
     * another host), but this fixture also supports checking that our own
     * host does not unnecessarily reinitialize a shared module. */
#ifdef AP_TEST_SINGLE_INIT
    assert(entry_refs == 0);
#endif
    entry_refs++;
    return true;
}
static void entry_deinit(void) {
    assert(entry_refs > 0);
    if (--entry_refs == 0) assert(live_instances == 0);
}

static const void *entry_get_factory(const char *id) {
    return (strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0) ? &FACTORY : NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION_INIT,
    .init = entry_init,
    .deinit = entry_deinit,
    .get_factory = entry_get_factory,
};
