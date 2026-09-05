# Hosting a CLAP plugin from inside a Dyad synchronous node.
#
# `ClapEffect` (dyad/plugins.dyad) runs the plugin *inside* the compiled node
# rather than in a surrounding loop. The implementation is csrc/clap_host.c and
# the operators below are one-line `ccall`s into it.
#
# Named `ccall((:sym, lib), ...)` rather than Julia callbacks is what makes one
# component definition serve all three targets:
#
#   backend = :julia   Julia ccalls the shared library directly.
#   backend = :c       SynchCompiler links the library into the node's .so.
#   export_c           the emitted top.c declares
#                        extern double clap_process(double, ...);
#                      and links against csrc/clap_host.c with no Julia involved.
#
# The same two load-bearing properties the audio and vision boundaries carry:
#
#  1. Exactly one process() per tick. stkcompile does no CSE and no DCE, so one
#     equation is one call; a second call would advance the plugin's internal
#     state twice for one block of time.
#  2. Ordering. `clap_process` takes the input block's token as its `dep`, and a
#     token that does not name the current block is refused with NaN rather than
#     answered from whatever the buffer holds.
#
# A plugin path and a plugin id are strings, so they cannot cross a synchronous
# interface: they are `structural parameter`s driver-side, and `clap_open!` must
# be called with the same ones the model was built against.


export build_clap_host!, clap_host_available, clap_lib_path, clap_src_path, clap_scan,
       clap_open!, clap_close!,
       clap_is_open, clap_last_error, clap_plugin_name,
       clap_params, clap_param_count, clap_latency,
       clap_block_size, clap_sample_rate, clap_n_process, clap_reset_counters!,
       clap_fill!, clap_out, clap_test_bundle,
       CLAP_WAVE_SILENCE, CLAP_WAVE_SINE, CLAP_WAVE_SQUARE,
       CLAP_WAVE_RAMP, CLAP_WAVE_IMPULSE

# The host library. `libclap_host` is what `ccall` needs: the JLL loads the
# library in its `__init__` and this is the soname it is registered under,
# so it is a compile-time constant that survives a relocated depot. The
# absolute path -- what a generated C program links against -- is
# `clap_lib_path()`, and the C source it was built from is `clap_src_path()`.
using CLAPHost_jll: CLAPHost_jll
using Scratch: @get_scratch!

const CLAP_HOST_AVAILABLE = CLAPHost_jll.is_available()
# Where the JLL has no build, the soname stands in so the module still loads
# and authoring works; a hosting call then fails to load the library.
const CLAP_LIB = CLAP_HOST_AVAILABLE ? CLAPHost_jll.libclap_host : "libclap_host"
const CLAP_SRC = normpath(joinpath(@__DIR__, "..", "csrc", "clap_host.c"))

"""
    clap_lib_path() -> String

Absolute path of the prebuilt CLAP host library (from `CLAPHost_jll`). This is
what a driver that links the host into a standalone program wants; Julia
callers never need it because every `ccall` here goes through [`CLAP_LIB`].

To run against a locally modified `csrc/clap_host.c` instead, build it with
your C compiler and point the JLL at it through a preference:

    using Preferences, CLAPHost_jll
    set_preferences!(CLAPHost_jll, "libclap_host_path" => "/path/to/libclap_host.so")

then restart Julia.
"""
function clap_lib_path()
    CLAP_HOST_AVAILABLE ||
        error("CLAPHost_jll has no build of the CLAP host for this platform " *
              "($(Base.BinaryPlatforms.host_triplet())); host from a C program over " *
              "csrc/clap_host.c instead, see clap_host_available()")
    return CLAPHost_jll.libclap_host_path::String
end

"""
    clap_host_available() -> Bool

Whether `CLAPHost_jll` ships the prebuilt host for this platform. Where it
does not (Windows, until https://github.com/JuliaPackaging/Yggdrasil/pull/14685
lands), the module loads and [`export_plugin`](@ref) works, but the
`clap_*` hosting functions cannot load the host: host from a C program
over `csrc/clap_host.c` instead, as `test/export/probe_step.c` does.
"""
clap_host_available() = CLAP_HOST_AVAILABLE

"""
    clap_src_path() -> String

Path of `csrc/clap_host.c`, which ships with the package so that a generated
standalone C program can link the host directly with no Julia present. The
vendored CLAP headers it needs are next to it under `csrc/vendor/`.
"""
clap_src_path() = CLAP_SRC

# Waveform codes for `clp_in_tone`, mirroring the CLAP_WAVE_* macros in
# csrc/clap_host.h. Integers rather than strings because they are arguments to a
# clocked equation: a test source is described entirely by its own parameters,
# with nothing to keep in sync driver-side.
const CLAP_WAVE_SILENCE = 0
const CLAP_WAVE_SINE    = 1
const CLAP_WAVE_SQUARE  = 2
const CLAP_WAVE_RAMP    = 3
const CLAP_WAVE_IMPULSE = 4

"Path of a C compiler to build the test plugins with, or `nothing`."
function _c_compiler()
    for c in ("cc", "gcc", "clang")
        p = Sys.which(c)
        p === nothing || return p
    end
    return nothing
end

"""
    build_clap_host!(; force = false)

Deprecated. The host library is prebuilt and shipped by `CLAPHost_jll`, so
there is nothing to build; this returns [`clap_lib_path`](@ref) for callers
written against the 1.0 API. To work on the C source, see [`clap_lib_path`](@ref).
"""
function build_clap_host!(; force::Bool = false)
    Base.depwarn("build_clap_host! is deprecated and a no-op: the CLAP host is shipped prebuilt by " *
                 "CLAPHost_jll. Use clap_lib_path() for its location.", :build_clap_host!)
    return clap_lib_path()
end

"""
    clap_test_bundle(; force = false)

Build the bundle of test plugins that ships with this package
(`test/plugins/ap_test_plugins.c`: `ap.gain`, `ap.onepole`, `ap.lookahead`)
and return its path.

Hosting is only proved by hosting something, and depending on a third-party
plugin would make the suite depend on a binary whose arithmetic we cannot check
and may not be able to fetch. These are ours, and their output is analytic.

This is the one place the package needs a C compiler, and it is only needed
to run the tests. The bundle is built into a per-package scratch space rather
than into the package directory, so a read-only installation still works for
hosting and fails here, at test time, with a message that says why.
"""
function clap_test_bundle(; force::Bool = false)
    src = normpath(joinpath(@__DIR__, "..", "test", "plugins", "ap_test_plugins.c"))
    dir = @get_scratch!("test_plugins")
    out = joinpath(dir, "ap_test.clap")
    if force || !isfile(out) || stat(src).mtime > stat(out).mtime
        cc = _c_compiler()
        cc === nothing && error(
            "clap_test_bundle: building the test plugins needs a C compiler on PATH " *
            "(tried cc, gcc, clang). Hosting itself does not: the host library comes " *
            "prebuilt from CLAPHost_jll.")
        run(`$cc -O2 -fPIC -shared -Wall -Wextra -o $out $src`)
    end
    return out
end

# ---------------------------------------------------------------------------
# Driver-side lifecycle and discovery. Strings live here and nowhere else.
# ---------------------------------------------------------------------------

"""
    clap_scan(path) -> Vector{@NamedTuple{id::String, name::String}}

Enumerate a `.clap` bundle without instantiating anything. Throws with the
host's own message when the bundle cannot be loaded.
"""
function clap_scan(path::AbstractString)
    n = ccall((:clap_host_scan, CLAP_LIB), Clong, (Cstring,), path)
    n < 0 && error("clap_scan($(repr(path))) failed: $(clap_last_error())")
    return [(id = unsafe_string(ccall((:clap_host_scan_id, CLAP_LIB), Cstring, (Clong,), i)),
             name = unsafe_string(ccall((:clap_host_scan_name, CLAP_LIB), Cstring, (Clong,), i)))
            for i in 0:(n - 1)]
end

"""
    clap_open!(path; plugin_id = "", sample_rate = 48000, block_size = 512, channels = 1)

Instantiate and activate a plugin at a **fixed** block size: the plugin is
activated with `min == max == block_size`, so one that cannot work at a fixed
block fails here, loudly, rather than at the first tick.

`block_size` is the editing-chain contract — it must equal the number of frames
each tick carries, or the stream is not contiguous.
"""
function clap_open!(path::AbstractString; plugin_id::AbstractString = "",
                    sample_rate::Real = 48000, block_size::Integer = 512,
                    channels::Integer = 1)
    r = ccall((:clap_host_open, CLAP_LIB), Cint,
              (Cstring, Cstring, Cdouble, Cdouble, Cdouble),
              path, plugin_id, sample_rate, block_size, channels)
    r == 0 || error("clap_open!($(repr(path))) failed: $(clap_last_error())")
    return nothing
end

"Deactivate, destroy and unload. Safe when nothing is open."
function clap_close!()
    ccall((:clap_host_close, CLAP_LIB), Cvoid, ())
    return nothing
end

clap_last_error()  = unsafe_string(ccall((:clap_host_last_error, CLAP_LIB), Cstring, ()))
clap_plugin_name() = unsafe_string(ccall((:clap_host_plugin_name, CLAP_LIB), Cstring, ()))
clap_is_open()     = ccall((:clap_host_is_open, CLAP_LIB), Cdouble, ()) > 0.5
clap_block_size()  = Int(ccall((:clap_host_block_size, CLAP_LIB), Cdouble, ()))
clap_sample_rate() = ccall((:clap_host_sample_rate, CLAP_LIB), Cdouble, ())
clap_n_process()   = ccall((:clap_host_n_process, CLAP_LIB), Clong, ())
clap_param_count() = ccall((:clap_host_n_params, CLAP_LIB), Clong, ())

"""
    clap_latency() -> Float64

Latency the plugin reports, in samples. **Not compensated** — hosting a
lookahead plugin leaves its output shifted by this many samples relative to the
input, and a model that cares must align downstream itself.
"""
clap_latency() = ccall((:clap_host_latency, CLAP_LIB), Cdouble, ())

function clap_reset_counters!()
    ccall((:clap_host_reset_counters, CLAP_LIB), Cvoid, ())
    return nothing
end

"""
    clap_params() -> Vector{@NamedTuple{id, name, min, max, default}}

The open plugin's automatable parameters. The `id`s are what a model passes to
`ClapEffect`'s slots, which is why they are numbers: a `clap_id` is a `uint32`
and every `uint32` is exactly representable as a `Float64`, so a model can name
its own parameters with nothing to keep in sync driver-side.
"""
function clap_params()
    n = clap_param_count()
    # Written out rather than looped over a symbol: a ccall's function name and
    # library must be literal, not a local variable.
    return [(id      = ccall((:clap_host_param_id, CLAP_LIB), Cdouble, (Clong,), i),
             name    = unsafe_string(ccall((:clap_host_param_name, CLAP_LIB),
                                           Cstring, (Clong,), i)),
             min     = ccall((:clap_host_param_min, CLAP_LIB), Cdouble, (Clong,), i),
             max     = ccall((:clap_host_param_max, CLAP_LIB), Cdouble, (Clong,), i),
             default = ccall((:clap_host_param_default, CLAP_LIB), Cdouble, (Clong,), i))
            for i in 0:(n - 1)]
end

"The plugin's own current value for `param_id`, or `NaN` for an unknown id."
clap_param_value(param_id::Real) =
    ccall((:clap_host_param_value, CLAP_LIB), Cdouble, (Cdouble,), param_id)

"""
    clap_fill!(samples; channels = 1) -> token

Fill the input block from `samples` (per channel) and return its token, so a
driver or a test can supply audio the node then processes.
"""
function clap_fill!(samples::AbstractVector{<:Real}; channels::Integer = 1)
    v = Vector{Cdouble}(samples)
    return ccall((:clap_in_fill, CLAP_LIB), Cdouble, (Ptr{Cdouble}, Clong, Clong),
                 v, length(v) ÷ channels, channels)
end

"""
    clap_out(token; channel = 0) -> Vector{Float64}

The output block named by `token`. Empty when the token is stale — the same
refusal the node-side accessors make, so a test cannot accidentally check
yesterday's audio.
"""
function clap_out(token::Real; channel::Integer = 0)
    n = ccall((:clap_out_count, CLAP_LIB), Cdouble, (Cdouble,), token)
    isnan(n) && return Float64[]
    return [ccall((:clap_out_sample, CLAP_LIB), Cdouble, (Cdouble, Cdouble, Cdouble),
                  token, i, channel) for i in 0:(Int(n) - 1)]
end

# ---------------------------------------------------------------------------
# Node-side operators. One equation, one call. Each takes the token it
# depends on, so nothing can be scheduled before the block it reads.
# ---------------------------------------------------------------------------

clp_in_tone(t, waveform, freq, amp) =
    ccall((:clap_in_tone, CLAP_LIB), Cdouble, (Cdouble, Cdouble, Cdouble, Cdouble),
          t, waveform, freq, amp)

clp_process(dep, id0, v0, id1, v1, id2, v2, id3, v3) =
    ccall((:clap_process, CLAP_LIB), Cdouble,
          (Cdouble,                                      # dep
           Cdouble, Cdouble, Cdouble, Cdouble,           # slots 0, 1
           Cdouble, Cdouble, Cdouble, Cdouble),          # slots 2, 3
          dep, id0, v0, id1, v1, id2, v2, id3, v3)

clp_out_rms(dep)   = ccall((:clap_out_rms, CLAP_LIB), Cdouble, (Cdouble,), dep)
clp_out_peak(dep)  = ccall((:clap_out_peak, CLAP_LIB), Cdouble, (Cdouble,), dep)
clp_out_valid(dep) = ccall((:clap_out_valid, CLAP_LIB), Cdouble, (Cdouble,), dep)
clp_in_sample(dep, i, ch) =
    ccall((:clap_in_sample, CLAP_LIB), Cdouble, (Cdouble, Cdouble, Cdouble), dep, i, ch)
clp_out_sample(dep, i, ch) =
    ccall((:clap_out_sample, CLAP_LIB), Cdouble, (Cdouble, Cdouble, Cdouble), dep, i, ch)
