/* Deterministic transport test: call the real device callback with irregular
 * frame counts. No audio hardware, sleeps, or scheduling deadlines. */
#include "../csrc/live_device.c"
#include <assert.h>
#include <stdio.h>
int main(void) {
    ap_audio_device *d = calloc(1, sizeof(*d)); assert(d);
    d->config = (ap_live_config){48000, 32, 2, 4, AP_LIVE_HARDWARE, 0}; d->capture = 1;
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
    ma_device_notification event = {0}; event.pDevice = &d->device;
    event.type = ma_device_notification_type_stopped; notification(&event);
    assert(ap_audio_wait(d, &capture, &sequence) == AP_LIVE_STATE);
    atomic_store(&d->lost, 0); ap_audio_wake(d);
    assert(ap_audio_wait(d, &capture, &sequence) == AP_LIVE_STOPPED);
    ap_audio_close(d);
    puts("device reblocking, capture, rollover, late output, loss and stop: passed");
    return 0;
}
