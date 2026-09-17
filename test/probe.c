/* probe.c -- C-level verification of the CLAP host, hosting our own
 * bundle. Every expectation here is arithmetic, not a recording.
 *
 * Build and run from the repository root, against the same sources the JLL
 * is built from:
 *
 *   cc -O2 -fPIC -shared -o ap_test.clap test/plugins/ap_test_plugins.c
 *   cc -O2 -fPIC -shared -o ap_many.clap test/plugins/ap_test_many.c
 *   cc -O2 -fPIC -shared -o ap_manyparams.clap test/plugins/ap_test_manyparams.c
 *   cc -O2 -o probe test/probe.c csrc/clap_host.c -ldl -lm
 *   ./probe ./ap_test.clap /lib/x86_64-linux-gnu/libm.so.6 ./ap_many.clap ./ap_manyparams.clap
 *
 * argv[1] is the bundle (or the path `clap_test_bundle()` returns from
 * Julia); argv[2] to argv[4] are optional -- a shared object that is not a
 * plugin, the many-plugin bundle of test/plugins/ap_test_many.c, and the
 * eight-parameter bundle of test/plugins/ap_test_manyparams.c. */

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
    ck(clap_host_scan_count() == 3, "scan_count agrees with what scan returned");

    /* The rest of the descriptor. */
    ck(strcmp(clap_host_scan_vendor(0), "JuliaHub") == 0, "descriptor 0 vendor");
    ck(strcmp(clap_host_scan_version(0), "0.1.0") == 0, "descriptor 0 version");
    ck(strcmp(clap_host_scan_description(0), "out = in * gain") == 0,
       "descriptor 0 description");
    ck(clap_host_scan_n_features(0) == 1, "descriptor 0 has one feature");
    ck(strcmp(clap_host_scan_feature(0, 0), "audio-effect") == 0, "  and it is audio-effect");
    ck(strcmp(clap_host_scan_feature(0, 1), "") == 0, "out-of-range feature is empty");
    ck(clap_host_scan_n_features(9) == 0, "out-of-range descriptor has no features");

    /* --- failure paths, all loud ----------------------------------- */
    ck(clap_host_scan("/nonexistent.clap") == -1, "missing bundle fails");
    ck(strlen(clap_host_last_error()) > 0, "  ... and sets an error message");
    ck(clap_host_scan_count() == 0, "  ... and leaves no descriptors behind");
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

    /* --- a bundle with more plugins than any fixed cache would hold ---
     * Last, because scanning it replaces the descriptor cache everything
     * above reads. A host that stopped at 32 reported a 400-plugin bundle
     * as a 32-plugin bundle, with nothing to say it had. */
    if (argc > 3) {
        const char *MANY = argv[3];
        long m = clap_host_scan(MANY);
        ck(m == 100, "scan finds all 100 plugins, not a capped 32");
        ck(clap_host_scan_count() == m, "  scan_count agrees");
        ck(strcmp(clap_host_scan_id(0), "ap.many.000") == 0, "  first id");
        ck(strcmp(clap_host_scan_id(99), "ap.many.099") == 0, "  hundredth id");
        ck(strcmp(clap_host_scan_name(99), "AudioPlugins Many 099") == 0, "  and its name");
        ck(strcmp(clap_host_scan_id(100), "") == 0, "  one past the end is empty");

        /* Plugin 0 sets every optional field; its neighbours set none. */
        ck(strcmp(clap_host_scan_vendor(0), "AudioPlugins Test Vendor") == 0, "  vendor");
        ck(strcmp(clap_host_scan_version(0), "4.5.6") == 0, "  version");
        ck(strcmp(clap_host_scan_description(0),
                  "every optional descriptor field, set") == 0, "  description");
        ck(strcmp(clap_host_scan_vendor(1), "") == 0, "  an unset field reads as empty");

        /* 18 features declared, CLAP_HOST_MAX_FEATURES kept: truncation is
         * the documented behaviour, and it must not eat the neighbours. */
        ck(clap_host_scan_n_features(0) == CLAP_HOST_MAX_FEATURES,
           "  an over-long feature list truncates to the cache size");
        ck(strcmp(clap_host_scan_feature(0, 0), "audio-effect") == 0, "  feature 0 kept");
        ck(strcmp(clap_host_scan_feature(0, 1), "stereo") == 0, "  feature 1 kept");
        ck(clap_host_scan_n_features(1) == 1, "  the next plugin still has its own");

        /* Found is not the same as usable: open one from past the old cap. */
        ck(clap_host_open(MANY, "ap.many.099", 48000, 64, 1) == 0,
           "  open the hundredth plugin");
        double buf[64];
        for (int i = 0; i < 64; i++) buf[i] = 0.5;
        double tok = clap_process(clap_in_fill(buf, 64, 1), -1, 0, -1, 0, -1, 0, -1, 0);
        ck(clap_out_sample(tok, 7, 0) == 0.5, "  and it passes audio through");
        clap_host_close();

        /* Scanning back down to a small bundle must not report the big
         * one's count: the cache is grown, not refilled in place. */
        ck(clap_host_scan(BUNDLE) == 3, "  rescanning the small bundle finds 3 again");
        ck(strcmp(clap_host_scan_id(3), "") == 0, "  with nothing left over from the big one");
    }

    /* --- more parameters than clap_process has slots ---------------- *
     * The chain is what a generated per-plugin component uses, and the
     * fixture's powers-of-two weights are what make a mis-addressed
     * parameter distinguishable from a mis-valued one. */
    if (argc > 4) {
        const char *MP = argv[4];
        ck(clap_host_open(MP, "ap.weights", 48000, 8, 1) == 0, "open the 8-parameter plugin");
        ck(clap_host_n_params() == 8, "  it reports eight parameters");
        ck(clap_host_open_index() == 0, "  and knows which descriptor it is");

        double buf[8];
        for (int i = 0; i < 8; i++) buf[i] = 1.0;

        double want = 0.0, tok = clap_in_fill(buf, 8, 1);
        for (int k = 0; k < 8; k++) {
            want += (double)(1u << k) * ((double)k / 16.0);
            tok = clap_set_param(tok, k, (double)k / 16.0);
        }
        double out = clap_process(tok, -1, 0, -1, 0, -1, 0, -1, 0);
        ck(fabs(clap_out_sample(out, 3, 0) - want) < 1e-5,
           "  a chain of eight drives all eight");

        /* Held: the second block queues nothing and must produce the same. */
        tok = clap_in_fill(buf, 8, 1);
        for (int k = 0; k < 8; k++) tok = clap_set_param(tok, k, (double)k / 16.0);
        out = clap_process(tok, -1, 0, -1, 0, -1, 0, -1, 0);
        ck(fabs(clap_out_sample(out, 3, 0) - want) < 1e-5,
           "  holding them queues no event and changes nothing");

        /* The chain and the four slots compose. Reopened first, because the
         * blocks above left seven parameters set and this check is about
         * which mechanism drove which id, not about what preceded it. */
        clap_host_open(MP, "ap.weights", 48000, 8, 1);
        tok = clap_set_param(clap_in_fill(buf, 8, 1), 7, 0.5);
        out = clap_process(tok, 0, 1.0, 1, 1.0, -1, 0, -1, 0);
        ck(fabs(clap_out_sample(out, 0, 0) - (1.0 + 2.0 + 128.0 * 0.5)) < 1e-5,
           "  the chain and the slots compose");

        /* Refusals, and that a refusal poisons the rest of the chain. */
        tok = clap_in_fill(buf, 8, 1);
        ck(isnan(clap_set_param(tok, 8, 0.5)), "  an unknown parameter id is refused");
        ck(isnan(clap_set_param(tok, -1, 0.5)), "  a negative id is refused");
        ck(isnan(clap_set_param(tok, 0, NAN)), "  a NaN value is refused");
        ck(isnan(clap_set_param(tok + 1, 0, 0.5)), "  a stale block is refused");
        ck(isnan(clap_set_param(NAN, 0, 0.5)), "  and NaN in is NaN out");
        ck(isnan(clap_process(clap_set_param(NAN, 0, 0.5), -1, 0, -1, 0, -1, 0, -1, 0)),
           "  a refused link poisons the process it feeds");

        /* A chain that is never processed does not leak into the next block:
         * from a fresh instance, whose parameters are all zero, so a leak
         * would show up as a non-zero output. */
        clap_host_open(MP, "ap.weights", 48000, 8, 1);
        tok = clap_in_fill(buf, 8, 1);
        clap_set_param(tok, 0, 1.0);
        out = clap_process(clap_in_fill(buf, 8, 1), -1, 0, -1, 0, -1, 0, -1, 0);
        ck(clap_out_sample(out, 0, 0) == 0.0, "  an abandoned chain is dropped at the block");
        clap_host_close();

        /* --- clap_expect: the wrong plugin is refused, not processed --- */
        ck(clap_host_open(BUNDLE, "ap.onepole", 48000, 8, 1) == 0, "open ap.onepole to guard it");
        ck(clap_host_open_index() == 1, "  it is descriptor 1");
        tok = clap_in_fill(buf, 8, 1);
        ck(clap_expect(tok, 1) == tok, "  clap_expect passes the token through");
        ck(isnan(clap_expect(tok, 0)), "  and refuses a different descriptor");
        ck(isnan(clap_expect(NAN, 1)), "  NaN in, NaN out");
        ck(isnan(clap_process(clap_expect(tok, 0), 0, 0.25, -1, 0, -1, 0, -1, 0)),
           "  the refusal reaches the output");
        clap_host_close();
        ck(clap_host_open_index() == -1, "  and nothing is open afterwards");
    }

    printf("\n%s (%d failure%s)\n", fails ? "FAILURES" : "ALL PROBES PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
