/* probe_vst3.c -- C-level verification of the VST3 host, hosting our own
 * bundle. Every expectation here is arithmetic.
 *
 *   ./probe_vst3 <path to ap_test.vst3> [<path to a real bundle, e.g. again-sample-accurate.vst3>]
 */
#include "../csrc/vst3_host.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
static void ck(int ok, const char *what) {
    printf("%-62s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

#define GAIN "41505447" "41494E00" "00000000" "00000001"
#define POLE "41505431" "504F4C45" "00000000" "00000002"
#define LOOK "41504C4F" "4F4B4148" "00000000" "00000003"

int main(int argc, char **argv) {
    const char *BUNDLE = argc > 1 ? argv[1] : "ap_test.vst3";
    double step[64];
    for (int i = 0; i < 64; i++) step[i] = 1.0;

    long n = vst3_host_scan(BUNDLE);
    ck(n == 3, "scan finds the 3 test classes");
    if (n < 0) printf("   error: %s\n", vst3_host_last_error());
    ck(strcmp(vst3_host_scan_id(0), GAIN) == 0, "class 0 id is the gain's");
    ck(strcmp(vst3_host_scan_name(0), "AudioPlugins Test Gain") == 0, "class 0 name");
    ck(strcmp(vst3_host_scan_category(1), "Fx|Filter") == 0, "class 1 subcategory");
    ck(strcmp(vst3_host_scan_id(9), "") == 0, "out-of-range class is empty");

    ck(vst3_host_scan("/nonexistent.vst3") == -1, "missing bundle fails");
    ck(strlen(vst3_host_last_error()) > 0, "  ... with a message");
    ck(vst3_host_scan("/lib/aarch64-linux-gnu/libm.so.6") == -1 ||
       vst3_host_scan("/lib/x86_64-linux-gnu/libm.so.6") == -1, "a non-VST3 shared object fails");
    ck(vst3_host_open(BUNDLE, "00000000000000000000000000000000", 48000, 64, 1) != 0, "unknown class id fails");
    ck(strstr(vst3_host_last_error(), "no audio-effect class") != NULL, "  ... naming the problem");
    ck(vst3_host_open(BUNDLE, GAIN, 48000, 99999, 1) != 0, "oversized block fails");
    ck(vst3_host_open(BUNDLE, GAIN, 48000, 64, 7) != 0, "too many channels fails");
    ck(vst3_host_open(BUNDLE, GAIN, -1, 64, 1) != 0, "non-positive sample rate fails");
    ck(vst3_host_is_open() == 0.0, "state after a failed open is closed");

    ck(vst3_host_open(BUNDLE, GAIN, 48000, 64, 1) == 0, "open gain (mono)");
    if (!vst3_host_is_open()) printf("   error: %s\n", vst3_host_last_error());
    ck(strcmp(vst3_host_plugin_name(), "AudioPlugins Test Gain") == 0, "plugin name");
    ck(vst3_host_block_size() == 64 && vst3_host_sample_rate() == 48000, "configuration in force");
    ck(vst3_host_n_params() == 1, "1 parameter");
    ck(vst3_host_param_id(0) == 0.0, "  id 0");
    ck(strcmp(vst3_host_param_name(0), "Gain") == 0, "  named Gain");
    ck(vst3_host_param_min(0) == 0.0 && vst3_host_param_max(0) == 4.0, "  plain range 0..4");
    ck(vst3_host_param_default(0) == 1.0, "  plain default 1");
    ck(vst3_host_param_default_normalized(0) == 0.25, "  normalised default 0.25");
    ck(vst3_host_param_plain(0, 0.5) == 2.0, "  plain(0.5) == 2");
    ck(vst3_host_param_normalized(0, 2.0) == 0.5, "  normalized(2) == 0.5");
    ck(isnan(vst3_host_param_value(77)), "unknown id reads NaN");

    double tok = vst3_in_tone(1.0, VST3_WAVE_SINE, 1000.0, 0.5);
    double out = vst3_process(tok, 0, 0.125, -1, 0, -1, 0, -1, 0);   /* gain 0.5 */
    ck(!isnan(out), "process returns a token");
    int exact = 1;
    for (long i = 0; i < 64; i++) {
        float x = (float)vst3_in_sample(tok, (double)i, 0), y = (float)vst3_out_sample(out, (double)i, 0);
        if (fabsf(x * 0.5f - y) > 1e-7f) { exact = 0; break; }
    }
    ck(exact, "out == in * 0.5, sample-exactly");
    ck(vst3_host_n_process() == 1, "exactly one process()");
    double t2 = vst3_in_fill(step, 64, 1);
    ck(isnan(vst3_process(tok, 0, 0.125, -1, 0, -1, 0, -1, 0)), "stale dep refused");
    double o2 = vst3_process(t2, 0, 0.5, -1, 0, -1, 0, -1, 0);       /* gain 2 */
    ck(fabs(vst3_out_peak(o2) - 2.0) < 1e-6, "gain 2 on the intended block");
    ck(isnan(vst3_out_peak(out)), "superseded output token reads NaN");
    ck(vst3_host_param_value(0) == 0.5, "the controller holds what was sent");
    ck(vst3_host_latency() == 0.0, "gain reports no latency");

    ck(vst3_host_open(BUNDLE, GAIN, 48000, 64, 2) == 0, "open gain (stereo)");
    double st[128];
    for (int i = 0; i < 64; i++) { st[2 * i] = 1.0; st[2 * i + 1] = -0.5; }
    double ts = vst3_in_fill(st, 128, 2);
    double os = vst3_process(ts, 0, 0.25, -1, 0, -1, 0, -1, 0);       /* gain 1 */
    ck(vst3_out_sample(os, 3, 0) == 1.0 && vst3_out_sample(os, 3, 1) == -0.5, "stereo channels kept apart");

    ck(vst3_host_open(BUNDLE, LOOK, 48000, 64, 1) == 0, "open lookahead");
    ck(vst3_host_latency() == 16.0, "latency 16 from getLatencySamples()");
    ck(vst3_host_n_params() == 0, "no parameters");
    double imp[64] = {0}; imp[0] = 1.0;
    double ti = vst3_in_fill(imp, 64, 1);
    double oi = vst3_process(ti, -1, 0, -1, 0, -1, 0, -1, 0);
    ck(vst3_out_sample(oi, 16, 0) == 1.0 && vst3_out_sample(oi, 15, 0) == 0.0, "impulse comes out 16 later");

    /* THE test: two 32-frame blocks == one 64-frame run. */
    double split[64], whole[64];
    ck(vst3_host_open(BUNDLE, POLE, 48000, 32, 1) == 0, "open onepole at 32");
    for (int b = 0; b < 2; b++) {
        double t = vst3_in_fill(step, 32, 1);
        double o = vst3_process(t, 0, 0.25, -1, 0, -1, 0, -1, 0);
        for (int i = 0; i < 32; i++) split[b * 32 + i] = vst3_out_sample(o, i, 0);
    }
    ck(vst3_host_open(BUNDLE, POLE, 48000, 64, 1) == 0, "reopen onepole at 64");
    double t64 = vst3_in_fill(step, 64, 1);
    double o64 = vst3_process(t64, 0, 0.25, -1, 0, -1, 0, -1, 0);
    for (int i = 0; i < 64; i++) whole[i] = vst3_out_sample(o64, i, 0);
    double worst = 0;
    for (int i = 0; i < 64; i++) worst = fmax(worst, fabs(split[i] - whole[i]));
    ck(worst < 1e-6, "two blocks == one concatenated run (state persists)");
    ck(fabs(split[31] - (1 - pow(0.75, 32))) < 1e-5, "step response 1-(1-a)^n at n=32");

    vst3_host_close();
    ck(vst3_host_is_open() == 0.0, "closed");
    ck(isnan(vst3_process(1, 0, 1, -1, 0, -1, 0, -1, 0)), "process after close is NaN");

    if (argc > 2) {   /* a real plugin from the SDK: again (sample-accurate variant) */
        long m = vst3_host_scan(argv[2]);
        printf("   %ld class(es) in %s\n", m, argv[2]);
        for (long i = 0; i < m; i++)
            printf("     %s  %s  (%s)\n", vst3_host_scan_id(i), vst3_host_scan_name(i), vst3_host_scan_category(i));
        ck(m > 0, "the real bundle enumerates");
        int r = vst3_host_open(argv[2], "", 48000, 64, 2);
        if (r != 0) printf("   error: %s\n", vst3_host_last_error());
        ck(r == 0, "the real plugin opens (stereo)");
        if (r == 0) {
            long np = vst3_host_n_params();
            long gi = -1;
            for (long i = 0; i < np; i++) if (strcmp(vst3_host_param_name(i), "Gain") == 0) gi = i;
            printf("   %ld params; Gain is index %ld id %g default %g\n", np, gi,
                   gi >= 0 ? vst3_host_param_id(gi) : -1.0, gi >= 0 ? vst3_host_param_default(gi) : NAN);
            ck(gi >= 0, "  it has a Gain parameter");
            double gid = vst3_host_param_id(gi);
            double t = vst3_in_fill(step, 64, 1);
            double o = vst3_process(t, gid, 0.5, -1, 0, -1, 0, -1, 0);
            /* AGain: output = input * normalised gain, so 0.5 -> 0.5. Give
             * a sample-accurate ramp implementation a block to settle. */
            t = vst3_in_fill(step, 64, 1);
            o = vst3_process(t, gid, 0.5, -1, 0, -1, 0, -1, 0);
            int ex = 1;
            for (int i = 0; i < 64; i++) if (fabs(vst3_out_sample(o, i, 0) - 0.5) > 1e-7) { ex = 0; break; }
            printf("   again: out[0]=%.7f out[63]=%.7f\n", vst3_out_sample(o, 0, 0), vst3_out_sample(o, 63, 0));
            ck(ex, "  again: out == in * 0.5 sample-exactly");
            ck(vst3_host_latency() == 0.0, "  again reports no latency");
        }
        vst3_host_close();
    }

    printf("\n%s (%d failure%s)\n", fails ? "FAILURES" : "ALL PROBES PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
