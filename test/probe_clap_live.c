#define _POSIX_C_SOURCE 200809L
#include "../csrc/clap_live.h"
#include <assert.h>
#include "../csrc/live_platform.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static _Thread_local int audio_alloc_guard;
#ifndef AP_LIVE_NO_ALLOC_WRAP
void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t m);
void *__real_realloc(void *p, size_t n);
void __real_free(void *p);
void *__wrap_malloc(size_t n) { assert(!audio_alloc_guard); return __real_malloc(n); }
void *__wrap_calloc(size_t n, size_t m) { assert(!audio_alloc_guard); return __real_calloc(n, m); }
void *__wrap_realloc(void *p, size_t n) { assert(!audio_alloc_guard); return __real_realloc(p, n); }
void __wrap_free(void *p) { assert(!audio_alloc_guard); __real_free(p); }
#endif
static _Atomic int fake_clock, deny_priority;
static _Atomic uint64_t fake_time;
uint64_t ap_live_test_now(void) {
    if (atomic_load(&fake_clock)) return atomic_fetch_add(&fake_time, 10000000);
    return ap_clock_ns();
}
int ap_live_test_deny_priority(void) { return atomic_load(&deny_priority); }
static void pause_ms(long ms) {
    ap_sleep_ns((uint64_t)ms * 1000000);
}
static ap_live_config config = { 48000, 32, 2, 4, AP_LIVE_DEVICE, 0 };
static ap_live *open_session(const char *path, const char *id) {
    ap_live *s = NULL; assert(ap_live_open(path, id, &config, &s) == 0); assert(s); return s;
}
/* Device emulator: command handshakes belong only to the test, never the host.
 * Each command executes on the same native thread, with no wall-clock deadline. */
typedef struct { ap_live *s; _Atomic int command; int result; float output[64]; } device;
static void *device_run(void *arg) {
    device *d = arg;
    for (;;) {
        int cmd = atomic_load_explicit(&d->command, memory_order_acquire);
        if (!cmd) { pause_ms(1); continue; }
        audio_alloc_guard = 1;
        if (cmd == 1) d->result = ap_live_device_begin(d->s);
        if (cmd == 2) d->result = ap_live_device_process(d->s, d->output);
        if (cmd == 3) d->result = ap_live_device_end(d->s);
        if (cmd == 4) { /* Concurrent/control access rejected before plugin calls. */
            assert(ap_live_close(d->s) == AP_LIVE_THREAD);
            assert(ap_live_stop(d->s) == AP_LIVE_THREAD);
            assert(ap_live_poll(d->s) == AP_LIVE_THREAD);
            assert(ap_live_try_write(d->s, d->output, NULL, 0, 0) == AP_LIVE_THREAD);
            d->result = 0;
        }
        audio_alloc_guard = 0;
        atomic_store_explicit(&d->command, 0, memory_order_release);
        if (cmd == 3) return NULL;
    }
}
static int command(device *d, int cmd) {
    atomic_store_explicit(&d->command, cmd, memory_order_release);
    while (atomic_load_explicit(&d->command, memory_order_acquire)) pause_ms(1);
    return d->result;
}
static void device_tests(const char *path) {
    ap_live *s = open_session(path, "ap.live");
    extern void ap_live_test_seed(ap_live *, uint32_t);
    ap_live_test_seed(s, UINT32_MAX - 1);
    float input[64], output[64];
    for (int i = 0; i < 64; ++i) input[i] = (i % 2) ? 2 : 1;
    uint64_t tick, sequence;
    ap_live_event events[] = {
        { .type = AP_LIVE_PARAM, .frame = 0, .param_id = 0, .value = 0.5 },
        { .type = AP_LIVE_PARAM, .frame = 16, .param_id = 0, .value = 2 }
    };
    events[0].value = NAN;
    assert(ap_live_try_write(s, input, events, 2, 0) == AP_LIVE_INVALID);
    events[0].value = 0.5;
    assert(ap_live_try_write(s, input, NULL, 1, 0) == AP_LIVE_INVALID);
    assert(ap_live_try_read(s, output, &tick, &sequence) == AP_LIVE_AGAIN);
    assert(ap_live_try_write(s, input, NULL, 0, 0) == 0);
    assert(ap_live_stop(s) == 0); /* Prepared sessions discard queued data too. */
    ap_live_test_seed(s, UINT32_MAX - 1);
    for (unsigned i = 0; i < 4; ++i) assert(ap_live_try_write(s, input, events, 2, i) == 0);
    assert(ap_live_try_write(s, input, events, 2, 4) == AP_LIVE_AGAIN);
    assert(ap_live_start(s) == 0);
    assert(ap_live_start(s) == AP_LIVE_STATE);
    assert(ap_live_close(s) == AP_LIVE_STATE);
    assert(ap_live_device_begin(s) == AP_LIVE_THREAD);
    device d = { .s = s }; ap_thread thread;
    assert(!ap_thread_create(&thread, device_run, &d));
    assert(command(&d, 1) == 0);
    assert(command(&d, 4) == 0);
    assert(ap_live_device_process(s, output) == AP_LIVE_THREAD);
    for (unsigned block = 0; block < 4; ++block) {
        assert(command(&d, 2) == 0);
        assert(ap_live_try_read(s, output, &tick, &sequence) == 0);
        assert(tick == block && sequence == block);
        for (int i = 0; i < 64; ++i) {
            assert(output[i] == input[i] * (i < 32 ? 0.5 : 2));
            assert(output[i] == d.output[i]);
        }
    }
    /* Wrap slots repeatedly, with sequence and both channels checked. */
    for (unsigned block = 4; block < 1004; ++block) {
        assert(ap_live_try_write(s, input, NULL, 0, block) == 0);
        assert(command(&d, 2) == 0);
        assert(ap_live_try_read(s, output, &tick, &sequence) == 0);
        assert(tick == block && sequence == block);
        for (int i = 0; i < 64; ++i) assert(output[i] == 2 * input[i]);
    }
    ap_live_event midi = { .type = AP_LIVE_MIDI, .frame = 8, .midi = { 0x90, 60, 127 } };
    assert(ap_live_try_write(s, input, &midi, 1, 1004) == 0);
    atomic_store(&fake_clock, 1);
    assert(command(&d, 2) == 0);
    atomic_store(&fake_clock, 0);
    assert(ap_live_try_read(s, output, &tick, &sequence) == 0);
    for (int i = 0; i < 64; ++i) assert(output[i] == input[i] * (i < 16 ? 2 : 1));
    for (int i = 0; i < 6; ++i) assert(command(&d, 2) == 0);
    for (int i = 0; i < 4; ++i) {
        assert(ap_live_try_read(s, output, &tick, &sequence) == 0);
        assert(tick == 1005u + i && sequence == UINT64_MAX);
        for (int j = 0; j < 64; ++j) assert(output[j] == 0);
    }
    assert(command(&d, 2) == 0);
    assert(ap_live_try_read(s, output, &tick, &sequence) == 0 && tick == 1011);
    ap_live_stats stats; assert(ap_live_get_stats(s, &stats) == 0);
    assert(stats.underruns == 7 && stats.overruns == 2 && stats.blocks == 1012);
    assert(stats.deadline_misses >= 1 && stats.output_events_dropped == 9);
    assert(stats.callback_requested);
    assert(ap_live_poll(s) == 0);
    assert(ap_live_get_stats(s, &stats) == 0 && !stats.callback_requested);
    assert(ap_live_stop(s) == AP_LIVE_AGAIN);
    assert(command(&d, 2) == AP_LIVE_STOPPED);
    assert(command(&d, 3) == 0); ap_thread_join(thread);
    assert(ap_live_stop(s) == 0);
    assert(ap_live_try_read(s, output, &tick, &sequence) == AP_LIVE_AGAIN);
    assert(ap_live_close(s) == 0);
}
static void delay_test(const char *path) {
    ap_live *s = open_session(path, "ap.delay");
    assert(ap_live_start(s) == 0);
    device d = { .s = s }; ap_thread thread;
    assert(!ap_thread_create(&thread, device_run, &d));
    assert(command(&d, 1) == 0);
    for (unsigned b = 0; b < 2; ++b) {
        float input[64];
        for (unsigned i = 0; i < 64; ++i) input[i] = b * 32 + i / 2 + 1;
        assert(ap_live_try_write(s, input, NULL, 0, b) == 0);
        assert(command(&d, 2) == 0);
        for (unsigned i = 0; i < 64; ++i) {
            int frame = (int)(b * 32 + i / 2);
            assert(d.output[i] == (frame < 16 ? 0 : frame - 15));
        }
    }
    assert(command(&d, 3) == 0); ap_thread_join(thread);
    assert(ap_live_stop(s) == 0); assert(ap_live_close(s) == 0);
}
static void timer_tests(const char *path) {
    config.driver = AP_LIVE_TIMER;
    ap_live *s = open_session(path, "ap.live");
    ap_live *peer = open_session(path, "ap.live");
    assert(ap_live_start(peer) == 0);
    for (int run = 0; run < 5; ++run) {
        assert(ap_live_start(s) == 0);
        ap_live_stats before, after;
        assert(ap_live_get_stats(s, &before) == 0);
        /* Allow native progress while the caller does no work; no timing bound
         * is asserted, only eventual progress with a generous test timeout. */
        for (int i = 0; i < 1000; ++i) {
            pause_ms(2); assert(ap_live_get_stats(s, &after) == 0);
            if (after.blocks - before.blocks >= 10) break;
        }
        assert(after.blocks - before.blocks >= 10);
        assert(after.underruns > 0 && after.overruns > 0);
        assert(ap_live_poll(s) == 0);
        assert(ap_live_stop(s) == 0);
    }
    assert(ap_live_close(s) == 0);
    assert(ap_live_stop(peer) == 0); assert(ap_live_close(peer) == 0);
    /* Actual concurrent ring access, independent of the deterministic device
     * emulator: a timer consumes while control publishes and reads in bursts. */
    s = open_session(path, "ap.live");
    assert(ap_live_start(s) == 0);
    unsigned sent = 0, received = 0;
    uint64_t previous_tick = 0;
    int have_tick = 0;
    for (unsigned spin = 0; spin < 10000 && sent < 1000; ++spin) {
        float input[64], output[64];
        for (unsigned burst = 0; burst < 8 && sent < 1000; ++burst) {
            for (unsigned i = 0; i < 64; ++i) input[i] = (float)(sent % 32);
            int result = ap_live_try_write(s, input, NULL, 0, sent);
            if (result == AP_LIVE_AGAIN) break;
            assert(result == 0); ++sent;
        }
        uint64_t tick, sequence;
        while (ap_live_try_read(s, output, &tick, &sequence) == 0) {
            if (have_tick) assert(tick > previous_tick);
            previous_tick = tick; have_tick = 1;
            float expected = sequence == UINT64_MAX ? 0 : (float)(sequence % 32);
            for (unsigned i = 0; i < 64; ++i) assert(output[i] == expected);
            ++received;
        }
        assert(ap_live_poll(s) == 0);
        pause_ms(1);
    }
    assert(sent == 1000 && received > 0);
    assert(ap_live_stop(s) == 0); assert(ap_live_close(s) == 0);
    const char *fails[] = { "ap.startfail", "ap.activatefail" };
    for (int i = 0; i < 2; ++i) {
        s = open_session(path, fails[i]);
        for (int j = 0; j < 2; ++j) assert(ap_live_start(s) == AP_LIVE_PLUGIN);
        assert(ap_live_close(s) == 0);
    }
    s = open_session(path, "ap.processfail");
    int result = ap_live_start(s); assert(result == 0 || result == AP_LIVE_PLUGIN);
    ap_live_stats stats;
    for (int i = 0; i < 1000; ++i) {
        assert(ap_live_get_stats(s, &stats) == 0);
        if (stats.process_errors) break;
        pause_ms(2);
    }
    assert(stats.process_errors == 1 && stats.error == AP_LIVE_PLUGIN);
    assert(ap_live_stop(s) == 0); assert(ap_live_close(s) == 0);
    s = open_session(path, "ap.restart");
    assert(ap_live_start(s) == 0);
    for (int i = 0; i < 1000; ++i) {
        assert(ap_live_get_stats(s, &stats) == 0);
        if (stats.restart_requested) break;
        pause_ms(2);
    }
    assert(stats.restart_requested);
    assert(ap_live_poll(s) == AP_LIVE_STOPPED);
    assert(ap_live_start(s) == AP_LIVE_STATE);
    assert(ap_live_close(s) == 0);
    atomic_store(&deny_priority, 1);
    for (unsigned priority = 1; priority <= 2; ++priority) {
        config.priority = priority;
        s = open_session(path, "ap.live");
        assert(ap_live_start(s) == (priority == 1 ? 0 : AP_LIVE_PRIORITY));
        assert(ap_live_get_stats(s, &stats) == 0 && !stats.priority_granted);
        assert(ap_live_stop(s) == 0); assert(ap_live_close(s) == 0);
    }
}
int main(int argc, char **argv) {
    assert(argc == 2 && ap_live_abi_version() == 1);
    ap_live *s = NULL;
    assert(ap_live_open(argv[1], "missing", &config, &s) == AP_LIVE_PLUGIN && !s);
    assert(ap_live_open(argv[1], "ap.initfail", &config, &s) == AP_LIVE_PLUGIN && !s);
    config.channels = 1;
    assert(ap_live_open(argv[1], "ap.live", &config, &s) == AP_LIVE_UNSUPPORTED && !s);
    config.channels = 2; config.queue_blocks = 3;
    assert(ap_live_open(argv[1], "ap.live", &config, &s) == AP_LIVE_INVALID && !s);
    config.queue_blocks = 4;
    device_tests(argv[1]); delay_test(argv[1]); timer_tests(argv[1]);
    puts("CLAP live probes passed");
    return 0;
}
