# Hosting an LV2 plugin, mirroring clap_io.jl call for call so that someone
# swapping formats does not have to relearn the API. The implementation is
# csrc/lv2_host.c, built on lilv and shipped prebuilt by LV2Host_jll; the
# operators below are one-line `ccall`s into it.
#
# Discovery is lilv's: the host loads every bundle under an LV2 search path,
# enumerates plugins by URI, and reads each plugin's port layout from its
# Turtle manifest, so a caller names a plugin by URI and never sees a port
# index unless it wants one. Parameters are the control input ports,
# identified by port index -- a small non-negative integer, hence a double
# that a model can use as a parameter id with nothing to keep in sync.
#
# Strings (search paths, URIs) are driver-side only, as in clap_io.jl.

export lv2_lib_path, lv2_src_path, lv2_default_path,
       lv2_scan, lv2_open!, lv2_close!,
       lv2_is_open, lv2_last_error, lv2_plugin_name, lv2_plugin_uri,
       lv2_params, lv2_param_count, lv2_param_value, lv2_latency,
       lv2_block_size, lv2_sample_rate, lv2_channels, lv2_n_process, lv2_reset_counters!,
       lv2_fill!, lv2_out, lv2_test_bundle,
       LV2_WAVE_SILENCE, LV2_WAVE_SINE, LV2_WAVE_SQUARE, LV2_WAVE_RAMP, LV2_WAVE_IMPULSE

using LV2Host_jll: LV2Host_jll, liblv2_host
using lv2_jll: lv2_jll

const LV2_LIB = liblv2_host
const LV2_SRC = normpath(joinpath(@__DIR__, "..", "csrc", "lv2_host.c"))

"""
    lv2_lib_path() -> String

Absolute path of the prebuilt LV2 host library (from `LV2Host_jll`), for a
driver that links the host into a standalone program. Julia callers never
need it. To run against a locally modified `csrc/lv2_host.c`, build it against
lilv and set the `liblv2_host_path` preference on `LV2Host_jll` (see
[`clap_lib_path`](@ref) for the recipe).
"""
lv2_lib_path() = LV2Host_jll.liblv2_host_path::String

"Path of `csrc/lv2_host.c`, which ships with the package for a C program to link."
lv2_src_path() = LV2_SRC

const LV2_WAVE_SILENCE = 0
const LV2_WAVE_SINE    = 1
const LV2_WAVE_SQUARE  = 2
const LV2_WAVE_RAMP    = 3
const LV2_WAVE_IMPULSE = 4

# lilv joins search-path entries with ':' on POSIX and ';' on Windows.
const LV2_PATH_SEP = Sys.iswindows() ? ";" : ":"

"""
    lv2_default_path(dirs...) -> String

An LV2 search path made of `dirs` followed by the LV2 specification bundles
that ship with `lv2_jll`, joined with the platform's separator. The
specification bundles are what let lilv classify ports and plugins (they
define `lv2:AudioPort`, `lv2:latency` and the rest), so every search path
this package builds ends with them; a bare user directory works for
discovery but leaves lilv without the vocabulary to describe what it found.
Pass the result to [`lv2_scan`](@ref) or [`lv2_open!`](@ref).
"""
function lv2_default_path(dirs::AbstractString...)
    spec = dirname(dirname(lv2_jll.lv2_core_manifest))   # .../lib/lv2
    return join([dirs..., spec], LV2_PATH_SEP)
end

"""
    lv2_test_bundle(; force = false) -> String

Build the LV2 bundle of test plugins that ships with this package
(`test/plugins/ap_test_lv2.c` + `ap_test_lv2.ttl`: `urn:audioplugins:test:gain`,
`:onepole`, `:lookahead`) into a per-package scratch space and return the
directory that *contains* the bundle, ready to be passed as a search path.
Needs a C compiler, like [`clap_test_bundle`](@ref), and only for tests.
"""
function lv2_test_bundle(; force::Bool = false)
    plugdir = normpath(joinpath(@__DIR__, "..", "test", "plugins"))
    src = joinpath(plugdir, "ap_test_lv2.c")
    ttl = joinpath(plugdir, "ap_test_lv2.ttl")
    root = @get_scratch!("test_plugins")
    bundle = joinpath(root, "ap_test.lv2")
    dlext = Sys.iswindows() ? "dll" : Sys.isapple() ? "dylib" : "so"
    bin = joinpath(bundle, "ap_test." * dlext)
    stale(f) = !isfile(f) || stat(src).mtime > stat(f).mtime || stat(ttl).mtime > stat(f).mtime
    if force || stale(bin)
        cc = _c_compiler()
        cc === nothing && error(
            "lv2_test_bundle: building the test plugins needs a C compiler on PATH " *
            "(tried cc, gcc, clang). Hosting itself does not: the host library comes " *
            "prebuilt from LV2Host_jll.")
        mkpath(bundle)
        run(`$cc -O2 -fPIC -shared -Wall -Wextra -o $bin $src`)
        cp(ttl, joinpath(bundle, "ap_test.ttl"); force = true)
        # The manifest names the binary, so it carries the platform's extension.
        open(joinpath(bundle, "manifest.ttl"), "w") do io
            println(io, "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .")
            println(io, "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .")
            for p in ("gain", "onepole", "lookahead")
                println(io, "<urn:audioplugins:test:$p> a lv2:Plugin ; ",
                            "lv2:binary <ap_test.$dlext> ; rdfs:seeAlso <ap_test.ttl> .")
            end
        end
    end
    return root
end

# ---------------------------------------------------------------------------
# Driver-side lifecycle and discovery. Strings live here and nowhere else.
# ---------------------------------------------------------------------------

"""
    lv2_scan(lv2_path) -> Vector{@NamedTuple{uri::String, name::String}}

Load every bundle under `lv2_path` (directories joined by the platform's
separator; see [`lv2_default_path`](@ref)) and enumerate the plugins, without
instantiating anything. Pass `""` for lilv's default search path (`LV2_PATH`,
or the platform's standard directories). Throws with the host's own message
when nothing is found.
"""
function lv2_scan(lv2_path::AbstractString)
    n = ccall((:lv2_host_scan, LV2_LIB), Clong, (Cstring,), lv2_path)
    n < 0 && error("lv2_scan($(repr(lv2_path))) failed: $(lv2_last_error())")
    return [(uri  = unsafe_string(ccall((:lv2_host_scan_uri, LV2_LIB), Cstring, (Clong,), i)),
             name = unsafe_string(ccall((:lv2_host_scan_name, LV2_LIB), Cstring, (Clong,), i)))
            for i in 0:(n - 1)]
end

"""
    lv2_open!(lv2_path; uri = "", sample_rate = 48000, block_size = 512, channels = 1)

Instantiate and activate the plugin `uri` found under `lv2_path` at a **fixed**
block size. `channels` is the number of host audio channels: the k-th audio
input port gets host channel `min(k, channels-1)` (a mono host feeds every
input of a stereo plugin) and the k-th audio output port writes host channel
k, or is discarded when `k >= channels`; the plugin must have at least
`channels` audio outputs. Fails loudly for a plugin that requires a host
feature or a port class this host does not provide.
"""
function lv2_open!(lv2_path::AbstractString; uri::AbstractString = "",
                   sample_rate::Real = 48000, block_size::Integer = 512,
                   channels::Integer = 1)
    r = ccall((:lv2_host_open, LV2_LIB), Cint,
              (Cstring, Cstring, Cdouble, Cdouble, Cdouble),
              lv2_path, uri, sample_rate, block_size, channels)
    r == 0 || error("lv2_open!($(repr(uri))) failed: $(lv2_last_error())")
    return nothing
end

"Deactivate, free and unload. Safe when nothing is open."
function lv2_close!()
    ccall((:lv2_host_close, LV2_LIB), Cvoid, ())
    return nothing
end

lv2_last_error()  = unsafe_string(ccall((:lv2_host_last_error, LV2_LIB), Cstring, ()))
lv2_plugin_name() = unsafe_string(ccall((:lv2_host_plugin_name, LV2_LIB), Cstring, ()))
lv2_plugin_uri()  = unsafe_string(ccall((:lv2_host_plugin_uri, LV2_LIB), Cstring, ()))
lv2_is_open()     = ccall((:lv2_host_is_open, LV2_LIB), Cdouble, ()) > 0.5
lv2_block_size()  = Int(ccall((:lv2_host_block_size, LV2_LIB), Cdouble, ()))
lv2_sample_rate() = ccall((:lv2_host_sample_rate, LV2_LIB), Cdouble, ())
lv2_channels()    = Int(ccall((:lv2_host_channels, LV2_LIB), Cdouble, ()))
lv2_n_process()   = ccall((:lv2_host_n_process, LV2_LIB), Clong, ())
lv2_param_count() = ccall((:lv2_host_n_params, LV2_LIB), Clong, ())
lv2_n_audio_in()  = Int(ccall((:lv2_host_n_audio_in, LV2_LIB), Cdouble, ()))
lv2_n_audio_out() = Int(ccall((:lv2_host_n_audio_out, LV2_LIB), Cdouble, ()))

"""
    lv2_latency() -> Float64

Latency the plugin reports on its designated `lv2:latency` port, in samples
(0 when it has none). A plugin writes that port from `run()`, so the value
is authoritative after the first block. **Not compensated**, as for CLAP.
"""
lv2_latency() = ccall((:lv2_host_latency, LV2_LIB), Cdouble, ())

function lv2_reset_counters!()
    ccall((:lv2_host_reset_counters, LV2_LIB), Cvoid, ())
    return nothing
end

"""
    lv2_params() -> Vector{@NamedTuple{id, name, symbol, min, max, default}}

The open plugin's control input ports, read from its manifest through lilv.
`id` is the port index, which is what `lv2_process` takes. `min`/`max`/
`default` are `NaN` when the manifest does not give them.
"""
function lv2_params()
    n = lv2_param_count()
    return [(id      = ccall((:lv2_host_param_id, LV2_LIB), Cdouble, (Clong,), i),
             name    = unsafe_string(ccall((:lv2_host_param_name, LV2_LIB), Cstring, (Clong,), i)),
             symbol  = unsafe_string(ccall((:lv2_host_param_symbol, LV2_LIB), Cstring, (Clong,), i)),
             min     = ccall((:lv2_host_param_min, LV2_LIB), Cdouble, (Clong,), i),
             max     = ccall((:lv2_host_param_max, LV2_LIB), Cdouble, (Clong,), i),
             default = ccall((:lv2_host_param_default, LV2_LIB), Cdouble, (Clong,), i))
            for i in 0:(n - 1)]
end

"The value connected to control input port `port_index`, or `NaN` if it is not one."
lv2_param_value(port_index::Real) =
    ccall((:lv2_host_param_value, LV2_LIB), Cdouble, (Cdouble,), port_index)

"""
    lv2_fill!(samples; channels = 1) -> token

Fill the input block from `samples` (interleaved when `channels > 1`) and
return its token.
"""
function lv2_fill!(samples::AbstractVector{<:Real}; channels::Integer = 1)
    v = Vector{Cdouble}(samples)
    return ccall((:lv2_in_fill, LV2_LIB), Cdouble, (Ptr{Cdouble}, Clong, Clong),
                 v, length(v) ÷ channels, channels)
end

"""
    lv2_out(token; channel = 0) -> Vector{Float64}

The output block named by `token`; empty when the token is stale.
"""
function lv2_out(token::Real; channel::Integer = 0)
    n = ccall((:lv2_out_count, LV2_LIB), Cdouble, (Cdouble,), token)
    isnan(n) && return Float64[]
    return [ccall((:lv2_out_sample, LV2_LIB), Cdouble, (Cdouble, Cdouble, Cdouble),
                  token, i, channel) for i in 0:(Int(n) - 1)]
end

# ---------------------------------------------------------------------------
# Node-side operators. One equation, one call; same shapes as the CLAP ones.
# ---------------------------------------------------------------------------

lv2_in_tone(t, waveform, freq, amp) =
    ccall((:lv2_in_tone, LV2_LIB), Cdouble, (Cdouble, Cdouble, Cdouble, Cdouble),
          t, waveform, freq, amp)

lv2_process(dep, id0, v0, id1, v1, id2, v2, id3, v3) =
    ccall((:lv2_process, LV2_LIB), Cdouble,
          (Cdouble,
           Cdouble, Cdouble, Cdouble, Cdouble,
           Cdouble, Cdouble, Cdouble, Cdouble),
          dep, id0, v0, id1, v1, id2, v2, id3, v3)

lv2_out_rms(dep)   = ccall((:lv2_out_rms, LV2_LIB), Cdouble, (Cdouble,), dep)
lv2_out_peak(dep)  = ccall((:lv2_out_peak, LV2_LIB), Cdouble, (Cdouble,), dep)
lv2_out_valid(dep) = ccall((:lv2_out_valid, LV2_LIB), Cdouble, (Cdouble,), dep)
lv2_in_sample(dep, i, ch) =
    ccall((:lv2_in_sample, LV2_LIB), Cdouble, (Cdouble, Cdouble, Cdouble), dep, i, ch)
lv2_out_sample(dep, i, ch) =
    ccall((:lv2_out_sample, LV2_LIB), Cdouble, (Cdouble, Cdouble, Cdouble), dep, i, ch)
