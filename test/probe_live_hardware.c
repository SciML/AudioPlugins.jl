#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "../csrc/clap_live.h"
#include "../csrc/live_device.h"
#include "../csrc/live_platform.h"
#include <assert.h>
#include <stdio.h>
#include <stdatomic.h>
uint64_t ap_live_test_now(void) { return ap_clock_ns(); }
int ap_live_test_deny_priority(void) { return 1; }
extern void ap_live_test_device_loss(ap_live *);
int main(int argc, char **argv) {
    assert(argc == 2);
    for (int capture = 0; capture <= 1; ++capture) {
        ap_live_config config = {48000, 64, 2, 4, AP_LIVE_HARDWARE, 0};
        ap_live *s = NULL;
        assert(!ap_live_open(argv[1], "ap.live", &config, &s));
        assert(!ap_live_configure_device(s, "null", capture, -1, -1));
        for (int cycle = 0; cycle < 10; ++cycle) {
            assert(!ap_live_start(s));
            float samples[128] = {0}; ap_live_stats before, after;
            assert(!ap_live_get_stats(s, &before));
            uint64_t tick, sequence;
            int progressed = 0;
            for (int i = 0; i < 5000; ++i) {
                int result = ap_live_try_write(s, samples, NULL, 0, (uint64_t)i);
                assert(result == 0 || result == AP_LIVE_AGAIN);
                result = ap_live_try_read(s, samples, &tick, &sequence);
                assert(result == 0 || result == AP_LIVE_AGAIN);
                assert(!ap_live_get_stats(s, &after));
                if (after.blocks - before.blocks >= 8) { progressed = 1; break; }
                ap_sleep_ns(1000000);
            }
            assert(progressed && !after.error);
            assert(!ap_live_stop(s));
        }
        assert(!ap_live_start(s));
        ap_live_test_device_loss(s);
        ap_live_stats stats;
        for (int i = 0; i < 5000; ++i) {
            ap_live_get_stats(s, &stats);
            if (stats.state == AP_LIVE_FINISHED) break;
            ap_sleep_ns(1000000);
        }
        assert(stats.state == AP_LIVE_FINISHED);
        assert(ap_live_poll(s) == AP_LIVE_STOPPED);
        ap_device_stats transport; assert(!ap_live_get_device_stats(s, &transport));
        assert(transport.lost);
        assert(ap_live_start(s) == AP_LIVE_STATE);
        assert(!ap_live_close(s));
        config.priority = 2;
        assert(!ap_live_open(argv[1], "ap.live", &config, &s));
        assert(!ap_live_configure_device(s, "null", capture, -1, -1));
        assert(ap_live_start(s) == AP_LIVE_PRIORITY);
        assert(!ap_live_close(s));
    }
    puts("native device worker stress, loss, stop and denied priority: passed");
    return 0;
}
