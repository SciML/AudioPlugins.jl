/* Deterministic transport test: call the real device callback with irregular
 * frame counts. No audio hardware, sleeps, or scheduling deadlines. */
#include "../csrc/live_device.c"
#include <assert.h>
#include <stdio.h>
int main(void) {
    ap_audio_device *d = calloc(1, sizeof(*d)); assert(d);
    d->config = (ap_live_config){48000, 32, 2, 4, AP_LIVE_HARDWARE, 0}; d->capture = 1;
    d->pipeline_blocks = 2; d->max_callback_frames = 64;
    atomic_init(&d->wake_pending, 0); atomic_init(&d->workgroup_joined, 0);
    atomic_init(&d->closing, 0); atomic_init(&d->lost, 0);
    atomic_init(&d->underruns, 0); atomic_init(&d->overruns, 0); atomic_flag_clear(&d->callback_guard);
    assert(ring_init(&d->capture_ring, &d->config)); assert(ring_init(&d->playback_ring, &d->config));
    d->staging = calloc(64, sizeof(float)); d->worker_capture = calloc(64, sizeof(float)); d->playback = calloc(64, sizeof(float));
    assert(ma_semaphore_init(0, &d->wake) == MA_SUCCESS); d->semaphore_ready = 1;
    d->device.pUserData = d;
    /* Cross 32-bit queue-index rollover without changing sample ordering. */
    atomic_store(&d->capture_ring.read, UINT32_MAX); atomic_store(&d->capture_ring.write, UINT32_MAX);
    atomic_store(&d->playback_ring.read, UINT32_MAX); atomic_store(&d->playback_ring.write, UINT32_MAX);
    float input[128], output[128]; for (int i = 0; i < 128; ++i) input[i] = (float)i;
    callback(&d->device, output, input, 11);
    callback(&d->device, output + 22, input + 22, 21);
    for (int i = 0; i < 64; ++i) assert(output[i] == 0);
    const float *capture; uint64_t sequence;
    assert(ap_audio_wait(d, &capture, &sequence) == 0 && sequence == 0);
    for (int i = 0; i < 64; ++i) assert(capture[i] == input[i]);
    ap_audio_submit(d, capture, sequence);
    callback(&d->device, output, input + 64, 32);
    for (int i = 0; i < 64; ++i) assert(output[i] == 0);
    assert(ap_audio_wait(d, &capture, &sequence) == 0 && sequence == 1);
    ap_audio_submit(d, capture, sequence);
    callback(&d->device, output, NULL, 13);
    callback(&d->device, output + 26, NULL, 51);
    for (int i = 0; i < 128; ++i) assert(output[i] == input[i]);
    /* The worker stalls: playback stays silent and queues remain bounded. */
    for (int i = 0; i < 10; ++i) callback(&d->device, output, NULL, 32);
    assert(atomic_load(&d->underruns) == 10);
    assert(atomic_load(&d->overruns) > 0);
    assert(ap_audio_wait(d, &capture, &sequence) == 0 && sequence == 2);
    ap_audio_submit(d, input, sequence); /* Too late, must never be played. */
    callback(&d->device, output, NULL, 32);
    for (int i = 0; i < 64; ++i) assert(output[i] == 0);
    /* A negotiated 128-frame hardware period must work with 32-frame plugin
     * blocks even when the worker runs only between whole device bursts. */
    assert(ap_audio_prepare(d) == AP_LIVE_OK);
    free(d->capture_ring.samples); free(d->capture_ring.sequences);
    free(d->playback_ring.samples); free(d->playback_ring.sequences);
    d->device_period = 128; d->device_rate = 48000;
    d->device.playback.internalPeriods = 2; d->device.sampleRate = 48000;
    assert(configure_transport(d));
    assert(d->pipeline_blocks == 10 && d->config.queue_blocks == 32);
    assert(ring_init(&d->capture_ring, &d->config)); assert(ring_init(&d->playback_ring, &d->config));
    atomic_store(&d->underruns, 0); atomic_store(&d->overruns, 0);
    float burst_in[512], burst_out[512];
    for (uint32_t burst = 0; burst < 10; ++burst) {
        for (uint32_t i = 0; i < 512; ++i) burst_in[i] = (float)(burst * 512 + i + 1);
        /* Includes full two-period demand, as during hardware prefill. */
        callback(&d->device, burst_out, burst_in, 256);
        for (uint32_t i = 0; i < 512; ++i) {
            int position = (int)(burst * 512 + i) - 10 * 64;
            assert(burst_out[i] == (position < 0 ? 0 : (float)(position + 1)));
        }
        for (uint32_t b = 0; b < 8; ++b) {
            assert(ap_audio_wait(d, &capture, &sequence) == AP_LIVE_OK);
            ap_audio_submit(d, capture, sequence);
        }
    }
    assert(!atomic_load(&d->underruns) && !atomic_load(&d->overruns));
    /* Rate conversion rounds upward; unsupported buffer budgets fail. */
    d->device_rate = 44100; assert(configure_transport(d));
    assert(d->pipeline_blocks == 11);
    d->device_period = UINT32_MAX; assert(!configure_transport(d));
    /* A larger callback after negotiation is silenced and requires reopen. */
    float oversized[642];
    callback(&d->device, oversized, NULL, 321);
    for (uint32_t i = 0; i < 642; ++i) assert(oversized[i] == 0);
    assert(ap_audio_wait(d, &capture, &sequence) == AP_LIVE_STATE);
    assert(ap_audio_prepare(d) == AP_LIVE_STATE);
    atomic_store(&d->lost, 0);
    ma_device_notification event = {0}; event.pDevice = &d->device;
    event.type = ma_device_notification_type_stopped; notification(&event);
    assert(ap_audio_wait(d, &capture, &sequence) == AP_LIVE_STATE);
    atomic_store(&d->lost, 0); ap_audio_wake(d);
    assert(ap_audio_wait(d, &capture, &sequence) == AP_LIVE_STOPPED);
    ap_audio_close(d);
    puts("device reblocking, hardware bursts, capture, rollover, late output, loss and stop: passed");
    return 0;
}
