# The VST3 host, tested by hosting the VST3 bundle that ships with this
# package (test/plugins/ap_test_vst3.cpp, built here against vst3sdk_jll)
# and, as a second witness, the SDK's own `again` example that vst3sdk_jll
# ships prebuilt. The same arithmetic expectations as the CLAP and LV2
# suites, deliberately.

using Test
using Libdl
using AudioPlugins
using vst3sdk_jll
const AP = AudioPlugins

const SDK_INC = joinpath(vst3sdk_jll.artifact_dir, "include", "vst3sdk")
const SDK_LIB = joinpath(vst3sdk_jll.artifact_dir, "lib", "vst3sdk")
const AGAIN   = joinpath(vst3sdk_jll.artifact_dir, "lib", "vst3", "again-sample-accurate.vst3")
const BUNDLE  = vst3_test_bundle((SDK_INC, SDK_LIB))
const GAIN, POLE, LOOK = AP.VST3_TEST_GAIN, AP.VST3_TEST_ONEPOLE, AP.VST3_TEST_LOOKAHEAD

@testset "AudioPlugins / VST3" begin

    @testset "the host comes prebuilt, the test plugins do not" begin
        @test isfile(vst3_lib_path())
        @test !startswith(vst3_lib_path(), pkgdir(AudioPlugins))
        h = Libdl.dlopen(vst3_lib_path())
        @test Libdl.dlsym(h, :vst3_process) != C_NULL
        # Only the C surface is exported: nothing of the SDK leaks out.
        @test Libdl.dlsym(h, :_ZN9Steinberg3Vst12PlugProviderC1ERKN4VST37Hosting13PluginFactoryENS2_9ClassInfoEb; throw_error = false) === nothing
        Libdl.dlclose(h)
        @test isfile(vst3_src_path())
        @test isdir(BUNDLE)
        @test !startswith(BUNDLE, pkgdir(AudioPlugins))
    end

    @testset "discovery" begin
        classes = vst3_scan(BUNDLE)
        @test length(classes) == 3
        @test [c.id for c in classes] == [GAIN, POLE, LOOK]
        @test classes[1].name == "AudioPlugins Test Gain"
        @test classes[2].category == "Fx|Filter"
    end

    @testset "failure paths are loud" begin
        @test_throws ErrorException vst3_scan("/nonexistent.vst3")
        @test_throws ErrorException vst3_scan(clap_lib_path())        # a real .so that is not VST3
        @test_throws ErrorException vst3_open!(BUNDLE; class_id = "0"^32, block_size = 64)
        @test occursin("no audio-effect class", vst3_last_error())
        @test_throws ErrorException vst3_open!(BUNDLE; class_id = GAIN, block_size = 99999)
        @test_throws ErrorException vst3_open!(BUNDLE; class_id = GAIN, block_size = 64, channels = 7)
        @test_throws ErrorException vst3_open!(BUNDLE; class_id = GAIN, sample_rate = -1, block_size = 64)
        @test !vst3_is_open()
    end

    @testset "open reports the configuration actually in force" begin
        vst3_open!(BUNDLE; class_id = GAIN, sample_rate = 48000, block_size = 64)
        @test vst3_is_open()
        @test vst3_block_size() == 64
        @test vst3_sample_rate() == 48000
        @test vst3_channels() == 1
        @test vst3_plugin_name() == "AudioPlugins Test Gain"
        @test vst3_plugin_id() == GAIN
    end

    @testset "parameter discovery from the controller" begin
        ps = vst3_params()
        @test length(ps) == 1
        @test ps[1].id == 0.0
        @test ps[1].name == "Gain"
        @test (ps[1].min, ps[1].max, ps[1].default) == (0.0, 4.0, 1.0)
        @test ps[1].default_normalized == 0.25
        @test ps[1].steps == 0 && !ps[1].readonly && !ps[1].bypass
        @test vst3_param_plain(0, 0.5) == 2.0
        @test vst3_param_normalized(0, 2.0) == 0.5
        @test isnan(vst3_param_value(77))
    end

    @testset "gain arithmetic: out == in * g, sample-exactly" begin
        vst3_reset_counters!()
        x = [sin(2pi * 5 * i / 64) * 0.5 for i in 0:63]
        for g in (0.5, 1.0, 2.0, 0.0)
            tok = vst3_fill!(x)
            out = AP.vst3_process(tok, 0, g / 4, -1, 0, -1, 0, -1, 0)   # normalised
            @test !isnan(out)
            y = vst3_out(out)
            @test length(y) == 64
            @test all(abs.(Float32.(x .* g) .- Float32.(y)) .< 1e-7)
        end
        @test vst3_n_process() == 4
    end

    @testset "a stale token is refused, not answered" begin
        t1 = vst3_fill!(ones(64))
        o1 = AP.vst3_process(t1, 0, 0.25, -1, 0, -1, 0, -1, 0)
        t2 = vst3_fill!(zeros(64))
        o2 = AP.vst3_process(t2, 0, 0.25, -1, 0, -1, 0, -1, 0)
        @test isnan(AP.vst3_out_rms(o1))
        @test !isnan(AP.vst3_out_rms(o2))
        @test isempty(vst3_out(o1))
        @test isnan(AP.vst3_process(t1, 0, 0.25, -1, 0, -1, 0, -1, 0))
        @test AP.vst3_out_valid(o1) == 0.0
        @test AP.vst3_out_valid(o2) == 1.0
    end

    @testset "parameter changes land on the intended block" begin
        t = vst3_fill!(fill(1.0, 64))
        o = AP.vst3_process(t, 0, 0.5, -1, 0, -1, 0, -1, 0)      # gain 2
        @test AP.vst3_out_peak(o) ≈ 2.0
        @test AP.vst3_out_rms(o) ≈ 2.0
        t = vst3_fill!(fill(1.0, 64))
        o = AP.vst3_process(t, 0, 0.0625, -1, 0, -1, 0, -1, 0)   # gain 0.25
        @test AP.vst3_out_peak(o) ≈ 0.25
        @test vst3_param_value(0) ≈ 0.0625                     # the controller agrees
    end

    @testset "stereo keeps its channels apart" begin
        vst3_open!(BUNDLE; class_id = GAIN, block_size = 64, channels = 2)
        @test vst3_channels() == 2
        x = vec(permutedims([fill(1.0, 64) fill(-0.5, 64)]))   # interleaved L,R
        t = vst3_fill!(x; channels = 2)
        o = AP.vst3_process(t, 0, 0.25, -1, 0, -1, 0, -1, 0)   # gain 1
        @test all(vst3_out(o; channel = 0) .== 1.0)
        @test all(vst3_out(o; channel = 1) .== -0.5)
    end

    @testset "latency is reported (and documented as uncompensated)" begin
        vst3_open!(BUNDLE; class_id = GAIN, block_size = 64)
        @test vst3_latency() == 0.0
        vst3_open!(BUNDLE; class_id = LOOK, block_size = 64)
        @test vst3_latency() == 16.0
        @test vst3_param_count() == 0
        imp = zeros(64); imp[1] = 1.0
        t = vst3_fill!(imp)
        o = AP.vst3_process(t, -1, 0, -1, 0, -1, 0, -1, 0)
        y = vst3_out(o)
        @test y[17] ≈ 1.0
        @test all(abs.(y[[1:16; 18:64]]) .< 1e-9)
    end

    @testset "state persists across blocks (the invariant that matters)" begin
        a = 0.25
        vst3_open!(BUNDLE; class_id = POLE, block_size = 32)
        split = Float64[]
        for _ in 1:2
            t = vst3_fill!(ones(32))
            o = AP.vst3_process(t, 0, a, -1, 0, -1, 0, -1, 0)
            append!(split, vst3_out(o))
        end
        vst3_open!(BUNDLE; class_id = POLE, block_size = 64)
        t = vst3_fill!(ones(64))
        o = AP.vst3_process(t, 0, a, -1, 0, -1, 0, -1, 0)
        whole = vst3_out(o)
        @test length(split) == 64 && length(whole) == 64
        @test maximum(abs.(split .- whole)) < 1e-6
        @test split[32] ≈ 1 - (1 - a)^32 atol = 1e-5
        @test split[64] ≈ 1 - (1 - a)^64 atol = 1e-5
        @test split[33] > split[1]
    end

    @testset "reopening is a fresh instance" begin
        vst3_open!(BUNDLE; class_id = POLE, block_size = 32)
        t = vst3_fill!(ones(32))
        o = AP.vst3_process(t, 0, 0.25, -1, 0, -1, 0, -1, 0)
        @test vst3_out(o)[1] ≈ 0.25 atol = 1e-6
    end

    @testset "closing is clean and idempotent" begin
        vst3_close!()
        @test !vst3_is_open()
        vst3_close!()
        @test !vst3_is_open()
        @test isnan(AP.vst3_process(1.0, 0, 1.0, -1, 0, -1, 0, -1, 0))
    end

    # The SDK's own gain example, prebuilt by vst3sdk_jll: a real third-party
    # plugin with a separate controller, a sidechain bus and sample-accurate
    # parameter smoothing, hosted sample-exactly.
    @testset "the SDK's again example" begin
        @test isdir(AGAIN)
        classes = vst3_scan(AGAIN)
        @test any(c -> c.name == "AGain Sample Accurate", classes)
        again = classes[findfirst(c -> c.name == "AGain Sample Accurate", classes)]
        vst3_open!(AGAIN; class_id = again.id, block_size = 64, channels = 2)
        @test vst3_plugin_name() == "AGain Sample Accurate"
        ps = vst3_params()
        gi = findfirst(p -> p.name == "Gain", ps)
        @test gi !== nothing
        gid = ps[gi].id
        for _ in 1:2   # a block for the smoother to settle, then the block we check
            t = vst3_fill!(vec(permutedims([ones(64) ones(64)])); channels = 2)
            global o_again = AP.vst3_process(t, gid, 0.5, -1, 0, -1, 0, -1, 0)
        end
        @test all(vst3_out(o_again; channel = 0) .== 0.5)
        @test all(vst3_out(o_again; channel = 1) .== 0.5)
        @test vst3_latency() == 0.0
        vst3_close!()
    end

end
