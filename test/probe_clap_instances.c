/* Concurrently live real CLAP effects, linked to the shipped host sources. */
#include "../csrc/clap_host.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static double process(double h, double t, double value) {
    return clap_process_for(h, t, 0, value, -1, 0, -1, 0, -1, 0);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const char *bundle = argv[1];
    assert(clap_host_open(bundle, "ap.gain", 48000, 32, 1) == 0);
    double gain = clap_host_open_instance(bundle, "ap.gain", 48000, 32, 2);
    double pole = clap_host_open_instance(bundle, "ap.onepole", 48000, 32, 2);
    double other = clap_host_open_instance(bundle, "ap.gain", 48000, 32, 2);
    assert(isfinite(gain) && isfinite(pole) && isfinite(other));
    assert(gain != pole && gain != other);
    assert(clap_host_is_open() == 1);
    assert(strcmp(clap_host_plugin_name(), "AudioPlugins Test Gain") == 0);
    double legacy = clap_in_tone(32.0/48000, CLAP_WAVE_SQUARE, 0, 1);
    legacy = clap_process(legacy, 0, 3, -1, 0, -1, 0, -1, 0);
    assert(clap_out_sample(legacy, 0, 0) == 3);
    assert(clap_out_sample(legacy + 0.25, 0, 0) == 3);
    assert(strcmp(clap_host_plugin_name_for(gain), "AudioPlugins Test Gain") == 0);
    assert(clap_host_n_params_for(pole) == 1);
    assert(clap_host_param_max_for(gain, 0) == 4);
    assert(clap_host_param_max_for(pole, 0) == 1);

    double input[64];
    for (int i = 0; i < 32; i++) { input[2*i] = 1; input[2*i+1] = 2; }
    double prev = NAN, last = NAN;
    for (int block = 0; block < 3; block++) {
        double in = clap_in_fill_for(gain, input, 32, 2);
        double out = process(gain, in, 0.5);
        assert(isfinite(out));
        assert(isnan(process(pole, in, 0.25))); /* foreign input */
        assert(isnan(clap_out_sample_for(pole, out, 0, 0))); /* foreign output */
        assert(isnan(clap_in_copy_for(pole, gain, prev))); /* stale output */
        double dep = clap_in_copy_for(pole, gain, out);
        last = process(pole, dep, 0.25);
        for (int i = 0; i < 32; i++) {
            double expected = 0.5 * (1 - pow(0.75, block*32+i+1));
            assert(fabs(clap_out_sample_for(pole, last, i, 0) - expected) < 1e-6);
            assert(fabs(clap_out_sample_for(pole, last, i, 1) - 2*expected) < 1e-6);
        }
        assert(clap_host_n_process_for(gain) == block + 1);
        assert(clap_host_n_process_for(pole) == block + 1);
        double t = clap_set_param_for(other, clap_in_fill_for(other, input, 32, 2), 0, 2);
        double o = clap_process_for(other, t, -1, 0, -1, 0, -1, 0, -1, 0);
        assert(clap_out_sample_for(other, o, 0, 0) == 2);
        assert(clap_host_param_value_for(gain, 0) == 0.5);
        assert(clap_host_param_value_for(other, 0) == 2);
        prev = out;
    }

    /* Legacy discovery/lifecycle and failed opens leave managed effects alone. */
    assert(clap_host_scan(bundle) == 3);
    assert(clap_host_open(bundle, "ap.lookahead", 48000, 32, 1) == 0);
    assert(clap_host_latency() == 16);
    clap_host_close();
    assert(isnan(clap_host_open_instance(bundle, "missing", 48000, 32, 1)));
    assert(isnan(clap_host_open_instance("/no/such/plugin.clap", "", 48000, 32, 1)));
    assert(isnan(clap_host_open_instance(bundle, "ap.gain", 48000, 9000, 1)));
    assert(isnan(clap_host_open_instance(bundle, "ap.gain", INFINITY, 32, 1)));
    assert(isnan(clap_host_open_instance(bundle, "ap.gain", 48000, NAN, 1)));
    assert(isnan(clap_host_open_instance(bundle, "ap.gain", 48000, 32, INFINITY)));
    assert(isnan(clap_host_open_instance(bundle, "ap.gain", 48000, 32.5, 1)));
    assert(clap_out_valid_for(pole, last) == 1);
    assert(clap_host_is_open_for(gain) == 1);
    for (int kind = 0; kind < 3; kind++) {
        double mismatch = clap_host_open_instance(bundle, "ap.gain",
            kind == 0 ? 44100 : 48000, kind == 1 ? 64 : 32, kind == 2 ? 1 : 2);
        assert(isfinite(mismatch));
        assert(isnan(clap_in_copy_for(mismatch, pole, last)));
        clap_host_close_instance(mismatch);
    }
    assert(isnan(clap_in_copy_for(gain, pole, last - 0.25)));
    assert(isnan(process(gain, NAN, 1)));
    assert(isnan(process(INFINITY, 1, 1)));
    assert(isnan(process(gain + 0.25, 1, 1)));
    clap_host_close_instance(gain);
    clap_host_close_instance(gain);
    assert(clap_host_is_open_for(gain) == 0);
    assert(clap_out_valid_for(gain, prev) == 0);
    assert(isnan(clap_in_copy_for(pole, gain, prev)));
    assert(clap_out_valid_for(pole, last) == 1);
    clap_host_close_instance(other);
    /* Last surviving plugin can still process after every peer closes. */
    assert(isfinite(process(pole, clap_in_fill_for(pole, input, 32, 2), 0.5)));
    clap_host_close_instance(pole);
    for (int i = 0; i < 100; i++) {
        double fresh = clap_host_open_instance(bundle, "ap.onepole", 48000, 32, 2);
        assert(fresh != pole && fresh != gain);
        assert(isnan(process(pole, last, 1)));
        double out = process(fresh, clap_in_fill_for(fresh, input, 32, 2), 0.25);
        assert(clap_out_sample_for(fresh, out, 0, 0) == 0.25);
        clap_host_close_instance(fresh);
    }
    puts("CLAP instance chain, isolation, lifetime and stale-handle checks passed");
    return 0;
}
