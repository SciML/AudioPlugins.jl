using Test, AudioPlugins, Libdl
include("build_offline.jl")
const HOSTS = build_offline(mktempdir())

module SourceOfflineInstances
    const LV2_LIB = first(parentmodule(@__MODULE__).HOSTS)
    const VST3_LIB = last(parentmodule(@__MODULE__).HOSTS)
    lv2_last_error() = unsafe_string(ccall((:lv2_host_last_error, LV2_LIB), Cstring, ()))
    vst3_last_error() = unsafe_string(ccall((:vst3_host_last_error, VST3_LIB), Cstring, ()))
    include(joinpath(@__DIR__, "..", "src", "lv2_instances.jl"))
    include(joinpath(@__DIR__, "..", "src", "vst3_instances.jl"))
end
const OI = SourceOfflineInstances
const LV2_PATH = lv2_default_path(lv2_test_bundle(; force = true))
const SDK = (joinpath(vst3sdk_jll.artifact_dir, "include", "vst3sdk"), joinpath(vst3sdk_jll.artifact_dir, "lib", "vst3sdk"))
const VST3_BUNDLE = vst3_test_bundle(SDK; force = true)

@testset "public offline instance API" begin
    @static if isdefined(Base, :ispublic)
        for name in (:lv2_copy, :lv2_set, :lv2_expect, :vst3_copy, :vst3_set, :vst3_expect)
            @test Base.ispublic(AudioPlugins, name)
        end
    end
    for (format, constructor, path, idkey, id, value) in (
            (:lv2, Lv2Instance, LV2_PATH, :uri, "urn:audioplugins:test:gain", 0.5),
            (:vst3, Vst3Instance, VST3_BUNDLE, :class_id, AudioPlugins.VST3_TEST_GAIN, 0.125),
        )
        api(name) = getfield(AudioPlugins, Symbol(format, "_", name))
        saved = Ref{Any}()
        constructor(path; NamedTuple{(idkey,)}((id,))..., block_size = 32) do instance
            saved[] = instance
            @test api(:is_open)(instance)
            @test api(:params)(instance)[1].max == 4
            input = api(:fill!)(instance, ones(32))
            input = api(:expect)(instance, input, api(:plugin_index)(instance))
            @test isfinite(input)
            input = api(:set)(instance, input, 0, value)
            output = api(:process)(instance, input, -1, 0, -1, 0, -1, 0, -1, 0)
            @test api(:out)(instance, output) == fill(0.5, 32)
            @test api(:out_valid)(instance, output) == 1
            @test api(:param_value)(instance, 0) == value
        end
        @test !api(:is_open)(saved[])
    end
end

for (format, constructor, path, gain_id, pole_id, idkey, scale) in (
        (:lv2, OI.Lv2Instance, LV2_PATH, "urn:audioplugins:test:gain", "urn:audioplugins:test:onepole", :uri, 1),
        (:vst3, OI.Vst3Instance, VST3_BUNDLE, AudioPlugins.VST3_TEST_GAIN, AudioPlugins.VST3_TEST_ONEPOLE, :class_id, 4),
    )
    api(name) = getfield(OI, Symbol(format, "_", name))
    create(id; kwargs...) = constructor(path; NamedTuple{(idkey,)}((id,))..., block_size = 32, kwargs...)
    process(inst, token; id = -1, value = 0) = api(:process)(inst, token, id, value, -1, 0, -1, 0, -1, 0)
    @testset "$format independent instances" begin
        gain, pole, other = create(gain_id), create(pole_id), create(gain_id)
        try
            @test api(:plugin_name)(gain) == "AudioPlugins Test Gain"
            @test api(:params)(gain)[1].max == 4
            @test api(:params)(pole)[1].max == 1
            @test api(:param_count)(gain) == 1
            @test api(:block_size)(gain) == 32
            @test api(:sample_rate)(gain) == 48000
            @test api(:channels)(gain) == api(:n_audio_in)(gain) == api(:n_audio_out)(gain) == 1
            @test api(:latency)(pole) == 0
            if format == :vst3
                @test OI.vst3_params(gain)[1].default_normalized == 0.25
                @test OI.vst3_param_plain(gain, 0, 0.125) == 0.5
                @test OI.vst3_param_normalized(gain, 0, 0.5) == 0.125
                @test OI.vst3_n_restart_requests(gain) == OI.vst3_n_plugin_edits(gain) == 0
            else
                @test OI.lv2_params(gain)[1].symbol == "gain"
                @test isempty(OI.lv2_atom_ports(gain))
            end
            library = format == :lv2 ? OI.LV2_LIB : OI.VST3_LIB
            host = Libdl.dlopen(library)
            try
                open_default = Libdl.dlsym(host, Symbol(format, "_host_open"))
                scan_default = Libdl.dlsym(host, Symbol(format, "_host_scan"))
                close_default = Libdl.dlsym(host, Symbol(format, "_host_close"))
                @test ccall(open_default, Cint, (Cstring, Cstring, Cdouble, Cdouble, Cdouble), path, gain_id, 48000, 32, 1) == 0
                @test ccall(scan_default, Clong, (Cstring,), path) > 0
                ccall(close_default, Cvoid, ())
                @test api(:is_open)(gain) && api(:is_open)(pole)
                @test api(:params)(gain)[1].max == 4
            finally
                Libdl.dlclose(host)
            end
            previous = NaN
            for block in 0:2
                input = api(:fill!)(gain, ones(32))
                @test api(:expect)(gain.handle, input, api(:plugin_index)(gain)) == input
                @test isnan(api(:expect)(gain, input, 100))
                @test isnan(process(gain, api(:expect)(gain, input, 100)))
                @test api(:in_sample)(gain.handle, input, 0, 0) == 1
                out = process(gain.handle, input; id = 0, value = 0.5 / scale)
                @test api(:out)(gain, out) == fill(0.5, 32)
                @test api(:out_rms)(gain, out) == api(:out_peak)(gain, out) == 0.5
                @test api(:out_valid)(gain, out) == 1
                @test isnan(process(pole, input))
                @test api(:out_valid)(pole, out) == 0
                @test isempty(api(:out)(pole, out))
                @test isnan(api(:copy!)(pole, gain, previous))
                dep = api(:copy)(pole.handle, gain.handle, out)
                result = process(pole, dep; id = 0, value = 0.25)
                @test api(:out)(pole, result) ≈ [0.5 * (1 - 0.75^i) for i in (32block + 1):(32block + 32)] atol = 1.0e-6
                @test isnan(process(pole, dep))
                @test api(:n_process)(gain) == api(:n_process)(pole) == block + 1
                token = api(:set)(other, api(:fill!)(other, ones(32)), 0, 2 / scale)
                @test api(:param_value)(other, 0) != 2 / scale || block > 0
                @test api(:out)(other, process(other, token)) == fill(2, 32)
                @test api(:param_value)(gain, 0) == 0.5 / scale
                @test api(:param_value)(other, 0) == 2 / scale
                @test isnan(api(:out_sample)(gain, previous, 0, 0))
                previous = out
            end
            for config in ((; sample_rate = 44100), (; block_size = 64), (; channels = 2))
                # LV2 gain is mono; merge supplies two declared inputs for the stereo mismatch.
                id = format == :lv2 && haskey(config, :channels) ? "urn:audioplugins:test:merge" : gain_id
                settings = merge((; block_size = 32), config)
                mismatch = constructor(path; NamedTuple{(idkey,)}((id,))..., settings...)
                try
                    @test isnan(api(:copy!)(mismatch, gain, previous))
                finally
                    api(:close!)(mismatch)
                end
            end
            token = api(:fill!)(other, ones(32))
            @test isnan(api(:set)(other, token, 9999, 1))
            @test isnan(api(:set)(other, token, 0, NaN))
            @test isnan(api(:set)(other, token, -1, 1))
            @test isnan(api(:set)(other, token - 1, 0, 1))
            @test isnan(api(:set)(other, NaN, 0, 1))
            @test isnan(process(other, NaN))
            @test_throws ErrorException create(format == :lv2 ? "urn:missing" : "missing")
            for config in ((; sample_rate = NaN), (; sample_rate = Inf), (; block_size = 0), (; channels = 0))
                @test_throws ErrorException constructor(path; NamedTuple{(idkey,)}((gain_id,))..., config...)
            end
            @test api(:is_open)(gain) && api(:is_open)(pole)
            api(:close!)(gain)
            api(:close!)(gain)
            @test !api(:is_open)(gain)
            @test api(:out_valid)(gain, previous) == 0
            @test isnan(api(:copy!)(pole, gain, previous))
            @test api(:is_open)(pole) && api(:is_open)(other)
            token = api(:set)(other, api(:fill!)(other, ones(32)), 0, 0)
            current = process(pole, api(:fill!)(pole, ones(32)))
            copied = api(:copy!)(other, pole, current)
            @test isnan(process(other, token))
            copied_output = process(other, copied)
            @test api(:param_value)(other, 0) == 2 / scale
            @test api(:out_peak)(other, copied_output) > 1
            t = api(:in_tone)(other.handle, 32 / 48000, 2, 0, 1)
            @test isfinite(process(other, t; id = 0, value = 3 / scale))
            api(:reset_counters!)(other)
            @test api(:n_process)(other) == 0
            if format == :vst3
                for (value, expected) in ((-1, 0), (2, 4))
                    input = api(:set)(other, api(:fill!)(other, ones(32)), 0, value)
                    @test api(:out)(other, process(other, input)) == fill(expected, 32)
                end
                for (value, expected) in ((Inf, 4), (-Inf, 0))
                    input = api(:fill!)(other, ones(32))
                    output = process(other, input; id = 0, value = value)
                    @test api(:out)(other, output) == fill(expected, 32)
                    @test api(:param_value)(other, 0) == expected / 4
                end
            end
        finally
            foreach(api(:close!), (gain, pole, other))
        end
        saved = Ref{typeof(gain)}()
        @test_throws ErrorException constructor(path; NamedTuple{(idkey,)}((gain_id,))...) do instance
            saved[] = instance
            error("leave scope")
        end
        @test !api(:is_open)(saved[])
        fresh = create(gain_id)
        try
            @test fresh.handle != saved[].handle
            @test isnan(process(saved[], 1))
            @test api(:is_open)(fresh)
        finally
            api(:close!)(fresh)
        end
    end
end

const PARAMETER_BUNDLES = build_parameter_bundles(mktempdir(), SDK)
for (fmt, constructor, path, id, key, scale) in (
        (:lv2, OI.Lv2Instance, first(PARAMETER_BUNDLES), "urn:audioplugins:test:gain", :uri, 1),
        (:vst3, OI.Vst3Instance, last(PARAMETER_BUNDLES), AudioPlugins.VST3_TEST_GAIN, :class_id, 4),
    )
    api(name) = getfield(OI, Symbol(fmt, "_", name))
    @testset "$fmt parameter chains beyond four slots" begin
        constructor(path; NamedTuple{(key,)}((id,))..., block_size = 32) do effect
            params = api(:params)(effect)
            @test length(params) == 8
            for repetition in 1:2
                token = api(:fill!)(effect, ones(32))
                for (i, p) in enumerate(params)
                    @test api(:set)(effect.handle, token, p.id, i / (8scale)) == token
                end
                output = api(:process)(effect.handle, token, -1, 0, -1, 0, -1, 0, -1, 0)
                @test api(:out)(effect, output) == fill(4.5, 32)
                @test [api(:param_value)(effect, p.id) for p in params] == [i / (8scale) for i in 1:8]
            end
            token = api(:fill!)(effect, ones(32))
            for p in params
                token = api(:set)(effect, token, p.id, 0)
            end
            # A new input invalidates the queued chain and preserves applied values.
            token = api(:fill!)(effect, ones(32))
            output = api(:process)(effect, token, -1, 0, -1, 0, -1, 0, -1, 0)
            @test api(:out)(effect, output) == fill(4.5, 32)
            token = api(:set)(effect, api(:fill!)(effect, ones(32)), 0, 0)
            token = api(:set)(effect, token, 0, 1 / scale)
            output = api(:process)(effect, token, 0, 0.5 / scale, -1, 0, -1, 0, -1, 0)
            @test api(:param_value)(effect, 0) == 0.5 / scale
            @test isfinite(api(:out_peak)(effect, output))
        end
    end
end
