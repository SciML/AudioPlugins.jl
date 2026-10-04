/* Isolated LV2 live adapter. lilv is confined to initialization/destruction. */
#include "live_adapter.h"
#include <lilv/lilv.h>
#include <lv2/urid/urid.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/resize-port/resize-port.h>
#include <stdatomic.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define PORTS 512
#define URIDS 1024
#define ATOM_BYTES 4096
typedef struct {
    clap_plugin_t api;
    const clap_host_t *host;
    ap_live_config config;
    LilvWorld *world;
    const LilvPlugin *descriptor;
    LilvInstance *instance;
    uint32_t nports, nparams, nin, nout, input[2], output[2], params[PORTS];
    clap_param_info_t info[PORTS];
    float control[PORTS];
    float *audio;
    int atom_in, atom_out, midi_input, latency_port;
    union { uint64_t alignment; uint8_t bytes[ATOM_BYTES]; } midi_in, midi_out;
    LV2_URID_Map map;
    LV2_URID_Unmap unmap;
    LV2_Feature features[4];
    const LV2_Feature *feature_ptrs[5];
    char uris[URIDS][256];
    _Atomic uint32_t uri_count;
    atomic_flag map_guard;
    LV2_URID sequence, midi, chunk;
} lv2_live;
static LV2_URID map_uri(LV2_URID_Map_Handle handle, const char *uri) {
    lv2_live *s = handle;
    if (!uri) return 0;
    size_t n = 0; while (n < 256 && uri[n]) ++n;
    if (n == 256) return 0;
    uint32_t count = atomic_load_explicit(&s->uri_count, memory_order_acquire);
    for (uint32_t i = 0; i < count; ++i) if (!strcmp(s->uris[i], uri)) return i + 1;
    if (atomic_flag_test_and_set_explicit(&s->map_guard, memory_order_acquire)) return 0;
    count = atomic_load_explicit(&s->uri_count, memory_order_relaxed);
    LV2_URID result = 0;
    for (uint32_t i = 0; i < count; ++i) if (!strcmp(s->uris[i], uri)) { result = i + 1; break; }
    if (!result && count < URIDS) {
        memcpy(s->uris[count], uri, n + 1);
        result = count + 1;
        atomic_store_explicit(&s->uri_count, result, memory_order_release);
    }
    atomic_flag_clear_explicit(&s->map_guard, memory_order_release);
    return result;
}
static const char *unmap_uri(LV2_URID_Unmap_Handle handle, LV2_URID uri) {
    lv2_live *s = handle;
    return uri && uri <= atomic_load_explicit(&s->uri_count, memory_order_acquire) ? s->uris[uri-1] : NULL;
}
static int is_port(lv2_live *s, const LilvPort *p, const char *type) {
    LilvNode *node = lilv_new_uri(s->world, type);
    int yes = lilv_port_is_a(s->descriptor, p, node); lilv_node_free(node); return yes;
}
static int property(lv2_live *s, const LilvPort *p, const char *name) {
    LilvNode *node = lilv_new_uri(s->world, name);
    int yes = lilv_port_has_property(s->descriptor, p, node); lilv_node_free(node); return yes;
}
static bool CLAP_ABI init(const clap_plugin_t *p) {
    lv2_live *s = p->plugin_data;
    return s->instance != NULL;
}
static void CLAP_ABI destroy(const clap_plugin_t *p) {
    lv2_live *s = p->plugin_data;
    if (s->instance) lilv_instance_free(s->instance);
    if (s->world) lilv_world_free(s->world);
    free(s->audio); free(s);
}
static bool CLAP_ABI activate(const clap_plugin_t *p, double rate, uint32_t lo, uint32_t hi) {
    lv2_live *s = p->plugin_data;
    if (rate != s->config.sample_rate || lo != s->config.block_size || hi != lo) return false;
    lilv_instance_activate(s->instance); return true;
}
static void CLAP_ABI deactivate(const clap_plugin_t *p) { lilv_instance_deactivate(((lv2_live *)p->plugin_data)->instance); }
static bool CLAP_ABI start(const clap_plugin_t *p) { (void)p; return true; }
static void CLAP_ABI stop(const clap_plugin_t *p) { (void)p; }
static uint32_t CLAP_ABI latency(const clap_plugin_t *p) {
    lv2_live *s = p->plugin_data;
    float value = s->latency_port < 0 ? 0 : s->control[s->latency_port];
    return isfinite(value) && value >= 0 && value < UINT32_MAX ? (uint32_t)value : 0;
}
static clap_process_status CLAP_ABI process(const clap_plugin_t *p, const clap_process_t *pr) {
    lv2_live *s = p->plugin_data;
    uint32_t frames = s->config.block_size;
    for (uint32_t ch = 0; ch < s->nin; ++ch)
        memcpy(s->audio + ch * frames, pr->audio_inputs[0].data32[ch], frames * sizeof(float));
    LV2_Atom_Sequence *seq = (LV2_Atom_Sequence *)s->midi_in.bytes;
    seq->atom.type = s->sequence; seq->atom.size = sizeof(seq->body);
    seq->body.unit = 0; seq->body.pad = 0;
    LV2_Atom *out = (LV2_Atom *)s->midi_out.bytes;
    out->type = s->chunk; out->size = ATOM_BYTES - sizeof(*out);
    for (uint32_t i = 0; i < pr->in_events->size(pr->in_events); ++i) {
        const clap_event_header_t *e = pr->in_events->get(pr->in_events, i);
        if (e->type == CLAP_EVENT_PARAM_VALUE) {
            const clap_event_param_value_t *v = (const clap_event_param_value_t *)e;
            if (e->time) return CLAP_PROCESS_ERROR; /* LV2 scalar controls are block-rate. */
            s->control[v->param_id] = (float)v->value;
        } else if (e->type == CLAP_EVENT_MIDI && s->atom_in >= 0) {
            const clap_event_midi_t *v = (const clap_event_midi_t *)e;
            struct { LV2_Atom_Event event; uint8_t data[8]; } event;
            memset(&event, 0, sizeof(event));
            event.event.time.frames = e->time; event.event.body.type = s->midi;
            event.event.body.size = (v->data[0] & 0xe0) == 0xc0 ? 2 : (v->data[0] >= 0xf8 ? 1 : 3);
            memcpy(event.data, v->data, event.event.body.size);
            if (!lv2_atom_sequence_append_event(seq, ATOM_BYTES - sizeof(LV2_Atom), &event.event)) return CLAP_PROCESS_ERROR;
        }
    }
    lilv_instance_run(s->instance, frames);
    for (uint32_t ch = 0; ch < s->nout; ++ch)
        memcpy(pr->audio_outputs[0].data32[ch], s->audio + (s->nin + ch) * frames, frames * sizeof(float));
    return CLAP_PROCESS_CONTINUE;
}
static uint32_t CLAP_ABI port_count(const clap_plugin_t *p, bool in) { (void)p; (void)in; return 1; }
static bool CLAP_ABI port_info(const clap_plugin_t *p, uint32_t i, bool in, clap_audio_port_info_t *info) {
    lv2_live *s = p->plugin_data; if (i) return false;
    memset(info, 0, sizeof(*info)); info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = in ? s->nin : s->nout; return true;
}
static uint32_t CLAP_ABI param_count(const clap_plugin_t *p) { return ((lv2_live *)p->plugin_data)->nparams; }
static bool CLAP_ABI param_info(const clap_plugin_t *p, uint32_t i, clap_param_info_t *info) {
    lv2_live *s = p->plugin_data; if (i >= s->nparams) return false;
    *info = s->info[i]; return true;
}
static uint32_t CLAP_ABI note_count(const clap_plugin_t *p, bool in) {
    lv2_live *s = p->plugin_data; return in && s->midi_input ? 1 : 0;
}
static bool CLAP_ABI note_info(const clap_plugin_t *p, uint32_t i, bool in, clap_note_port_info_t *info) {
    if (i || !note_count(p, in)) return false;
    memset(info, 0, sizeof(*info)); info->supported_dialects = CLAP_NOTE_DIALECT_MIDI; return true;
}
static const clap_plugin_audio_ports_t ports = {port_count, port_info};
static const clap_plugin_params_t params = {.count = param_count, .get_info = param_info};
static const clap_plugin_note_ports_t notes = {note_count, note_info};
static const clap_plugin_latency_t latency_ext = {latency};
static const void *CLAP_ABI extension(const clap_plugin_t *p, const char *id) {
    (void)p;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &ports;
    if (!strcmp(id, CLAP_EXT_PARAMS)) return &params;
    if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &notes;
    if (!strcmp(id, "audioplugins.audio-latency")) return &latency_ext;
    if (!strcmp(id, CLAP_EXT_LATENCY)) return &latency_ext;
    if (!strcmp(id, "audioplugins.block-parameters")) return &params;
    return NULL;
}
static void CLAP_ABI callback(const clap_plugin_t *p) { (void)p; }
static const clap_plugin_t *create(const clap_host_t *h, const char *path, const char *id, const ap_live_config *c) {
    lv2_live *s = calloc(1, sizeof(*s)); if (!s) return NULL;
    s->api = (clap_plugin_t){.plugin_data = s, .init = init, .destroy = destroy,
        .activate = activate, .deactivate = deactivate, .start_processing = start,
        .stop_processing = stop, .process = process, .get_extension = extension, .on_main_thread = callback};
    s->host = h; s->config = *c; s->atom_in = s->atom_out = s->latency_port = -1;
    atomic_init(&s->uri_count, 0); atomic_flag_clear(&s->map_guard);
    if (!atomic_is_lock_free(&s->uri_count)) goto fail;
    s->world = lilv_world_new(); if (!s->world) goto fail;
    LilvNode *search = lilv_new_string(s->world, path);
    lilv_world_set_option(s->world, LILV_OPTION_LV2_PATH, search); lilv_node_free(search);
    lilv_world_load_all(s->world);
    LilvNode *uri = lilv_new_uri(s->world, id);
    s->descriptor = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(s->world), uri); lilv_node_free(uri);
    if (!s->descriptor) goto fail;
    s->map = (LV2_URID_Map){s, map_uri}; s->unmap = (LV2_URID_Unmap){s, unmap_uri};
    s->features[0] = (LV2_Feature){LV2_URID__map, &s->map};
    s->features[1] = (LV2_Feature){LV2_URID__unmap, &s->unmap};
    s->features[2] = (LV2_Feature){LV2_BUF_SIZE__fixedBlockLength, NULL};
    s->features[3] = (LV2_Feature){LV2_BUF_SIZE__boundedBlockLength, NULL};
    for (int i = 0; i < 4; ++i) s->feature_ptrs[i] = &s->features[i];
    LilvNodes *required = lilv_plugin_get_required_features(s->descriptor);
    int supported = 1;
    LILV_FOREACH(nodes, it, required) {
        const char *name = lilv_node_as_uri(lilv_nodes_get(required, it));
        int found = 0; for (int i = 0; i < 4; ++i) if (!strcmp(name, s->features[i].URI)) found = 1;
        if (!found) supported = 0;
    }
    lilv_nodes_free(required); if (!supported) goto fail;
    s->nports = lilv_plugin_get_num_ports(s->descriptor); if (s->nports > PORTS) goto fail;
    float mins[PORTS], maxs[PORTS], defaults[PORTS];
    lilv_plugin_get_port_ranges_float(s->descriptor, mins, maxs, defaults);
    for (uint32_t i = 0; i < s->nports; ++i) {
        const LilvPort *port = lilv_plugin_get_port_by_index(s->descriptor, i);
        int input = is_port(s, port, LV2_CORE__InputPort);
        if (input == is_port(s, port, LV2_CORE__OutputPort)) goto fail;
        if (property(s, port, LV2_CORE_PREFIX "isSideChain")) goto fail;
        if (is_port(s, port, LV2_CORE__AudioPort)) {
            if (input) { if (s->nin == c->channels) goto fail; s->input[s->nin++] = i; }
            else { if (s->nout == c->channels) goto fail; s->output[s->nout++] = i; }
        } else if (is_port(s, port, LV2_CORE__ControlPort)) {
            float scale = property(s, port, LV2_CORE__sampleRate) ? (float)c->sample_rate : 1;
            s->control[i] = isfinite(defaults[i]) ? defaults[i] * scale : 0;
            if (input) {
                clap_param_info_t *info = &s->info[s->nparams]; s->params[s->nparams++] = i;
                info->id = i; info->min_value = isfinite(mins[i]) ? mins[i] * scale : -1e30;
                info->max_value = isfinite(maxs[i]) ? maxs[i] * scale : 1e30; info->default_value = s->control[i];
                if (property(s, port, LV2_CORE__integer) || property(s, port, LV2_CORE__toggled)) info->flags |= CLAP_PARAM_IS_STEPPED;
                LilvNode *name = lilv_port_get_name(s->descriptor, port);
                if (name) { strncpy(info->name, lilv_node_as_string(name), sizeof(info->name) - 1); lilv_node_free(name); }
            }
        } else if (is_port(s, port, LV2_ATOM__AtomPort)) {
            int *slot = input ? &s->atom_in : &s->atom_out;
            if (*slot >= 0) goto fail;
            LilvNode *property = lilv_new_uri(s->world, LV2_ATOM__bufferType);
            LilvNode *type = lilv_port_get(s->descriptor, port, property); lilv_node_free(property);
            int valid = type && lilv_node_is_uri(type) && !strcmp(lilv_node_as_uri(type), LV2_ATOM__Sequence); lilv_node_free(type);
            if (!valid) goto fail;
            property = lilv_new_uri(s->world, LV2_RESIZE_PORT__minimumSize);
            LilvNode *size = lilv_port_get(s->descriptor, port, property); lilv_node_free(property);
            valid = !size || lilv_node_as_float(size) <= ATOM_BYTES; lilv_node_free(size);
            if (!valid) goto fail;
            *slot = (int)i;
            LilvNode *midi = lilv_new_uri(s->world, LV2_MIDI__MidiEvent);
            if (input) s->midi_input = lilv_port_supports_event(s->descriptor, port, midi);
            lilv_node_free(midi);
        } else goto fail;
    }
    if (s->nin != c->channels || s->nout != c->channels) goto fail;
    if (lilv_plugin_has_latency(s->descriptor)) s->latency_port = (int)lilv_plugin_get_latency_port_index(s->descriptor);
    s->audio = calloc((size_t)(s->nin + s->nout) * c->block_size, sizeof(float)); if (!s->audio) goto fail;
    s->sequence = map_uri(s, LV2_ATOM__Sequence); s->midi = map_uri(s, LV2_MIDI__MidiEvent); s->chunk = map_uri(s, LV2_ATOM__Chunk);
    s->instance = lilv_plugin_instantiate(s->descriptor, c->sample_rate, s->feature_ptrs); if (!s->instance) goto fail;
    for (uint32_t i = 0; i < s->nports; ++i) lilv_instance_connect_port(s->instance, i, &s->control[i]);
    for (uint32_t i = 0; i < s->nin; ++i) lilv_instance_connect_port(s->instance, s->input[i], s->audio + i * c->block_size);
    for (uint32_t i = 0; i < s->nout; ++i) lilv_instance_connect_port(s->instance, s->output[i], s->audio + (s->nin + i) * c->block_size);
    if (s->atom_in >= 0) lilv_instance_connect_port(s->instance, (uint32_t)s->atom_in, s->midi_in.bytes);
    if (s->atom_out >= 0) lilv_instance_connect_port(s->instance, (uint32_t)s->atom_out, s->midi_out.bytes);
    if (s->latency_port >= (int)s->nports) goto fail;
    volatile unsigned char *memory = (volatile unsigned char *)s;
    for (size_t i = 0; i < sizeof(*s); i += 4096) memory[i] = memory[i];
    return &s->api;
fail:
    destroy(&s->api); return NULL;
}
int ap_live_open_lv2(const char *path, const char *id, const ap_live_config *c, ap_live **s) {
    return ap_live_open_adapter(path, id, c, s, create);
}
