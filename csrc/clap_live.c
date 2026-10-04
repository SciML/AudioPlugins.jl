#define _GNU_SOURCE
#include "clap_live.h"
#include "vendor/clap/clap.h"
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef __linux__
#error "Experimental CLAP live host currently supports Linux only"
#endif

/* No mutable global registry and no calls into the offline singleton. CLAP 1.2
 * explicitly permits nested entry.init/deinit calls by distinct hosts. */
typedef union {
    clap_event_header_t header;
    clap_event_param_value_t param;
    clap_event_midi_t midi;
} event_storage;

typedef struct {
    float *samples;
    ap_live_event events[AP_LIVE_MAX_EVENTS];
    uint32_t n;
    uint64_t tick, input_sequence;
} block;

typedef struct {
    _Atomic uint32_t read, write;
    block *slots;
} ring;

struct ap_live {
    ap_live_config config;
    pthread_t control, worker, device_thread;
    int joinable, activated, entry_initialized, device_started;
    int previous_policy;
    struct sched_param previous_priority;
    int priority_changed;
    void *module;
    const clap_plugin_entry_t *entry;
    const clap_plugin_t *plugin;
    clap_host_t host;
    clap_param_info_t *params;
    uint32_t nparams;
    int midi_input;
    ring input, output;
    float *audio;
    float *in_ptr[2], *out_ptr[2];
    clap_audio_buffer_t in_buffer, out_buffer;
    event_storage events[AP_LIVE_MAX_EVENTS];
    uint32_t nevents;
    uint64_t tick, period_ns;
    int64_t steady;
    _Atomic uint32_t state, ready, stop, restart, callback;
    _Atomic uint32_t priority_granted, blocks, underruns, overruns;
    _Atomic uint32_t deadline_misses, process_errors, output_events_dropped;
    _Atomic int32_t error;
    atomic_flag device_guard;
};

/* Thread identity is thread-local, so thread-check never races a pthread_t
 * assignment while a plugin calls it from an auxiliary thread. */
static _Thread_local ap_live *audio_session;
static void touch_pages(void *memory, size_t bytes) {
    volatile unsigned char *p = memory;
    for (size_t i = 0; i < bytes; i += 4096) p[i] = p[i];
    if (bytes) p[bytes - 1] = p[bytes - 1];
}
static uint64_t now_ns(void) {
#ifdef AP_LIVE_TEST
    extern uint64_t ap_live_test_now(void);
    return ap_live_test_now();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
#endif
}
static int control(const ap_live *s) {
    return s && pthread_equal(s->control, pthread_self());
}
static bool CLAP_ABI is_main(const clap_host_t *h) { return control(h->host_data); }
static bool CLAP_ABI is_audio(const clap_host_t *h) { return audio_session == h->host_data; }
static const clap_host_thread_check_t thread_check = { is_main, is_audio };
static const void *CLAP_ABI extension(const clap_host_t *h, const char *id) {
    (void)h;
    return !strcmp(id, CLAP_EXT_THREAD_CHECK) ? &thread_check : NULL;
}
static void CLAP_ABI request_restart(const clap_host_t *h) {
    ap_live *s = h->host_data;
    atomic_store_explicit(&s->restart, 1, memory_order_release);
    atomic_store_explicit(&s->stop, 1, memory_order_release);
}
static void CLAP_ABI request_callback(const clap_host_t *h) {
    ap_live *s = h->host_data;
    atomic_store_explicit(&s->callback, 1, memory_order_release);
}
static void CLAP_ABI request_process(const clap_host_t *h) {
    (void)h; /* Every tick processes, including CLAP_PROCESS_SLEEP. */
}
static uint32_t CLAP_ABI event_count(const clap_input_events_t *l) {
    return ((ap_live *)l->ctx)->nevents;
}
static const clap_event_header_t *CLAP_ABI event_get(const clap_input_events_t *l, uint32_t i) {
    ap_live *s = l->ctx;
    return i < s->nevents ? &s->events[i].header : NULL;
}
static bool CLAP_ABI event_push(const clap_output_events_t *l, const clap_event_header_t *e) {
    (void)e;
    ap_live *s = l->ctx;
    atomic_fetch_add_explicit(&s->output_events_dropped, 1, memory_order_relaxed);
    return false; /* No unsupported output event is reported as accepted. */
}
static void ring_free(ring *r, uint32_t n) {
    if (r->slots) for (uint32_t i = 0; i < n; ++i) free(r->slots[i].samples);
    free(r->slots);
}
static int ring_init(ring *r, uint32_t n, size_t samples) {
    r->slots = calloc(n, sizeof(block));
    if (!r->slots) return 0;
    touch_pages(r->slots, n * sizeof(block));
    for (uint32_t i = 0; i < n; ++i) {
        r->slots[i].samples = malloc(samples * sizeof(float));
        if (!r->slots[i].samples) return 0;
        memset(r->slots[i].samples, 0, samples * sizeof(float));
        touch_pages(r->slots[i].samples, samples * sizeof(float));
    }
    return 1;
}
static void dispose(ap_live *s) {
    if (s->activated && s->plugin) s->plugin->deactivate(s->plugin);
    if (s->plugin) s->plugin->destroy(s->plugin);
    if (s->entry_initialized && s->entry) s->entry->deinit();
    if (s->module) dlclose(s->module);
    ring_free(&s->input, s->config.queue_blocks);
    ring_free(&s->output, s->config.queue_blocks);
    free(s->audio);
    free(s->params);
    free(s);
}
uint32_t ap_live_abi_version(void) { return 1; }

int ap_live_open(const char *path, const char *id, const ap_live_config *c, ap_live **result) {
    if (!result) return AP_LIVE_INVALID;
    *result = NULL;
    if (!path || !id || !c || !isfinite(c->sample_rate) || c->sample_rate < 1000 ||
        c->sample_rate > 768000 || !c->block_size || c->block_size > 8192 ||
        !c->channels || c->channels > 2 || c->queue_blocks < 2 || c->queue_blocks > 1024 ||
        (c->queue_blocks & (c->queue_blocks - 1)) || c->driver > AP_LIVE_DEVICE || c->priority > 2)
        return AP_LIVE_INVALID;
    ap_live *s = calloc(1, sizeof(*s));
    if (!s) return AP_LIVE_INVALID;
    s->config = *c;
    s->control = pthread_self();
#define INIT(name) atomic_init(&s->name, 0)
    INIT(state); INIT(ready); INIT(stop); INIT(restart); INIT(callback);
    INIT(priority_granted); INIT(blocks); INIT(underruns); INIT(overruns);
    INIT(deadline_misses); INIT(process_errors); INIT(output_events_dropped); INIT(error);
#undef INIT
    atomic_init(&s->input.read, 0); atomic_init(&s->input.write, 0);
    atomic_init(&s->output.read, 0); atomic_init(&s->output.write, 0);
    atomic_flag_clear(&s->device_guard);
    /* Only 32-bit atomics enter the audio path; no libatomic locks on 32-bit. */
    if (!atomic_is_lock_free(&s->state) || !atomic_is_lock_free(&s->error)) {
        dispose(s); return AP_LIVE_UNSUPPORTED;
    }
    s->period_ns = (uint64_t)(1e9 * c->block_size / c->sample_rate);
    s->host = (clap_host_t){ CLAP_VERSION_INIT, s, "AudioPlugins Live", "JuliaHub",
        "https://github.com/SciML/AudioPlugins.jl", "0.1", extension,
        request_restart, request_process, request_callback };
    s->module = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!s->module) goto plugin_error;
    s->entry = dlsym(s->module, "clap_entry");
    if (!s->entry || !clap_version_is_compatible(s->entry->clap_version) ||
        s->entry->clap_version.major < 1 ||
        (s->entry->clap_version.major == 1 && s->entry->clap_version.minor < 2) ||
        !s->entry->init(path)) goto plugin_error;
    s->entry_initialized = 1;
    const clap_plugin_factory_t *factory = s->entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (!factory) goto plugin_error;
    s->plugin = factory->create_plugin(factory, &s->host, id);
    if (!s->plugin || !s->plugin->init(s->plugin)) goto plugin_error;
    const clap_plugin_render_t *render = s->plugin->get_extension(s->plugin, CLAP_EXT_RENDER);
    if (render && !render->set(s->plugin, CLAP_RENDER_REALTIME)) goto plugin_error;
    /* Initial adapter deliberately admits only one matching mono/stereo bus.
     * Never silently route or drop sidechains. CLAP always supports 32-bit audio. */
    const clap_plugin_audio_ports_t *ports = s->plugin->get_extension(s->plugin, CLAP_EXT_AUDIO_PORTS);
    if (!ports || ports->count(s->plugin, true) != 1 || ports->count(s->plugin, false) != 1)
        goto unsupported;
    for (int input = 0; input < 2; ++input) {
        clap_audio_port_info_t info;
        if (!ports->get(s->plugin, 0, input != 0, &info) || info.channel_count != c->channels ||
            !(info.flags & CLAP_AUDIO_PORT_IS_MAIN))
            goto unsupported;
    }
    const clap_plugin_note_ports_t *notes = s->plugin->get_extension(s->plugin, CLAP_EXT_NOTE_PORTS);
    if (notes && notes->count(s->plugin, true)) {
        clap_note_port_info_t info;
        if (notes->count(s->plugin, true) != 1 || !notes->get(s->plugin, 0, true, &info) ||
            !(info.supported_dialects & CLAP_NOTE_DIALECT_MIDI)) goto unsupported;
        s->midi_input = 1;
    }
    const clap_plugin_params_t *params = s->plugin->get_extension(s->plugin, CLAP_EXT_PARAMS);
    if (params) {
        s->nparams = params->count(s->plugin);
        if (s->nparams > 4096) goto unsupported;
        if (s->nparams) {
            s->params = calloc(s->nparams, sizeof(*s->params));
            if (!s->params) goto plugin_error;
        }
        for (uint32_t i = 0; i < s->nparams; ++i)
            if (!params->get_info(s->plugin, i, s->params + i)) goto plugin_error;
    }
    size_t samples = (size_t)c->channels * c->block_size;
    if (!ring_init(&s->input, c->queue_blocks, samples) ||
        !ring_init(&s->output, c->queue_blocks, samples)) goto plugin_error;
    s->audio = malloc(2 * samples * sizeof(float));
    if (!s->audio) goto plugin_error;
    memset(s->audio, 0, 2 * samples * sizeof(float));
    touch_pages(s->audio, 2 * samples * sizeof(float));
    for (uint32_t ch = 0; ch < c->channels; ++ch) {
        s->in_ptr[ch] = s->audio + ch * c->block_size;
        s->out_ptr[ch] = s->audio + samples + ch * c->block_size;
    }
    s->in_buffer.data32 = s->in_ptr; s->in_buffer.channel_count = c->channels;
    s->out_buffer.data32 = s->out_ptr; s->out_buffer.channel_count = c->channels;
    touch_pages(s, sizeof(*s));
    *result = s;
    return AP_LIVE_OK;
unsupported:
    dispose(s); return AP_LIVE_UNSUPPORTED;
plugin_error:
    dispose(s); return AP_LIVE_PLUGIN;
}

static const clap_param_info_t *param_info(const ap_live *s, uint32_t id) {
    for (uint32_t i = 0; i < s->nparams; ++i) if (s->params[i].id == id) return s->params + i;
    return NULL;
}
int ap_live_try_write(ap_live *s, const float *samples, const ap_live_event *events,
                      uint32_t n, uint64_t sequence) {
    if (!s || !samples || n > AP_LIVE_MAX_EVENTS || (n && !events) || sequence == UINT64_MAX)
        return AP_LIVE_INVALID;
    if (!control(s)) return AP_LIVE_THREAD;
    if (atomic_load(&s->stop) || atomic_load(&s->restart) ||
        atomic_load(&s->state) == AP_LIVE_FINISHED) return AP_LIVE_STATE;
    for (uint32_t i = 0; i < n; ++i) {
        const ap_live_event *e = events + i;
        if (e->frame >= s->config.block_size || (i && e->frame < events[i-1].frame))
            return AP_LIVE_INVALID;
        if (e->type == AP_LIVE_PARAM) {
            const clap_param_info_t *p = param_info(s, e->param_id);
            if (!p || (p->flags & CLAP_PARAM_IS_READONLY) || !isfinite(e->value) ||
                e->value < p->min_value || e->value > p->max_value ||
                ((p->flags & CLAP_PARAM_IS_STEPPED) && trunc(e->value) != e->value))
                return AP_LIVE_INVALID;
        } else if (e->type != AP_LIVE_MIDI || !s->midi_input) return AP_LIVE_UNSUPPORTED;
    }
    uint32_t w = atomic_load_explicit(&s->input.write, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&s->input.read, memory_order_acquire);
    if (w - r == s->config.queue_blocks) return AP_LIVE_AGAIN;
    block *b = &s->input.slots[w & (s->config.queue_blocks - 1)];
    memcpy(b->samples, samples, sizeof(float) * s->config.channels * s->config.block_size);
    if (n) memcpy(b->events, events, n * sizeof(*events));
    b->n = n; b->input_sequence = sequence;
    atomic_store_explicit(&s->input.write, w + 1, memory_order_release);
    return AP_LIVE_OK;
}
int ap_live_try_read(ap_live *s, float *samples, uint64_t *tick, uint64_t *sequence) {
    if (!s || !samples || !tick || !sequence) return AP_LIVE_INVALID;
    if (!control(s)) return AP_LIVE_THREAD;
    uint32_t r = atomic_load_explicit(&s->output.read, memory_order_relaxed);
    uint32_t w = atomic_load_explicit(&s->output.write, memory_order_acquire);
    if (r == w) return AP_LIVE_AGAIN;
    block *b = &s->output.slots[r & (s->config.queue_blocks - 1)];
    memcpy(samples, b->samples, sizeof(float) * s->config.channels * s->config.block_size);
    *tick = b->tick; *sequence = b->input_sequence;
    atomic_store_explicit(&s->output.read, r + 1, memory_order_release);
    return AP_LIVE_OK;
}

static int set_priority(ap_live *s) {
    atomic_store(&s->priority_granted, 0);
    if (!s->config.priority) return AP_LIVE_OK;
    int denied = pthread_getschedparam(pthread_self(), &s->previous_policy, &s->previous_priority);
    struct sched_param p = { .sched_priority = sched_get_priority_min(SCHED_FIFO) };
#ifdef AP_LIVE_TEST
    extern int ap_live_test_deny_priority(void);
    if (ap_live_test_deny_priority()) denied = 1;
#endif
    if (!denied) denied = pthread_setschedparam(pthread_self(), SCHED_FIFO, &p);
    if (denied) return s->config.priority == 2 ? AP_LIVE_PRIORITY : AP_LIVE_OK;
    s->priority_changed = 1;
    atomic_store(&s->priority_granted, 1);
    return AP_LIVE_OK;
}
static void restore_priority(ap_live *s) {
    if (s->priority_changed) {
        if (pthread_setschedparam(pthread_self(), s->previous_policy, &s->previous_priority))
            atomic_store(&s->error, AP_LIVE_PRIORITY);
        s->priority_changed = 0;
    }
}
static int audio_begin(ap_live *s) {
    int result = set_priority(s);
    audio_session = s;
    if (result == AP_LIVE_OK && !s->plugin->start_processing(s->plugin)) result = AP_LIVE_PLUGIN;
    audio_session = NULL;
    if (result) {
        restore_priority(s);
        atomic_store(&s->error, result);
        atomic_store(&s->state, AP_LIVE_FINISHED);
    }
    return result;
}
static void audio_end(ap_live *s) {
    audio_session = s;
    s->plugin->stop_processing(s->plugin);
    audio_session = NULL;
    restore_priority(s);
    atomic_store_explicit(&s->state, AP_LIVE_FINISHED, memory_order_release);
}
static int process_block(ap_live *s, float *device_output) {
    uint32_t frames = s->config.block_size, channels = s->config.channels;
    size_t bytes = sizeof(float) * frames * channels;
    if (atomic_load_explicit(&s->stop, memory_order_acquire)) {
        if (device_output) memset(device_output, 0, bytes);
        return AP_LIVE_STOPPED;
    }
    uint64_t before = now_ns(), sequence = UINT64_MAX;
    uint32_t r = atomic_load_explicit(&s->input.read, memory_order_relaxed);
    uint32_t w = atomic_load_explicit(&s->input.write, memory_order_acquire);
    s->nevents = 0;
    if (r == w) {
        memset(s->audio, 0, bytes);
        atomic_fetch_add_explicit(&s->underruns, 1, memory_order_relaxed);
    } else {
        block *b = &s->input.slots[r & (s->config.queue_blocks - 1)];
        sequence = b->input_sequence;
        for (uint32_t ch = 0; ch < channels; ++ch)
            for (uint32_t i = 0; i < frames; ++i) s->in_ptr[ch][i] = b->samples[i * channels + ch];
        s->nevents = b->n;
        for (uint32_t i = 0; i < b->n; ++i) {
            ap_live_event *e = b->events + i;
            event_storage *v = s->events + i;
            memset(v, 0, sizeof(*v));
            v->header.time = e->frame;
            v->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            if (e->type == AP_LIVE_PARAM) {
                v->header.size = sizeof(v->param); v->header.type = CLAP_EVENT_PARAM_VALUE;
                v->param.param_id = e->param_id; v->param.value = e->value;
                v->param.note_id = -1; v->param.port_index = -1;
                v->param.channel = -1; v->param.key = -1;
            } else {
                v->header.size = sizeof(v->midi); v->header.type = CLAP_EVENT_MIDI;
                memcpy(v->midi.data, e->midi, 3);
            }
        }
        atomic_store_explicit(&s->input.read, r + 1, memory_order_release);
    }
    memset(s->audio + frames * channels, 0, bytes);
    s->in_buffer.constant_mask = 0; s->out_buffer.constant_mask = 0;
    clap_input_events_t in = { s, event_count, event_get };
    clap_output_events_t out = { s, event_push };
    clap_process_t process = { .steady_time = s->steady, .frames_count = frames,
        .audio_inputs = &s->in_buffer, .audio_outputs = &s->out_buffer,
        .audio_inputs_count = 1, .audio_outputs_count = 1, .in_events = &in, .out_events = &out };
    audio_session = s;
    clap_process_status status = s->plugin->process(s->plugin, &process);
    audio_session = NULL;
    int result = AP_LIVE_OK;
    if (status == CLAP_PROCESS_ERROR) {
        memset(s->audio + frames * channels, 0, bytes);
        atomic_fetch_add_explicit(&s->process_errors, 1, memory_order_relaxed);
        atomic_store(&s->error, AP_LIVE_PLUGIN);
        atomic_store(&s->stop, 1);
        result = AP_LIVE_PLUGIN;
    }
    w = atomic_load_explicit(&s->output.write, memory_order_relaxed);
    r = atomic_load_explicit(&s->output.read, memory_order_acquire);
    block *b = w - r == s->config.queue_blocks ? NULL : &s->output.slots[w & (s->config.queue_blocks - 1)];
    for (uint32_t ch = 0; ch < channels; ++ch) for (uint32_t i = 0; i < frames; ++i) {
        float value = s->out_ptr[ch][i];
        if (b) b->samples[i * channels + ch] = value;
        if (device_output) device_output[i * channels + ch] = value;
    }
    if (b) {
        b->tick = s->tick; b->input_sequence = sequence;
        atomic_store_explicit(&s->output.write, w + 1, memory_order_release);
    } else atomic_fetch_add_explicit(&s->overruns, 1, memory_order_relaxed);
    ++s->tick;
    s->steady = s->steady > INT64_MAX - frames ? 0 : s->steady + frames;
    atomic_fetch_add_explicit(&s->blocks, 1, memory_order_relaxed);
    if (now_ns() - before > s->period_ns)
        atomic_fetch_add_explicit(&s->deadline_misses, 1, memory_order_relaxed);
    return result;
}

static void *timer_main(void *arg) {
    ap_live *s = arg;
    int result = audio_begin(s);
    atomic_store_explicit(&s->ready, 1, memory_order_release);
    if (result) return NULL;
    uint64_t next = now_ns();
    while (!atomic_load_explicit(&s->stop, memory_order_acquire)) {
        if (process_block(s, NULL) != AP_LIVE_OK) break;
        next += s->period_ns;
        uint64_t now = now_ns();
        /* Do not spin trying to catch up after a scheduling stall. */
        if (now > next) next = now + s->period_ns;
        struct timespec target = { (time_t)(next / 1000000000u), (long)(next % 1000000000u) };
        int err;
        while ((err = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, NULL)) == EINTR) {}
        if (err) { atomic_store(&s->error, AP_LIVE_STATE); break; }
        if (now_ns() - next > s->period_ns)
            atomic_fetch_add_explicit(&s->deadline_misses, 1, memory_order_relaxed);
    }
    audio_end(s);
    return NULL;
}
int ap_live_start(ap_live *s) {
    if (!control(s)) return AP_LIVE_THREAD;
    if (s->joinable || s->activated || atomic_load(&s->state) != AP_LIVE_PREPARED ||
        atomic_load(&s->restart)) return AP_LIVE_STATE;
    atomic_store(&s->stop, 0); atomic_store(&s->ready, 0); atomic_store(&s->error, 0);
    if (!s->plugin->activate(s->plugin, s->config.sample_rate, s->config.block_size, s->config.block_size))
        return AP_LIVE_PLUGIN;
    s->activated = 1;
    atomic_store(&s->state, AP_LIVE_RUNNING);
    if (s->config.driver == AP_LIVE_DEVICE) return AP_LIVE_OK;
    if (pthread_create(&s->worker, NULL, timer_main, s)) {
        s->plugin->deactivate(s->plugin); s->activated = 0;
        atomic_store(&s->state, AP_LIVE_PREPARED);
        return AP_LIVE_STATE;
    }
    s->joinable = 1;
    struct timespec pause = { 0, 1000000 };
    while (!atomic_load_explicit(&s->ready, memory_order_acquire)) nanosleep(&pause, NULL);
    int result = atomic_load(&s->error);
    if (result) ap_live_stop(s);
    return result;
}
int ap_live_stop(ap_live *s) {
    if (!control(s)) return AP_LIVE_THREAD;
    if (!s->activated) {
        atomic_store(&s->input.read, 0); atomic_store(&s->input.write, 0);
        atomic_store(&s->output.read, 0); atomic_store(&s->output.write, 0);
        return AP_LIVE_OK;
    }
    atomic_store_explicit(&s->stop, 1, memory_order_release);
    if (s->config.driver == AP_LIVE_DEVICE) {
        if (atomic_flag_test_and_set_explicit(&s->device_guard, memory_order_acquire)) return AP_LIVE_AGAIN;
        int busy = s->device_started;
        atomic_flag_clear_explicit(&s->device_guard, memory_order_release);
        if (busy) return AP_LIVE_AGAIN;
    } else if (s->joinable) {
        pthread_join(s->worker, NULL);
        s->joinable = 0;
    }
    s->plugin->deactivate(s->plugin); s->activated = 0;
    atomic_store(&s->input.read, 0); atomic_store(&s->input.write, 0);
    atomic_store(&s->output.read, 0); atomic_store(&s->output.write, 0);
    atomic_store(&s->state, AP_LIVE_PREPARED);
    atomic_store(&s->stop, 0);
    return AP_LIVE_OK;
}
int ap_live_close(ap_live *s) {
    if (!control(s)) return AP_LIVE_THREAD;
    if (s->activated || s->joinable) return AP_LIVE_STATE;
    dispose(s);
    return AP_LIVE_OK;
}
int ap_live_poll(ap_live *s) {
    if (!control(s)) return AP_LIVE_THREAD;
    if (atomic_load_explicit(&s->restart, memory_order_acquire)) {
        int result = ap_live_stop(s);
        if (result) return result;
        if (atomic_exchange_explicit(&s->callback, 0, memory_order_acq_rel))
            s->plugin->on_main_thread(s->plugin);
        return AP_LIVE_STOPPED;
    }
    if (atomic_exchange_explicit(&s->callback, 0, memory_order_acq_rel))
        s->plugin->on_main_thread(s->plugin);
    return AP_LIVE_OK;
}
int ap_live_get_stats(const ap_live *s, ap_live_stats *stats) {
    if (!s || !stats) return AP_LIVE_INVALID;
#define COPY(name) stats->name = atomic_load_explicit(&s->name, memory_order_relaxed)
    COPY(state); COPY(priority_granted); COPY(blocks); COPY(underruns); COPY(overruns);
    COPY(deadline_misses); COPY(process_errors); COPY(output_events_dropped); COPY(error);
#undef COPY
    stats->restart_requested = atomic_load_explicit(&s->restart, memory_order_relaxed);
    stats->callback_requested = atomic_load_explicit(&s->callback, memory_order_relaxed);
    return AP_LIVE_OK;
}
#ifdef AP_LIVE_TEST
/* Force unsigned index rollover in the source probe without billions of ticks. */
void ap_live_test_seed(ap_live *s, uint32_t index) {
    atomic_store(&s->input.read, index); atomic_store(&s->input.write, index);
    atomic_store(&s->output.read, index); atomic_store(&s->output.write, index);
}
#endif
int ap_live_device_begin(ap_live *s) {
    if (!s || s->config.driver != AP_LIVE_DEVICE) return AP_LIVE_INVALID;
    if (control(s)) return AP_LIVE_THREAD;
    if (atomic_flag_test_and_set_explicit(&s->device_guard, memory_order_acquire)) return AP_LIVE_AGAIN;
    int result = AP_LIVE_STATE;
    if (!s->device_started && atomic_load(&s->state) == AP_LIVE_RUNNING && !atomic_load(&s->stop)) {
        result = audio_begin(s);
        if (!result) { s->device_started = 1; s->device_thread = pthread_self(); }
    }
    atomic_flag_clear_explicit(&s->device_guard, memory_order_release);
    return result;
}
int ap_live_device_process(ap_live *s, float *output) {
    if (!s || s->config.driver != AP_LIVE_DEVICE) return AP_LIVE_INVALID;
    if (atomic_flag_test_and_set_explicit(&s->device_guard, memory_order_acquire)) return AP_LIVE_AGAIN;
    int result = !s->device_started ? AP_LIVE_STATE :
        !pthread_equal(s->device_thread, pthread_self()) ? AP_LIVE_THREAD : process_block(s, output);
    atomic_flag_clear_explicit(&s->device_guard, memory_order_release);
    return result;
}
int ap_live_device_end(ap_live *s) {
    if (!s || s->config.driver != AP_LIVE_DEVICE) return AP_LIVE_INVALID;
    if (atomic_flag_test_and_set_explicit(&s->device_guard, memory_order_acquire)) return AP_LIVE_AGAIN;
    int result = AP_LIVE_STATE;
    if (s->device_started) {
        result = AP_LIVE_THREAD;
        if (pthread_equal(s->device_thread, pthread_self())) {
            audio_end(s); s->device_started = 0; result = AP_LIVE_OK;
        }
    }
    atomic_flag_clear_explicit(&s->device_guard, memory_order_release);
    return result;
}
