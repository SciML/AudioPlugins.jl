# Test the instance ABI against the shipped C sources, independently of the
# CLAPHost_jll release cycle. The ordinary Core group tests the installed JLL.
using Test
using AudioPlugins

module SourceClapInstances
    using AudioPlugins: AudioPlugins, _resolve_plugin
    using Libdl: dlext
    const HOST_DIR = mktempdir()
    const CLAP_LIB = joinpath(HOST_DIR, "libclap_host.$dlext")
    let cc = AudioPlugins._c_compiler(), src = AudioPlugins.clap_src_path()
        dl = Sys.islinux() ? ["-ldl"] : String[]
        run(`$cc $(AudioPlugins._c_arch_flags()) -std=gnu99 -O2 -fPIC -shared -Wall -Wextra -o $CLAP_LIB $src $dl -lm`)
    end
    clap_last_error() = unsafe_string(ccall((:clap_host_last_error, CLAP_LIB), Cstring, ()))
    include(joinpath(@__DIR__, "..", "src", "clap_instances.jl"))
end

const IC = SourceClapInstances
const INSTANCE_BUNDLE = clap_test_bundle(; force = true)

@testset "concurrently hosted CLAP effects" begin
    gain = IC.ClapInstance(INSTANCE_BUNDLE; plugin_id = "ap.gain", block_size = 32, channels = 2)
    pole = IC.ClapInstance(INSTANCE_BUNDLE; plugin_id = "ap.onepole", block_size = 32, channels = 2)
    other = IC.ClapInstance(INSTANCE_BUNDLE; plugin_id = "ap.gain", block_size = 32, channels = 2)
    try
        @test IC.clap_plugin_name(gain) == "AudioPlugins Test Gain"
        @test IC.clap_plugin_index(pole) == 1
        @test IC.clap_block_size(gain) == 32
        @test IC.clap_sample_rate(gain) == 48000
        @test IC.clap_n_audio_in(gain) == IC.clap_n_audio_out(gain) == 2
        @test IC.clap_params(gain)[1].max == 4
        @test IC.clap_params(pole)[1].max == 1
        @test IC.clap_latency(pole) == 0
        previous = NaN
        for block in 0:2
            input = IC.clap_fill!(gain, repeat([1.0, 2.0], 32); channels = 2)
            @test IC.clp_expect(gain, input, 0) == input
            @test isnan(IC.clp_expect(gain, input, 1))
            @test IC.clp_in_sample(gain, input, 0, 1) == 2
            out = IC.clp_process(gain, input, 0, 0.5, -1, 0, -1, 0, -1, 0)
            @test IC.clp_out_peak(gain, out) == 1
            @test IC.clp_out_rms(gain, out) ≈ sqrt(0.625)
            @test isnan(IC.clp_process(pole, input, 0, 0.25, -1, 0, -1, 0, -1, 0))
            @test isempty(IC.clap_out(pole, out))
            @test isnan(IC.clap_copy!(pole, gain, previous))
            # Numeric handles are the same ABI a generated node uses.
            dep = IC.clp_copy(pole.handle, gain.handle, out)
            result = IC.clp_process(pole.handle, dep, 0, 0.25, -1, 0, -1, 0, -1, 0)
            expected = [0.5 * (1 - 0.75^i) for i in (32block + 1):(32block + 32)]
            @test IC.clap_out(pole, result) ≈ expected atol = 1.0e-6
            @test IC.clap_out(pole, result; channel = 1) ≈ 2expected atol = 1.0e-6
            @test IC.clap_n_process(gain) == IC.clap_n_process(pole) == block + 1
            t = IC.clp_set(other, IC.clap_fill!(other, ones(32)), 0, 2)
            o = IC.clp_process(other, t, -1, 0, -1, 0, -1, 0, -1, 0)
            @test IC.clap_out(other, o) == fill(2, 32)
            @test IC.clap_param_value(gain, 0) == 0.5
            @test IC.clap_param_value(other, 0) == 2
            previous = out
        end
        @test_throws ErrorException IC.ClapInstance(INSTANCE_BUNDLE; plugin_id = "missing")
        @test IC.clap_is_open(gain) && IC.clap_is_open(pole)
        for kwargs in ((; sample_rate = 44100), (; block_size = 64), (; channels = 1))
            config = merge((; sample_rate = 48000, block_size = 32, channels = 2), kwargs)
            mismatch = IC.ClapInstance(INSTANCE_BUNDLE; plugin_id = "ap.gain", config...)
            try
                @test isnan(IC.clap_copy!(mismatch, gain, previous))
            finally
                IC.clap_close!(mismatch)
            end
        end
        IC.clap_close!(gain)
        IC.clap_close!(gain)
        @test !IC.clap_is_open(gain)
        @test IC.clp_out_valid(gain, previous) == 0
        @test isnan(IC.clap_copy!(pole, gain, previous))
        @test IC.clap_is_open(pole)
        @test IC.clap_is_open(other)
        # A surviving instance remains usable after a peer is destroyed.
        t = IC.clp_in_tone(other, 32 / 48000, 2, 0, 1)
        @test isfinite(IC.clp_process(other, t, 0, 3, -1, 0, -1, 0, -1, 0))
        IC.clap_reset_counters!(other)
        @test IC.clap_n_process(other) == 0
    finally
        IC.clap_close!(gain)
        IC.clap_close!(pole)
        IC.clap_close!(other)
    end
end

@testset "instance scope closes on exceptions" begin
    saved = Ref{IC.ClapInstance}()
    @test_throws ErrorException IC.ClapInstance(INSTANCE_BUNDLE; plugin_id = "ap.gain") do effect
        saved[] = effect
        error("leave the scope")
    end
    @test !IC.clap_is_open(saved[])
    IC.ClapInstance(INSTANCE_BUNDLE; plugin_id = "ap.gain") do fresh
        @test fresh.handle != saved[].handle
        @test isnan(IC.clp_process(saved[], 1, 0, 1, -1, 0, -1, 0, -1, 0))
        @test IC.clap_is_open(fresh)
    end
end
