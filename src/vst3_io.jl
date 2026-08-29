# Hosting a VST3 plugin, mirroring clap_io.jl and lv2_io.jl call for call.
# The implementation is csrc/vst3_host.cpp -- C++ against the Steinberg VST3
# SDK (>= 3.8, MIT), built inside Yggdrasil against vst3sdk_jll and shipped
# prebuilt as VST3Host_jll, so nothing here needs a C++ toolchain -- and the
# operators below are one-line `ccall`s into its `extern "C"` surface.
#
# VST3 specifics a caller should know, all inherited from the format:
#   * a plugin class is named by a 32-hex-character class id, not a string
#     id; `vst3_scan` lists them;
#   * parameter VALUES ARE NORMALISED to 0..1 -- that is what the processor
#     consumes through inputParameterChanges. `vst3_params()` reports the
#     controller's plain range for information, and `vst3_param_plain` /
#     `vst3_param_normalized` convert;
#   * latency is IAudioProcessor::getLatencySamples(), re-read when the plugin
#     asks for a restart with kLatencyChanged; surfaced, never compensated.
#
# Strings (bundle paths, class ids) are driver-side only, as everywhere here.

export vst3_lib_path, vst3_src_path,
       vst3_scan, vst3_open!, vst3_close!,
       vst3_is_open, vst3_last_error, vst3_plugin_name, vst3_plugin_id,
       vst3_params, vst3_param_count, vst3_param_value, vst3_param_plain, vst3_param_normalized,
       vst3_latency, vst3_block_size, vst3_sample_rate, vst3_channels,
       vst3_n_process, vst3_reset_counters!,
       vst3_fill!, vst3_out, vst3_test_bundle,
       VST3_WAVE_SILENCE, VST3_WAVE_SINE, VST3_WAVE_SQUARE, VST3_WAVE_RAMP, VST3_WAVE_IMPULSE

using VST3Host_jll: VST3Host_jll, libvst3_host

const VST3_LIB = libvst3_host
const VST3_SRC = normpath(joinpath(@__DIR__, "..", "csrc", "vst3_host.cpp"))

"""
    vst3_lib_path() -> String

Absolute path of the prebuilt VST3 host library (from `VST3Host_jll`), for a
driver that links the host into a standalone program. To run against a locally
modified `csrc/vst3_host.cpp`, build it against the VST3 SDK and set the
`libvst3_host_path` preference on `VST3Host_jll` (see [`clap_lib_path`](@ref)).
"""
vst3_lib_path() = VST3Host_jll.libvst3_host_path::String

"Path of `csrc/vst3_host.cpp`, which ships with the package for a C++ build to link."
vst3_src_path() = VST3_SRC

const VST3_WAVE_SILENCE = 0
const VST3_WAVE_SINE    = 1
const VST3_WAVE_SQUARE  = 2
const VST3_WAVE_RAMP    = 3
const VST3_WAVE_IMPULSE = 4

# Class ids of the test plugins, fixed in test/plugins/ap_test_vst3.cpp.
const VST3_TEST_GAIN      = "41505447" * "41494E00" * "00000000" * "00000001"
const VST3_TEST_ONEPOLE   = "41505431" * "504F4C45" * "00000000" * "00000002"
const VST3_TEST_LOOKAHEAD = "41504C4F" * "4F4B4148" * "00000000" * "00000003"

"""
    vst3_test_bundle(sdk_root; force = false) -> String

Build the VST3 bundle of test plugins that ships with this package
(`test/plugins/ap_test_vst3.cpp`: gain, one-pole, 16-sample lookahead) into a
per-package scratch space and return the bundle path (`.../ap_test.vst3`).

`sdk_root` is a directory holding the VST3 SDK source tree (`pluginterfaces/`,
`base/`, `public.sdk/`) with its static libraries under `lib/` -- what
`vst3sdk_jll` provides as `joinpath(vst3sdk_jll.artifact_dir, "include",
"vst3sdk")` plus `lib/vst3sdk`, which is how the test suite calls this; pass
`(include_dir, lib_dir)` as a tuple when the two are apart. This is the one
place the package needs a **C++** compiler, and only for the tests: hosting
needs nothing but the prebuilt `VST3Host_jll`, which is why `vst3sdk_jll` is a
test dependency rather than a package dependency. The bundle is laid out as
the SDK's module loader expects (`Contents/<arch>-linux/`, `Contents/MacOS/`,
`Contents/<arch>-win/`).
"""
function vst3_test_bundle(sdk_root; force::Bool = false)
    sdk, libdir = sdk_root isa Tuple ? sdk_root : (sdk_root, joinpath(sdk_root, "lib"))
    src = normpath(joinpath(@__DIR__, "..", "test", "plugins", "ap_test_vst3.cpp"))
    root = @get_scratch!("test_plugins")
    bundle = joinpath(root, "ap_test.vst3")
    arch = Sys.ARCH == :aarch64 ? "aarch64" : Sys.ARCH == :x86_64 ? "x86_64" : string(Sys.ARCH)
    if Sys.isapple()
        inner = joinpath(bundle, "Contents", "MacOS"); bin = joinpath(inner, "ap_test")
        main = joinpath(sdk, "public.sdk", "source", "main", "macmain.cpp")
    elseif Sys.iswindows()
        inner = joinpath(bundle, "Contents", arch * "-win"); bin = joinpath(inner, "ap_test.vst3")
        main = joinpath(sdk, "public.sdk", "source", "main", "dllmain.cpp")
    else
        inner = joinpath(bundle, "Contents", arch * "-linux"); bin = joinpath(inner, "ap_test.so")
        main = joinpath(sdk, "public.sdk", "source", "main", "linuxmain.cpp")
    end
    if force || !isfile(bin) || stat(src).mtime > stat(bin).mtime
        cxx = _cxx_compiler()
        cxx === nothing && error(
            "vst3_test_bundle: building the VST3 test plugins needs a C++ compiler on PATH " *
            "(tried c++, g++, clang++). Hosting itself does not: the host library comes " *
            "prebuilt from VST3Host_jll.")
        mkpath(inner)
        sce = joinpath(sdk, "public.sdk", "source", "vst", "vstsinglecomponenteffect.cpp")
        cmd = `$cxx -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -DRELEASE=1 -I$sdk
               -o $bin $src $sce $main -L$libdir -lsdk -lsdk_common -lbase -lpluginterfaces -lpthread`
        Sys.isapple() && (cmd = `$cmd -framework CoreFoundation`)
        run(cmd)
        # The loader looks for a snapshot/moduleinfo only optionally; a bare
        # binary in the right place is a valid bundle.
    end
    return bundle
end

"Path of a C++ compiler, or `nothing`."
function _cxx_compiler()
    for c in ("c++", "g++", "clang++")
        p = Sys.which(c)
        p === nothing || return p
    end
    return nothing
end

# ---------------------------------------------------------------------------
# Driver-side lifecycle and discovery. Strings live here and nowhere else.
# ---------------------------------------------------------------------------

"""
    vst3_scan(path) -> Vector{@NamedTuple{id::String, name::String, category::String}}

Load a `.vst3` bundle (or a bare shared object) and enumerate its audio-effect
classes without instantiating anything. `id` is the class id as 32 hex
characters, which is what [`vst3_open!`](@ref) takes. Throws with the host's
own message when the bundle cannot be loaded.
"""
function vst3_scan(path::AbstractString)
    n = ccall((:vst3_host_scan, VST3_LIB), Clong, (Cstring,), path)
    n < 0 && error("vst3_scan($(repr(path))) failed: $(vst3_last_error())")
    return [(id       = unsafe_string(ccall((:vst3_host_scan_id, VST3_LIB), Cstring, (Clong,), i)),
             name     = unsafe_string(ccall((:vst3_host_scan_name, VST3_LIB), Cstring, (Clong,), i)),
             category = unsafe_string(ccall((:vst3_host_scan_category, VST3_LIB), Cstring, (Clong,), i)))
            for i in 0:(n - 1)]
end

"""
    vst3_open!(path; class_id = "", sample_rate = 48000, block_size = 512, channels = 1)

Instantiate, initialise and activate the class `class_id` (32 hex characters;
`""` for the first audio-effect class) at a **fixed** block size, asking the
plugin for mono (`channels = 1`) or stereo (`2`) on its main buses. A plugin
that refuses the arrangement, cannot process 32-bit float, or refuses
`setupProcessing` / `setActive` fails here, loudly.
"""
function vst3_open!(path::AbstractString; class_id::AbstractString = "",
                    sample_rate::Real = 48000, block_size::Integer = 512,
                    channels::Integer = 1)
    r = ccall((:vst3_host_open, VST3_LIB), Cint,
              (Cstring, Cstring, Cdouble, Cdouble, Cdouble),
              path, class_id, sample_rate, block_size, channels)
    r == 0 || error("vst3_open!($(repr(path)), $(repr(class_id))) failed: $(vst3_last_error())")
    return nothing
end

"Stop processing, deactivate, terminate, release and unload. Safe when nothing is open."
function vst3_close!()
    ccall((:vst3_host_close, VST3_LIB), Cvoid, ())
    return nothing
end

vst3_last_error()  = unsafe_string(ccall((:vst3_host_last_error, VST3_LIB), Cstring, ()))
vst3_plugin_name() = unsafe_string(ccall((:vst3_host_plugin_name, VST3_LIB), Cstring, ()))
vst3_plugin_id()   = unsafe_string(ccall((:vst3_host_plugin_id, VST3_LIB), Cstring, ()))
vst3_is_open()     = ccall((:vst3_host_is_open, VST3_LIB), Cdouble, ()) > 0.5
vst3_block_size()  = Int(ccall((:vst3_host_block_size, VST3_LIB), Cdouble, ()))
vst3_sample_rate() = ccall((:vst3_host_sample_rate, VST3_LIB), Cdouble, ())
vst3_channels()    = Int(ccall((:vst3_host_channels, VST3_LIB), Cdouble, ()))
vst3_n_process()   = ccall((:vst3_host_n_process, VST3_LIB), Clong, ())
vst3_param_count() = ccall((:vst3_host_n_params, VST3_LIB), Clong, ())
vst3_n_restart_requests() = ccall((:vst3_host_n_restart_requests, VST3_LIB), Clong, ())
vst3_n_plugin_edits()     = ccall((:vst3_host_n_plugin_edits, VST3_LIB), Clong, ())

"""
    vst3_latency() -> Float64

`IAudioProcessor::getLatencySamples()`, in samples, read at open and again
whenever the plugin requests a restart with `kLatencyChanged`. **Not
compensated**, as for CLAP and LV2.
"""
vst3_latency() = ccall((:vst3_host_latency, VST3_LIB), Cdouble, ())

function vst3_reset_counters!()
    ccall((:vst3_host_reset_counters, VST3_LIB), Cvoid, ())
    return nothing
end

"""
    vst3_params() -> Vector{NamedTuple}

The open plugin's parameters as its edit controller describes them:
`id` (the `ParamID`, what `vst3_process` takes), `name`, `units`, the plain
range `min`/`max`, `default` (plain) and `default_normalized`, `steps`
(0 = continuous, 1 = toggle, n = discrete), `readonly` (meters and the like)
and `bypass`. Values sent through `vst3_process` are **normalised** (0..1).
"""
function vst3_params()
    n = vst3_param_count()
    return [(id       = ccall((:vst3_host_param_id, VST3_LIB), Cdouble, (Clong,), i),
             name     = unsafe_string(ccall((:vst3_host_param_name, VST3_LIB), Cstring, (Clong,), i)),
             units    = unsafe_string(ccall((:vst3_host_param_units, VST3_LIB), Cstring, (Clong,), i)),
             min      = ccall((:vst3_host_param_min, VST3_LIB), Cdouble, (Clong,), i),
             max      = ccall((:vst3_host_param_max, VST3_LIB), Cdouble, (Clong,), i),
             default  = ccall((:vst3_host_param_default, VST3_LIB), Cdouble, (Clong,), i),
             default_normalized = ccall((:vst3_host_param_default_normalized, VST3_LIB), Cdouble, (Clong,), i),
             steps    = ccall((:vst3_host_param_steps, VST3_LIB), Cdouble, (Clong,), i),
             readonly = ccall((:vst3_host_param_is_readonly, VST3_LIB), Cdouble, (Clong,), i) > 0.5,
             bypass   = ccall((:vst3_host_param_is_bypass, VST3_LIB), Cdouble, (Clong,), i) > 0.5)
            for i in 0:(n - 1)]
end

"The controller's current normalised value for `param_id`, or `NaN` for an unknown id."
vst3_param_value(param_id::Real) =
    ccall((:vst3_host_param_value, VST3_LIB), Cdouble, (Cdouble,), param_id)

"Convert a normalised value to the controller's plain units for `param_id`; `NaN` for an unknown id."
vst3_param_plain(param_id::Real, normalized::Real) =
    ccall((:vst3_host_param_plain, VST3_LIB), Cdouble, (Cdouble, Cdouble), param_id, normalized)

"Convert a plain value to normalised for `param_id`; `NaN` for an unknown id."
vst3_param_normalized(param_id::Real, plain::Real) =
    ccall((:vst3_host_param_normalized, VST3_LIB), Cdouble, (Cdouble, Cdouble), param_id, plain)

"""
    vst3_fill!(samples; channels = 1) -> token

Fill the input block from `samples` (interleaved when `channels > 1`) and
return its token.
"""
function vst3_fill!(samples::AbstractVector{<:Real}; channels::Integer = 1)
    v = Vector{Cdouble}(samples)
    return ccall((:vst3_in_fill, VST3_LIB), Cdouble, (Ptr{Cdouble}, Clong, Clong),
                 v, length(v) ÷ channels, channels)
end

"""
    vst3_out(token; channel = 0) -> Vector{Float64}

The output block named by `token`; empty when the token is stale.
"""
function vst3_out(token::Real; channel::Integer = 0)
    n = ccall((:vst3_out_count, VST3_LIB), Cdouble, (Cdouble,), token)
    isnan(n) && return Float64[]
    return [ccall((:vst3_out_sample, VST3_LIB), Cdouble, (Cdouble, Cdouble, Cdouble),
                  token, i, channel) for i in 0:(Int(n) - 1)]
end

# ---------------------------------------------------------------------------
# Node-side operators. One equation, one call; same shapes as the CLAP ones.
# Parameter values are normalised (0..1).
# ---------------------------------------------------------------------------

vst3_in_tone(t, waveform, freq, amp) =
    ccall((:vst3_in_tone, VST3_LIB), Cdouble, (Cdouble, Cdouble, Cdouble, Cdouble),
          t, waveform, freq, amp)

vst3_process(dep, id0, v0, id1, v1, id2, v2, id3, v3) =
    ccall((:vst3_process, VST3_LIB), Cdouble,
          (Cdouble,
           Cdouble, Cdouble, Cdouble, Cdouble,
           Cdouble, Cdouble, Cdouble, Cdouble),
          dep, id0, v0, id1, v1, id2, v2, id3, v3)

vst3_out_rms(dep)   = ccall((:vst3_out_rms, VST3_LIB), Cdouble, (Cdouble,), dep)
vst3_out_peak(dep)  = ccall((:vst3_out_peak, VST3_LIB), Cdouble, (Cdouble,), dep)
vst3_out_valid(dep) = ccall((:vst3_out_valid, VST3_LIB), Cdouble, (Cdouble,), dep)
vst3_in_sample(dep, i, ch) =
    ccall((:vst3_in_sample, VST3_LIB), Cdouble, (Cdouble, Cdouble, Cdouble), dep, i, ch)
vst3_out_sample(dep, i, ch) =
    ccall((:vst3_out_sample, VST3_LIB), Cdouble, (Cdouble, Cdouble, Cdouble), dep, i, ch)
