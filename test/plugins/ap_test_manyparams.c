/* ap_test_manyparams.c -- one CLAP plugin with more parameters than
 * clap_process() has argument slots, so that the chained clap_set_param()
 * path has something to be checked against.
 *
 * test/plugins/ap_test_plugins.c covers the arithmetic of hosting; this
 * covers the arithmetic of *addressing*. Its one plugin, ap.weights, has
 * eight parameters and computes
 *
 *     out = in * (p0 + 2*p1 + 4*p2 + ... + 128*p7)
 *
 * The powers of two are the point. A sum of the parameters would pass just
 * as happily if the chain delivered the right eight values to the wrong
 * eight ids, and a mis-addressed parameter is exactly the failure a
 * generated per-plugin component can make. With these weights every
 * permutation gives a different answer, so one multiply of a known input
 * pins down which value reached which id.
 *
 *   cc -O2 -fPIC -shared -o ap_manyparams.clap test/plugins/ap_test_manyparams.c
 */

#include "../../csrc/vendor/clap/clap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N_PARAMS 8
#define MAX_CHAN 2

static const char *const FEATURES[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL };

static const clap_plugin_descriptor_t DESC_WEIGHTS = {
    .clap_version = CLAP_VERSION_INIT, .id = "ap.weights",
    .name = "AudioPlugins Test Weights",
    .vendor = "JuliaHub", .url = "", .manual_url = "", .support_url = "",
    .version = "0.1.0", .description = "out = in * sum(2^i * p_i), i in 0..7",
    .features = FEATURES,
};

typedef struct {
    clap_plugin_t plugin;
    double        param[N_PARAMS];
} inst_t;

/* Names deliberately unlike each other and unlike an identifier: a
 * generated component has to sanitise these, and a fixture whose names were
 * already identifiers would not exercise that. */
static const char *const PNAMES[N_PARAMS] = {
    "Weight 1", "Weight 2", "Weight 4", "Weight 8",
    "Weight 16", "Weight 32", "Weight 64", "Weight 128",
};

/* ---------------------------------------------------------------- *
 * clap.params
 * ---------------------------------------------------------------- */

static uint32_t params_count(const clap_plugin_t *p) { (void)p; return N_PARAMS; }

static bool params_get_info(const clap_plugin_t *p, uint32_t index,
                            clap_param_info_t *info) {
    (void)p;
    if (index >= N_PARAMS) return false;
    memset(info, 0, sizeof *info);
    info->id = index;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    snprintf(info->name, sizeof info->name, "%s", PNAMES[index]);
    info->min_value = 0.0;
    info->max_value = 1.0;
    info->default_value = 0.0;
    return true;
}

static bool params_get_value(const clap_plugin_t *p, clap_id id, double *out) {
    inst_t *s = p->plugin_data;
    if (id >= N_PARAMS) return false;
    *out = s->param[id];
    return true;
}

static bool params_value_to_text(const clap_plugin_t *p, clap_id id, double v,
                                 char *buf, uint32_t cap) {
    (void)p;
    if (id >= N_PARAMS || cap == 0) return false;
    snprintf(buf, cap, "%.3f", v);
    return true;
}

static bool params_text_to_value(const clap_plugin_t *p, clap_id id,
                                 const char *txt, double *out) {
    (void)p;
    if (id >= N_PARAMS || !txt) return false;
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
        if (e->param_id < N_PARAMS) s->param[e->param_id] = e->value;
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
 * clap.audio-ports -- one stereo in, one stereo out
 * ---------------------------------------------------------------- */

static uint32_t ports_count(const clap_plugin_t *p, bool is_input) {
    (void)p; (void)is_input;
    return 1;
}
static bool ports_get(const clap_plugin_t *p, uint32_t index, bool is_input,
                      clap_audio_port_info_t *info) {
    (void)p;
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
static void plug_destroy(const clap_plugin_t *p) { free(p->plugin_data); }

static bool plug_activate(const clap_plugin_t *p, double sr, uint32_t minf, uint32_t maxf) {
    (void)p; (void)sr;
    return minf >= 1 && maxf <= 8192;
}
static void plug_deactivate(const clap_plugin_t *p) { (void)p; }
static bool plug_start(const clap_plugin_t *p) { (void)p; return true; }
static void plug_stop(const clap_plugin_t *p) { (void)p; }
static void plug_reset(const clap_plugin_t *p) { (void)p; }

static clap_process_status plug_process(const clap_plugin_t *p, const clap_process_t *pr) {
    inst_t *s = p->plugin_data;
    if (!pr || pr->audio_inputs_count < 1 || pr->audio_outputs_count < 1)
        return CLAP_PROCESS_ERROR;

    apply_events(s, pr->in_events);

    const clap_audio_buffer_t *in = &pr->audio_inputs[0];
    clap_audio_buffer_t *out = &pr->audio_outputs[0];
    if (!in->data32 || !out->data32) return CLAP_PROCESS_ERROR;

    double g = 0.0;
    for (int k = 0; k < N_PARAMS; k++) g += (double)(1u << k) * s->param[k];

    uint32_t nch = in->channel_count < out->channel_count
                 ? in->channel_count : out->channel_count;
    if (nch > MAX_CHAN) nch = MAX_CHAN;
    for (uint32_t c = 0; c < nch; c++) {
        const float *x = in->data32[c];
        float *y = out->data32[c];
        for (uint32_t i = 0; i < pr->frames_count; i++) y[i] = (float)(x[i] * g);
    }
    return CLAP_PROCESS_CONTINUE;
}

static const void *plug_get_extension(const clap_plugin_t *p, const char *id) {
    (void)p;
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &PORTS_EXT;
    if (strcmp(id, CLAP_EXT_PARAMS) == 0) return &PARAMS_EXT;
    return NULL;
}

static void plug_on_main_thread(const clap_plugin_t *p) { (void)p; }

/* ---------------------------------------------------------------- *
 * Factory and entry
 * ---------------------------------------------------------------- */

static const clap_plugin_t *factory_create(const clap_plugin_factory_t *f,
                                           const clap_host_t *host, const char *id) {
    (void)f; (void)host;
    if (!id || strcmp(id, DESC_WEIGHTS.id) != 0) return NULL;
    inst_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->plugin.desc = &DESC_WEIGHTS;
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

static uint32_t factory_count(const clap_plugin_factory_t *f) { (void)f; return 1; }

static const clap_plugin_descriptor_t *factory_desc(const clap_plugin_factory_t *f,
                                                    uint32_t index) {
    (void)f;
    return (index == 0) ? &DESC_WEIGHTS : NULL;
}

static const clap_plugin_factory_t FACTORY = {
    .get_plugin_count = factory_count,
    .get_plugin_descriptor = factory_desc,
    .create_plugin = factory_create,
};

static bool entry_init(const char *path) { (void)path; return true; }
static void entry_deinit(void) { }

static const void *entry_get_factory(const char *id) {
    return (strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0) ? &FACTORY : NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION_INIT,
    .init = entry_init,
    .deinit = entry_deinit,
    .get_factory = entry_get_factory,
};
