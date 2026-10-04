/* miniaudio supplies native device transport, not the processing scheduler.
 * Each device block clocks an independent fixed-identity processing worker. */
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include "vendor/miniaudio.h"
#include "live_device.h"
#if defined(__APPLE__) && __has_include(<os/workgroup.h>)
#define AP_WORKGROUP 1
#include <CoreAudio/CoreAudio.h>
#include <os/workgroup.h>
#endif
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    _Atomic uint32_t read, write;
    float *samples;
    uint64_t *sequences;
} device_ring;
struct ap_audio_device {
    ma_context context;
    ma_device device;
    ma_semaphore wake;
    ap_live_config config;
    device_ring capture_ring, playback_ring;
    float *staging, *worker_capture, *playback;
    uint32_t offset, device_period, device_rate;
    uint64_t clock;
    int capture, context_ready, device_ready, semaphore_ready;
    _Atomic uint32_t closing, lost, underruns, overruns;
    atomic_flag callback_guard;
    _Atomic uint32_t workgroup_joined;
#ifdef AP_WORKGROUP
    os_workgroup_t workgroup;
    os_workgroup_join_token_s workgroup_token;
#endif
};
static void pretouch(void *memory, size_t bytes) {
    volatile unsigned char *p = memory;
    for (size_t i = 0; i < bytes; i += 4096) p[i] = p[i];
}
static int backend_context(const char *name, ma_context *context) {
    ma_backend backend;
    if (!name || !strcmp(name, "default")) {
#ifdef _WIN32
        backend = ma_backend_wasapi;
#elif defined(__APPLE__)
        backend = ma_backend_coreaudio;
#else
        /* Try real Linux APIs only. No null fallback on a headless machine. */
        ma_backend backends[] = {ma_backend_pulseaudio, ma_backend_alsa, ma_backend_jack};
        return ma_context_init(backends, 3, NULL, context) == MA_SUCCESS;
#endif
    } else if (!strcmp(name, "null")) backend = ma_backend_null;
    else if (!strcmp(name, "alsa")) backend = ma_backend_alsa;
    else if (!strcmp(name, "pulse")) backend = ma_backend_pulseaudio;
    else if (!strcmp(name, "jack")) backend = ma_backend_jack;
    else if (!strcmp(name, "coreaudio")) backend = ma_backend_coreaudio;
    else if (!strcmp(name, "wasapi")) backend = ma_backend_wasapi;
    else return 0;
    return ma_context_init(&backend, 1, NULL, context) == MA_SUCCESS;
}
int ap_live_audio_devices(const char *backend, int capture, int index, char *name, uint32_t capacity) {
    ma_context context;
    if (!backend_context(backend, &context)) return AP_LIVE_UNSUPPORTED;
    ma_device_info *playback, *recording; ma_uint32 nplayback, nrecording;
    int result = AP_LIVE_STATE;
    if (ma_context_get_devices(&context, &playback, &nplayback, &recording, &nrecording) == MA_SUCCESS) {
        ma_device_info *list = capture ? recording : playback;
        ma_uint32 count = capture ? nrecording : nplayback;
        result = (int)count;
        if (index >= 0) {
            if ((ma_uint32)index >= count || !name || !capacity) result = AP_LIVE_INVALID;
            else { size_t length = strlen(list[index].name); if (length >= capacity) result = AP_LIVE_INVALID;
                else memcpy(name, list[index].name, length + 1); }
        }
    }
    ma_context_uninit(&context); return result;
}
static void notification(const ma_device_notification *notification) {
    ap_audio_device *d = notification->pDevice->pUserData;
    if (notification->type == ma_device_notification_type_stopped ||
        notification->type == ma_device_notification_type_interruption_began ||
        notification->type == ma_device_notification_type_rerouted) {
        if (!atomic_load_explicit(&d->closing, memory_order_acquire)) {
            atomic_store_explicit(&d->lost, 1, memory_order_release);
            ma_semaphore_release(&d->wake);
        }
    }
}
/* Bounded frame reblocking: callbacks may have any frame count. Capture and
 * playback use the same device timeline and miniaudio's configured rate.
 * Output is due exactly two blocks later. Late results are discarded instead
 * of accumulating latency after a slow plugin/OS scheduling interruption. */
static void callback(ma_device *device, void *output, const void *input, ma_uint32 frames) {
    ap_audio_device *d = device->pUserData;
    uint32_t channels = d->config.channels, block = d->config.block_size;
    float *out = output; const float *in = input;
    if (atomic_flag_test_and_set_explicit(&d->callback_guard, memory_order_acquire)) {
        memset(out, 0, (size_t)frames * channels * sizeof(float));
        atomic_fetch_add_explicit(&d->underruns, 1, memory_order_relaxed); return;
    }
    for (uint32_t frame = 0; frame < frames; ++frame) {
        if (!d->offset) {
            uint32_t r = atomic_load_explicit(&d->playback_ring.read, memory_order_relaxed);
            uint32_t w = atomic_load_explicit(&d->playback_ring.write, memory_order_acquire);
            int found = 0;
            while (r != w) {
                uint32_t slot = r & (d->config.queue_blocks - 1);
                uint64_t due = d->playback_ring.sequences[slot];
                if (due > d->clock) break;
                if (due == d->clock) {
                    memcpy(d->playback, d->playback_ring.samples + (size_t)slot * block * channels,
                        (size_t)block * channels * sizeof(float)); found = 1;
                }
                ++r;
                if (found) break;
            }
            atomic_store_explicit(&d->playback_ring.read, r, memory_order_release);
            if (!found) {
                memset(d->playback, 0, (size_t)block * channels * sizeof(float));
                if (d->clock >= 2) atomic_fetch_add_explicit(&d->underruns, 1, memory_order_relaxed);
            }
        }
        for (uint32_t ch = 0; ch < channels; ++ch) {
            uint32_t position = d->offset * channels + ch;
            d->staging[position] = in ? in[frame * channels + ch] : 0;
            out[frame * channels + ch] = d->playback[position];
        }
        if (++d->offset == block) {
            uint32_t w = atomic_load_explicit(&d->capture_ring.write, memory_order_relaxed);
            uint32_t r = atomic_load_explicit(&d->capture_ring.read, memory_order_acquire);
            if (w - r < d->config.queue_blocks) {
                uint32_t slot = w & (d->config.queue_blocks - 1);
                memcpy(d->capture_ring.samples + (size_t)slot * block * channels, d->staging,
                    (size_t)block * channels * sizeof(float));
                d->capture_ring.sequences[slot] = d->clock;
                atomic_store_explicit(&d->capture_ring.write, w + 1, memory_order_release);
                ma_semaphore_release(&d->wake);
            } else atomic_fetch_add_explicit(&d->overruns, 1, memory_order_relaxed);
            d->offset = 0; ++d->clock;
        }
    }
    atomic_flag_clear_explicit(&d->callback_guard, memory_order_release);
}
static int ring_init(device_ring *r, const ap_live_config *c) {
    atomic_init(&r->read, 0); atomic_init(&r->write, 0);
    size_t samples = (size_t)c->queue_blocks * c->block_size * c->channels;
    r->samples = calloc(samples, sizeof(float)); r->sequences = calloc(c->queue_blocks, sizeof(uint64_t));
    if (!r->samples || !r->sequences) return 0;
    pretouch(r->samples, samples * sizeof(float));
    pretouch(r->sequences, c->queue_blocks * sizeof(uint64_t));
    return 1;
}
void ap_audio_close(ap_audio_device *d) {
    if (!d) return;
    atomic_store(&d->closing, 1);
#ifdef AP_WORKGROUP
    if (__builtin_available(macOS 11.0, *)) { if (d->workgroup) os_release(d->workgroup); }
#endif
    if (d->device_ready) ma_device_uninit(&d->device);
    if (d->context_ready) ma_context_uninit(&d->context);
    if (d->semaphore_ready) ma_semaphore_uninit(&d->wake);
    free(d->capture_ring.samples); free(d->capture_ring.sequences);
    free(d->playback_ring.samples); free(d->playback_ring.sequences);
    free(d->staging); free(d->worker_capture); free(d->playback); free(d);
}
ap_audio_device *ap_audio_open(const ap_live_config *c, const char *backend, int capture, int playback_index, int capture_index) {
    ap_audio_device *d = calloc(1, sizeof(*d)); if (!d) return NULL;
    pretouch(d, sizeof(*d));
    d->config = *c; d->capture = !!capture;
    atomic_init(&d->workgroup_joined, 0);
    atomic_init(&d->closing, 0); atomic_init(&d->lost, 0);
    atomic_init(&d->underruns, 0); atomic_init(&d->overruns, 0); atomic_flag_clear(&d->callback_guard);
    if (!ring_init(&d->capture_ring, c) || !ring_init(&d->playback_ring, c)) goto fail;
    size_t samples = (size_t)c->block_size * c->channels;
    d->staging = calloc(samples, sizeof(float)); d->worker_capture = calloc(samples, sizeof(float));
    d->playback = calloc(samples, sizeof(float));
    if (!d->staging || !d->worker_capture || !d->playback) goto fail;
    pretouch(d->staging, samples * sizeof(float));
    pretouch(d->worker_capture, samples * sizeof(float));
    pretouch(d->playback, samples * sizeof(float));
    if (ma_semaphore_init(0, &d->wake) != MA_SUCCESS) goto fail;
    d->semaphore_ready = 1;
    if (!backend_context(backend, &d->context)) goto fail;
    d->context_ready = 1;
    ma_device_config config = ma_device_config_init(capture ? ma_device_type_duplex : ma_device_type_playback);
    config.playback.format = config.capture.format = ma_format_f32;
    config.playback.channels = config.capture.channels = c->channels;
    config.sampleRate = (ma_uint32)c->sample_rate; config.periodSizeInFrames = c->block_size;
    config.dataCallback = callback; config.notificationCallback = notification; config.pUserData = d;
    ma_device_info *outputs, *inputs; ma_uint32 noutputs, ninputs;
    if (playback_index >= 0 || capture_index >= 0) {
        if (ma_context_get_devices(&d->context, &outputs, &noutputs, &inputs, &ninputs) != MA_SUCCESS) goto fail;
        if (playback_index >= 0) { if ((ma_uint32)playback_index >= noutputs) goto fail; config.playback.pDeviceID = &outputs[playback_index].id; }
        if (capture && capture_index >= 0) { if ((ma_uint32)capture_index >= ninputs) goto fail; config.capture.pDeviceID = &inputs[capture_index].id; }
    }
    if (ma_device_init(&d->context, &config, &d->device) != MA_SUCCESS) goto fail;
    d->device_ready = 1;
    d->device_period = d->device.playback.internalPeriodSizeInFrames;
    d->device_rate = d->device.playback.internalSampleRate;
#ifdef AP_WORKGROUP
    if (__builtin_available(macOS 11.0, *)) {
        if (d->context.backend == ma_backend_coreaudio) {
            AudioObjectPropertyAddress address = {kAudioDevicePropertyIOThreadOSWorkgroup,
                kAudioObjectPropertyScopeGlobal, 0};
            UInt32 size = sizeof(d->workgroup);
            if (AudioObjectGetPropertyData(d->device.coreaudio.deviceObjectIDPlayback,
                &address, 0, NULL, &size, &d->workgroup) != noErr) d->workgroup = NULL;
        }
    }
#endif
    return d;
fail:
    ap_audio_close(d); return NULL;
}
int ap_audio_prepare(ap_audio_device *d) {
    /* Called only with device stopped and processing worker joined. */
    if (d->semaphore_ready) ma_semaphore_uninit(&d->wake);
    d->semaphore_ready = 0;
    if (ma_semaphore_init(0, &d->wake) != MA_SUCCESS) return AP_LIVE_STATE;
    d->semaphore_ready = 1;
    d->offset = 0; d->clock = 0;
    atomic_store(&d->capture_ring.read, 0); atomic_store(&d->capture_ring.write, 0);
    atomic_store(&d->playback_ring.read, 0); atomic_store(&d->playback_ring.write, 0);
    atomic_store(&d->closing, 0); atomic_store(&d->lost, 0);
    return AP_LIVE_OK;
}
int ap_audio_start(ap_audio_device *d) {
    return ma_device_start(&d->device) == MA_SUCCESS ? AP_LIVE_OK : AP_LIVE_STATE;
}
void ap_audio_wake(ap_audio_device *d) {
    atomic_store_explicit(&d->closing, 1, memory_order_release);
    ma_semaphore_release(&d->wake);
}
void ap_audio_stop(ap_audio_device *d) {
    atomic_store_explicit(&d->closing, 1, memory_order_release);
    ma_device_stop(&d->device); /* Joins/quiesces native callbacks, off audio. */
    ma_semaphore_release(&d->wake);
}
int ap_audio_wait(ap_audio_device *d, const float **capture, uint64_t *sequence) {
    for (;;) {
        if (ma_semaphore_wait(&d->wake) != MA_SUCCESS) return AP_LIVE_STATE;
        if (atomic_load_explicit(&d->lost, memory_order_acquire)) return AP_LIVE_STATE;
        if (atomic_load_explicit(&d->closing, memory_order_acquire)) return AP_LIVE_STOPPED;
        uint32_t r = atomic_load_explicit(&d->capture_ring.read, memory_order_relaxed);
        uint32_t w = atomic_load_explicit(&d->capture_ring.write, memory_order_acquire);
        if (r != w) {
            uint32_t slot = r & (d->config.queue_blocks - 1);
            size_t samples = (size_t)d->config.block_size * d->config.channels;
            memcpy(d->worker_capture, d->capture_ring.samples + slot * samples, samples * sizeof(float));
            *capture = d->capture ? d->worker_capture : NULL;
            *sequence = d->capture_ring.sequences[slot];
            atomic_store_explicit(&d->capture_ring.read, r + 1, memory_order_release);
            return AP_LIVE_OK;
        }
    }
}
void ap_audio_submit(ap_audio_device *d, const float *samples, uint64_t sequence) {
    uint32_t w = atomic_load_explicit(&d->playback_ring.write, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&d->playback_ring.read, memory_order_acquire);
    if (w - r == d->config.queue_blocks) { atomic_fetch_add_explicit(&d->overruns, 1, memory_order_relaxed); return; }
    uint32_t slot = w & (d->config.queue_blocks - 1);
    size_t n = (size_t)d->config.block_size * d->config.channels;
    memcpy(d->playback_ring.samples + slot * n, samples, n * sizeof(float));
    d->playback_ring.sequences[slot] = sequence + 2;
    atomic_store_explicit(&d->playback_ring.write, w + 1, memory_order_release);
}
void ap_audio_stats(ap_audio_device *d, ap_device_stats *s) {
    s->underruns = atomic_load(&d->underruns); s->overruns = atomic_load(&d->overruns); s->lost = atomic_load(&d->lost);
    s->workgroup_joined = atomic_load(&d->workgroup_joined);
    s->buffering_frames = 2 * d->config.block_size; s->capture = d->capture;
    s->device_period_frames = d->device_period;
    s->device_sample_rate = d->device_rate; s->backend = d->context.backend;
}

int ap_audio_enter(ap_audio_device *d) {
#ifdef AP_WORKGROUP
    if (d->context.backend == ma_backend_coreaudio) {
        if (__builtin_available(macOS 11.0, *)) {
            if (!d->workgroup || os_workgroup_join(d->workgroup, &d->workgroup_token)) return -1;
            atomic_store(&d->workgroup_joined, 1);
        } else return -1;
    }
#else
    (void)d;
#endif
    return 0;
}
void ap_audio_leave(ap_audio_device *d) {
#ifdef AP_WORKGROUP
    if (__builtin_available(macOS 11.0, *)) {
        if (atomic_exchange(&d->workgroup_joined, 0)) os_workgroup_leave(d->workgroup, &d->workgroup_token);
    }
#else
    (void)d;
#endif
}

#ifdef AP_LIVE_TEST
void ap_audio_test_loss(ap_audio_device *d) {
    ma_device_notification event = {0}; event.pDevice = &d->device;
    event.type = ma_device_notification_type_stopped; notification(&event);
}
#endif
