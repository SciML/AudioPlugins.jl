/* probe.c -- C-level verification of the CLAP host, hosting our own
 * bundle. Every expectation here is arithmetic, not a recording.
 *
 * Build and run from the repository root, against the same sources the JLL
 * is built from:
 *
 *   cc -O2 -fPIC -shared -o /tmp/ap_test.clap test/plugins/ap_test_plugins.c
 *   cc -O2 -o /tmp/probe test/probe.c csrc/clap_host.c -ldl -lm
 *   /tmp/probe /tmp/ap_test.clap
 *
 * (or pass the path `clap_test_bundle()` returns from Julia). */

#include "../csrc/clap_host.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
static void ck(int ok, const char *what) {
    printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

int main(int argc, char **argv) {
    const char *BUNDLE = argc > 1 ? argv[1] : "ap_test.clap";
    /* --- discovery ------------------------------------------------- */
    long n = clap_host_scan(BUNDLE);
    ck(n == 3, "scan finds 3 plugins in the bundle");
    ck(strcmp(clap_host_scan_id(0), "ap.gain") == 0, "descriptor 0 is ap.gain");
    ck(strcmp(clap_host_scan_id(1), "ap.onepole") == 0, "descriptor 1 is ap.onepole");
    ck(strcmp(clap_host_scan_id(2), "ap.lookahead") == 0, "descriptor 2 is ap.lookahead");
    ck(strcmp(clap_host_scan_id(9), "") == 0, "out-of-range descriptor is empty");

    /* --- failure paths, all loud ----------------------------------- */
    ck(clap_host_scan("/nonexistent.clap") == -1, "missing bundle fails");
    ck(strlen(clap_host_last_error()) > 0, "  ... and sets an error message");
    if (argc > 2) {   /* optionally: a real shared object that is not a plugin */
        ck(clap_host_scan(argv[2]) == -1, "a shared object without clap_entry fails");
        ck(strstr(clap_host_last_error(), "clap_entry") != NULL,
           "  ... naming the missing clap_entry symbol");
    }
    ck(clap_host_open(BUNDLE, "no.such.id", 48000, 64, 1) != 0, "unknown plugin id fails");
    ck(strstr(clap_host_last_error(), "no.such.id") != NULL, "  ... naming the id asked for");
    ck(clap_host_open(BUNDLE, "ap.gain", 48000, 99999, 1) != 0, "oversized block fails");
    ck(clap_host_open(BUNDLE, "ap.gain", 48000, 64, 7) != 0, "too many channels fails");
    ck(clap_host_open(BUNDLE, "ap.gain", -1, 64, 1) != 0, "non-positive sample rate fails");
    ck(clap_host_is_open() == 0.0, "state after a failed open is closed");

    /* --- open, and the configuration actually in force -------------- */
    ck(clap_host_open(BUNDLE, "ap.gain", 48000, 64, 1) == 0, "open ap.gain succeeds");
    ck(clap_host_is_open() == 1.0, "is_open");
    ck(clap_host_block_size() == 64.0, "block size in force is 64");
    ck(clap_host_sample_rate() == 48000.0, "sample rate in force is 48000");
    ck(strcmp(clap_host_plugin_name(), "AudioPlugins Test Gain") == 0, "plugin name reported");

    /* --- parameters ------------------------------------------------- */
    ck(clap_host_n_params() == 1, "gain exposes 1 parameter");
    ck(clap_host_param_id(0) == 0.0, "  its id is 0");
    ck(clap_host_param_min(0) == 0.0 && clap_host_param_max(0) == 4.0, "  range 0..4");
    ck(clap_host_param_default(0) == 1.0, "  default 1.0");
    ck(strcmp(clap_host_param_name(0), "Gain") == 0, "  named \"Gain\"");
    ck(clap_host_param_id(5) == -1.0, "out-of-range parameter id is -1");

    /* --- the arithmetic: out == in * gain, sample-exactly ------------ */
    double tok = clap_in_tone(1.0, CLAP_WAVE_SINE, 1000.0, 0.5);
    ck(!isnan(tok), "input block generated");
    double out = clap_process(tok, 0, 0.5, -1, 0, -1, 0, -1, 0);
    ck(!isnan(out), "process returns an output token");
    ck(clap_host_n_process() == 1, "exactly one process() call");

    int exact = 1;
    for (long i = 0; i < 64; i++) {
        double x = clap_in_sample(tok, (double)i, 0);
        double y = clap_out_sample(out, (double)i, 0);
        /* float storage, so compare at float precision */
        if (fabsf((float)(x * 0.5) - (float)y) > 1e-7f) { exact = 0; break; }
    }
    ck(exact, "out[i] == in[i] * 0.5 for every sample in the block");
    ck(fabs(clap_out_rms(out) - clap_in_tone(1.0, CLAP_WAVE_SINE, 1000.0, 0.25) * 0.0
            - 0.0) >= 0.0, "(rms readable)");

    /* --- stale tokens are refused, not answered --------------------- */
    double tok2 = clap_in_tone(2.0, CLAP_WAVE_SINE, 1000.0, 0.5);
    double out2 = clap_process(tok2, 0, 0.5, -1, 0, -1, 0, -1, 0);
    ck(isnan(clap_out_sample(out, 0, 0)), "a superseded output token reads NaN");
    ck(isnan(clap_in_sample(tok, 0, 0)), "a superseded input token reads NaN");
    ck(!isnan(clap_out_sample(out2, 0, 0)), "the current token still reads");
    ck(isnan(clap_process(tok, 0, 0.5, -1, 0, -1, 0, -1, 0)),
       "process with a stale dep is refused");

    /* --- a parameter change takes effect on the intended block ------- */
    double t3 = clap_in_tone(3.0, CLAP_WAVE_SQUARE, 500.0, 1.0);
    double o3 = clap_process(t3, 0, 2.0, -1, 0, -1, 0, -1, 0);
    ck(fabs(clap_out_peak(o3) - 2.0) < 1e-6, "gain 2.0 doubles a full-scale square");
    double t4 = clap_in_tone(4.0, CLAP_WAVE_SQUARE, 500.0, 1.0);
    double o4 = clap_process(t4, 0, 0.25, -1, 0, -1, 0, -1, 0);
    ck(fabs(clap_out_peak(o4) - 0.25) < 1e-6, "  and 0.25 quarters it on the next block");
    ck(fabs(clap_host_param_value(0) - 0.25) < 1e-9,
       "the plugin's own value reflects what was sent");
    ck(isnan(clap_host_param_value(77)), "an unknown parameter id reads NaN");

    /* --- latency reporting ------------------------------------------ */
    ck(clap_host_latency() == 0.0, "gain reports no latency");
    ck(clap_host_open(BUNDLE, "ap.lookahead", 48000, 64, 1) == 0, "open ap.lookahead");
    ck(clap_host_latency() == 16.0, "lookahead reports 16 samples of latency");
    ck(clap_host_n_params() == 0, "lookahead exposes no parameters");

    /* --- THE test: state persists across blocks ---------------------
     * Two consecutive 32-frame blocks through the one-pole must equal one
     * 64-frame run over the concatenated input. If the plugin's state were
     * reset per block, or if the host re-activated between blocks, the
     * second half would restart from zero and this fails. */
    double split[64], whole[64];
    ck(clap_host_open(BUNDLE, "ap.onepole", 48000, 32, 1) == 0, "open ap.onepole at 32");
    double step[32];
    for (int i = 0; i < 32; i++) step[i] = 1.0;      /* unit step */
    for (int b = 0; b < 2; b++) {
        double t = clap_in_fill(step, 32, 1);
        double o = clap_process(t, 0, 0.25, -1, 0, -1, 0, -1, 0);
        for (int i = 0; i < 32; i++) split[b * 32 + i] = clap_out_sample(o, i, 0);
    }
    ck(clap_host_open(BUNDLE, "ap.onepole", 48000, 64, 1) == 0, "reopen ap.onepole at 64");
    double step64[64];
    for (int i = 0; i < 64; i++) step64[i] = 1.0;
    double t64 = clap_in_fill(step64, 64, 1);
    double o64 = clap_process(t64, 0, 0.25, -1, 0, -1, 0, -1, 0);
    for (int i = 0; i < 64; i++) whole[i] = clap_out_sample(o64, i, 0);

    double worst = 0.0;
    for (int i = 0; i < 64; i++) {
        double d = fabs(split[i] - whole[i]);
        if (d > worst) worst = d;
    }
    printf("   split-vs-whole worst |difference| = %.3g   (split[31]=%.6f split[32]=%.6f)\n",
           worst, split[31], split[32]);
    ck(worst < 1e-6, "two blocks == one concatenated run (state persists)");
    /* And the closed form: a one-pole step response is 1-(1-a)^n. */
    double a = 0.25, expect31 = 1.0 - pow(1.0 - a, 32.0);
    ck(fabs(split[31] - expect31) < 1e-5, "step response matches 1-(1-a)^n at n=32");

    clap_host_close();
    ck(clap_host_is_open() == 0.0, "closed");

    printf("\n%s (%d failure%s)\n", fails ? "FAILURES" : "ALL PROBES PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
