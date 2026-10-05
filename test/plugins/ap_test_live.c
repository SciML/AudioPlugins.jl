/* Live fixture: validates lifecycle thread identities and sample offsets. */
#include "../../csrc/vendor/clap/clap.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    clap_plugin_t plugin;
    const clap_host_t *host;
    const clap_host_thread_check_t *threads;
    double gain;
    float history[2][16];
    unsigned position;
    int active, processing, mode, first;
} instance;
static _Atomic unsigned refs, instances, callbacks;
unsigned ap_live_fixture_callbacks(void) { return atomic_load(&callbacks); }
static void main_thread(instance *s) {
    assert(s->threads->is_main_thread(s->host));
    assert(!s->threads->is_audio_thread(s->host));
}
static void audio_thread(instance *s) {
    assert(s->threads->is_audio_thread(s->host));
    assert(!s->threads->is_main_thread(s->host));
}
static bool CLAP_ABI init(const clap_plugin_t *p) {
    instance *s = p->plugin_data;
    s->threads = s->host->get_extension(s->host, CLAP_EXT_THREAD_CHECK);
    assert(s->threads);
    main_thread(s);
    return s->mode != 5;
}
static void CLAP_ABI destroy(const clap_plugin_t *p) {
    instance *s = p->plugin_data; main_thread(s);
    assert(!s->active && !s->processing);
    atomic_fetch_sub(&instances, 1); free(s);
}
static bool CLAP_ABI activate(const clap_plugin_t *p, double sr, uint32_t lo, uint32_t hi) {
    instance *s = p->plugin_data; main_thread(s);
    assert(!s->active && sr > 0 && lo == hi);
    if (s->mode == 6) return false;
    s->active = 1; s->first = 1;
    memset(s->history, 0, sizeof(s->history)); s->position = 0;
    return true;
}
static void CLAP_ABI deactivate(const clap_plugin_t *p) {
    instance *s = p->plugin_data; main_thread(s);
    assert(s->active && !s->processing); s->active = 0;
}
static bool CLAP_ABI start(const clap_plugin_t *p) {
    instance *s = p->plugin_data; audio_thread(s);
    assert(s->active && !s->processing);
    if (s->mode == 2) return false;
    s->processing = 1; return true;
}
static void CLAP_ABI stop(const clap_plugin_t *p) {
    instance *s = p->plugin_data; audio_thread(s);
    assert(s->processing); s->processing = 0;
}
static void CLAP_ABI reset(const clap_plugin_t *p) { audio_thread(p->plugin_data); }
static clap_process_status CLAP_ABI process(const clap_plugin_t *p, const clap_process_t *pr) {
    instance *s = p->plugin_data; audio_thread(s);
    assert(s->processing && s->active);
    if (s->mode == 3) return CLAP_PROCESS_ERROR;
    if (s->first) {
        s->host->request_callback(s->host);
        if (s->mode == 4) s->host->request_restart(s->host);
        s->first = 0;
    }
    uint32_t event = 0, count = pr->in_events->size(pr->in_events);
    for (uint32_t frame = 0; frame < pr->frames_count; ++frame) {
        while (event < count) {
            const clap_event_header_t *h = pr->in_events->get(pr->in_events, event);
            if (h->time != frame) break;
            if (h->type == CLAP_EVENT_PARAM_VALUE) s->gain = ((const clap_event_param_value_t *)h)->value;
            if (h->type == CLAP_EVENT_MIDI) s->gain = ((const clap_event_midi_t *)h)->data[2] / 127.0;
            assert(!pr->out_events->try_push(pr->out_events, h));
            ++event;
        }
        for (uint32_t ch = 0; ch < 2; ++ch) {
            float x = pr->audio_inputs[0].data32[ch][frame];
            float y = (float)(x * s->gain);
            if (s->mode == 1) { y = s->history[ch][s->position]; s->history[ch][s->position] = x; }
            pr->audio_outputs[0].data32[ch][frame] = y;
        }
        s->position = (s->position + 1) % 16;
    }
    assert(event == count);
    return CLAP_PROCESS_CONTINUE;
}
static void CLAP_ABI callback(const clap_plugin_t *p) {
    instance *s = p->plugin_data; main_thread(s);
    atomic_fetch_add(&callbacks, 1);
}
static uint32_t CLAP_ABI ports_count(const clap_plugin_t *p, bool in) { (void)p; (void)in; return 1; }
static bool CLAP_ABI ports_get(const clap_plugin_t *p, uint32_t i, bool in, clap_audio_port_info_t *info) {
    (void)in; main_thread(p->plugin_data);
    if (i) return false;
    memset(info, 0, sizeof(*info));
    info->channel_count = 2; info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    return true;
}
static bool CLAP_ABI notes_get(const clap_plugin_t *p, uint32_t i, bool in, clap_note_port_info_t *info) {
    (void)in; main_thread(p->plugin_data);
    if (i) return false;
    memset(info, 0, sizeof(*info)); info->supported_dialects = CLAP_NOTE_DIALECT_MIDI;
    return true;
}
static uint32_t CLAP_ABI param_count(const clap_plugin_t *p) { main_thread(p->plugin_data); return 1; }
static bool CLAP_ABI param_get(const clap_plugin_t *p, uint32_t i, clap_param_info_t *info) {
    main_thread(p->plugin_data); if (i) return false;
    memset(info, 0, sizeof(*info)); info->id = 0;
    info->max_value = 4; info->default_value = 1;
    return true;
}
static const clap_plugin_audio_ports_t ports = { ports_count, ports_get };
static const clap_plugin_note_ports_t notes = { ports_count, notes_get };
static const clap_plugin_params_t params = { .count = param_count, .get_info = param_get };
static uint32_t CLAP_ABI latency(const clap_plugin_t *p) {
    instance *s = p->plugin_data; main_thread(s); assert(s->active);
    return s->mode == 1 ? 16 : 0;
}
static const clap_plugin_latency_t latency_ext = { latency };
static const void *CLAP_ABI extension(const clap_plugin_t *p, const char *id) {
    (void)p;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &ports;
    if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &notes;
    if (!strcmp(id, CLAP_EXT_PARAMS)) return &params;
    if (!strcmp(id, CLAP_EXT_LATENCY)) return &latency_ext;
    return NULL;
}
static const char *const features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL };
static const clap_plugin_descriptor_t desc = {
    .clap_version = CLAP_VERSION_INIT, .id = "ap.live", .name = "Live fixture",
    .vendor = "AudioPlugins", .version = "1", .features = features
};
static uint32_t CLAP_ABI count(const clap_plugin_factory_t *f) { (void)f; return 1; }
static const clap_plugin_descriptor_t *CLAP_ABI descriptor(const clap_plugin_factory_t *f, uint32_t i) {
    (void)f; return i ? NULL : &desc;
}
static const clap_plugin_t *CLAP_ABI create(const clap_plugin_factory_t *f, const clap_host_t *h, const char *id) {
    (void)f;
    const char *ids[] = { "ap.live", "ap.delay", "ap.startfail", "ap.processfail", "ap.restart", "ap.initfail", "ap.activatefail" };
    int mode = -1;
    for (int i = 0; i < 7; ++i) if (!strcmp(id, ids[i])) mode = i;
    if (mode < 0) return NULL;
    instance *s = calloc(1, sizeof(*s)); if (!s) return NULL;
    atomic_fetch_add(&instances, 1);
    s->host = h; s->gain = 1; s->mode = mode;
    s->plugin = (clap_plugin_t){ &desc, s, init, destroy, activate, deactivate,
        start, stop, reset, process, extension, callback };
    return &s->plugin;
}
static const clap_plugin_factory_t factory = { count, descriptor, create };
static bool CLAP_ABI entry_init(const char *path) { (void)path; atomic_fetch_add(&refs, 1); return true; }
static void CLAP_ABI entry_deinit(void) { if (atomic_fetch_sub(&refs, 1) == 1) assert(!atomic_load(&instances)); }
static const void *CLAP_ABI get_factory(const char *id) { return !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &factory : NULL; }
CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, entry_init, entry_deinit, get_factory };
