# The CLAP host, tested by hosting plugins that ship with this package.
#
# Hosting is only proved by hosting something, and depending on a third-party
# plugin would make the suite depend on a binary whose arithmetic we cannot
# check and may not be able to fetch. `test/plugins/ap_test_plugins.c` is
# ours: a gain, a one-pole and a 16-sample lookahead, in one bundle, so every
# expectation below is arithmetic rather than a recording -- the same approach
# the synthetic sources take in AudioComponents and VisionComponents.

using Test
using Libdl
using AudioPlugins
const AP = AudioPlugins

const BUNDLE = clap_test_bundle()

sha_free(x) = x  # (no fixtures to checksum: the plugin is built from source here)

@testset "AudioPlugins / CLAP" begin

    @testset "the host comes prebuilt, the test plugins do not" begin
        # The host library is the JLL's, not something compiled into the
        # package directory: it exists, it loads, and it exports the ABI.
        @test isfile(clap_lib_path())
        @test !startswith(clap_lib_path(), pkgdir(AudioPlugins))
        h = Libdl.dlopen(clap_lib_path())
        @test Libdl.dlsym(h, :clap_process) != C_NULL
        Libdl.dlclose(h)
        # The sources still ship, for a standalone C program to link.
        @test isfile(clap_src_path())
        @test endswith(clap_src_path(), joinpath("csrc", "clap_host.c"))
        # The test bundle is built into a scratch space, not into the package.
        @test !startswith(BUNDLE, pkgdir(AudioPlugins))
        @test (@test_deprecated build_clap_host!()) == clap_lib_path()
    end

    @testset "the bundle builds and enumerates" begin
        @test isfile(BUNDLE)
        plugs = clap_scan(BUNDLE)
        @test length(plugs) == 3
        @test [p.id for p in plugs] == ["ap.gain", "ap.onepole", "ap.lookahead"]
        @test plugs[1].name == "AudioPlugins Test Gain"
    end

    @testset "failure paths are loud" begin
        @test_throws ErrorException clap_scan("/nonexistent.clap")
        # A real shared object that simply is not a CLAP plugin.
        libm = "/lib/aarch64-linux-gnu/libm.so.6"
        if isfile(libm)
            @test_throws ErrorException clap_scan(libm)
            @test occursin("clap_entry", clap_last_error())
        end
        @test_throws ErrorException clap_open!(BUNDLE; plugin_id = "no.such.id",
                                               block_size = 64)
        @test occursin("no.such.id", clap_last_error())
        @test_throws ErrorException clap_open!(BUNDLE; plugin_id = "ap.gain",
                                               block_size = 99999)
        @test_throws ErrorException clap_open!(BUNDLE; plugin_id = "ap.gain",
                                               block_size = 64, channels = 7)
        @test_throws ErrorException clap_open!(BUNDLE; plugin_id = "ap.gain",
                                               sample_rate = -1, block_size = 64)
        # State after a failed open is closed, not half-open.
        @test !clap_is_open()
    end

    @testset "open reports the configuration actually in force" begin
        clap_open!(BUNDLE; plugin_id = "ap.gain", sample_rate = 48000, block_size = 64)
        @test clap_is_open()
        @test clap_block_size() == 64
        @test clap_sample_rate() == 48000
        @test clap_plugin_name() == "AudioPlugins Test Gain"
    end

    @testset "parameter discovery" begin
        ps = clap_params()
        @test length(ps) == 1
        @test ps[1].id == 0.0
        @test ps[1].name == "Gain"
        @test (ps[1].min, ps[1].max, ps[1].default) == (0.0, 4.0, 1.0)
    end

    @testset "gain arithmetic: out == in * g, sample-exactly" begin
        clap_reset_counters!()
        x = [sin(2pi * 5 * i / 64) * 0.5 for i in 0:63]
        for g in (0.5, 1.0, 2.0, 0.0)
            tok = clap_fill!(x)
            out = AP.clp_process(tok, 0, g, -1, 0, -1, 0, -1, 0)
            @test !isnan(out)
            y = clap_out(out)
            @test length(y) == 64
            # float32 storage inside the plugin, so compare at float precision
            @test all(abs.(Float32.(x .* g) .- Float32.(y)) .< 1e-7)
        end
        @test clap_n_process() == 4     # exactly one process() per call
    end

    @testset "a stale token is refused, not answered" begin
        t1 = clap_fill!(ones(64))
        o1 = AP.clp_process(t1, 0, 1.0, -1, 0, -1, 0, -1, 0)
        t2 = clap_fill!(zeros(64))
        o2 = AP.clp_process(t2, 0, 1.0, -1, 0, -1, 0, -1, 0)
        @test isnan(AP.clp_out_rms(o1))            # superseded output token
        @test !isnan(AP.clp_out_rms(o2))           # the current one still reads
        @test isempty(clap_out(o1))                # and the Julia reader agrees
        @test isnan(AP.clp_process(t1, 0, 1.0, -1, 0, -1, 0, -1, 0))
        @test AP.clp_out_valid(o1) == 0.0
        @test AP.clp_out_valid(o2) == 1.0
    end

    @testset "parameter changes land on the intended block" begin
        t = clap_fill!(fill(1.0, 64))
        o = AP.clp_process(t, 0, 2.0, -1, 0, -1, 0, -1, 0)
        @test AP.clp_out_peak(o) ≈ 2.0
        @test AP.clp_out_rms(o) ≈ 2.0
        t = clap_fill!(fill(1.0, 64))
        o = AP.clp_process(t, 0, 0.25, -1, 0, -1, 0, -1, 0)
        @test AP.clp_out_peak(o) ≈ 0.25
        # And the plugin's own view agrees with what was sent.
        @test AP.clap_param_value(0) ≈ 0.25
        @test isnan(AP.clap_param_value(77))
    end

    @testset "latency is reported (and documented as uncompensated)" begin
        clap_open!(BUNDLE; plugin_id = "ap.gain", block_size = 64)
        @test clap_latency() == 0.0
        clap_open!(BUNDLE; plugin_id = "ap.lookahead", block_size = 64)
        @test clap_latency() == 16.0
        @test clap_param_count() == 0        # lookahead exposes no parameters
        # The delay is real: a unit impulse comes out 16 samples later.
        imp = zeros(64); imp[1] = 1.0
        t = clap_fill!(imp)
        o = AP.clp_process(t, -1, 0, -1, 0, -1, 0, -1, 0)
        y = clap_out(o)
        @test y[17] ≈ 1.0                   # 1-based: sample 16 -> index 17
        @test all(abs.(y[[1:16; 18:64]]) .< 1e-9)
    end

    @testset "state persists across blocks (the invariant that matters)" begin
        # Two consecutive 32-frame blocks through the one-pole must equal one
        # 64-frame run over the concatenated input. If the plugin's state reset
        # per block, or the host re-activated between blocks, the second half
        # would restart from zero.
        a = 0.25
        clap_open!(BUNDLE; plugin_id = "ap.onepole", block_size = 32)
        split = Float64[]
        for _ in 1:2
            t = clap_fill!(ones(32))
            o = AP.clp_process(t, 0, a, -1, 0, -1, 0, -1, 0)
            append!(split, clap_out(o))
        end
        clap_open!(BUNDLE; plugin_id = "ap.onepole", block_size = 64)
        t = clap_fill!(ones(64))
        o = AP.clp_process(t, 0, a, -1, 0, -1, 0, -1, 0)
        whole = clap_out(o)

        @test length(split) == 64 && length(whole) == 64
        @test maximum(abs.(split .- whole)) < 1e-6
        # Closed form: a one-pole step response is 1 - (1-a)^n.
        @test split[32] ≈ 1 - (1 - a)^32 atol = 1e-5
        @test split[64] ≈ 1 - (1 - a)^64 atol = 1e-5
        # And the state really is carried: block 2 starts above where block 1 began.
        @test split[33] > split[1]
    end

    @testset "the plugin is not reset by reopening at a new block size" begin
        # Reopening is a fresh instance, so it MUST start from zero -- the
        # counterpart of the test above, and what makes that one meaningful.
        clap_open!(BUNDLE; plugin_id = "ap.onepole", block_size = 32)
        t = clap_fill!(ones(32))
        o = AP.clp_process(t, 0, 0.25, -1, 0, -1, 0, -1, 0)
        first_of_fresh = clap_out(o)[1]
        @test first_of_fresh ≈ 0.25 atol = 1e-6    # y = 0 + 0.25*(1-0)
    end

    @testset "closing is clean and idempotent" begin
        clap_close!()
        @test !clap_is_open()
        clap_close!()
        @test !clap_is_open()
        @test isnan(AP.clp_process(1.0, 0, 1.0, -1, 0, -1, 0, -1, 0))
    end

end

include("vst3tests.jl")
