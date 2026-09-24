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
    clap_descriptors,
    clap_open!, clap_close!,
    clap_is_open, clap_last_error, clap_plugin_name,
    clap_params, clap_param_count, clap_latency, clap_compensating, clap_flush!,
    clap_block_size, clap_sample_rate, clap_n_process, clap_reset_counters!,
    clap_n_audio_in, clap_n_audio_out,
    clap_plugin_index,
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
        error(
        "CLAPHost_jll has no build of the CLAP host for this platform " *
            "($(Base.BinaryPlatforms.host_triplet())); host from a C program over " *
            "csrc/clap_host.c instead, see clap_host_available()"
    )
    return CLAPHost_jll.libclap_host_path::String
end

"""
    clap_host_available() -> Bool

Whether `CLAPHost_jll` ships the prebuilt host for this platform. It does
for Linux, macOS and Windows. Where it does not, the module
loads and [`export_plugin`](@ref) works, but the `clap_*` hosting
functions cannot load the host: host from a C program over
`csrc/clap_host.c` instead, as `test/export/probe_step.c` does.
"""
clap_host_available() = CLAP_HOST_AVAILABLE

"""
    clap_src_path() -> String

Path of `csrc/clap_host.c`, which ships with the package so that a generated
standalone C program can link the host directly with no Julia present. The
vendored CLAP headers it needs are next to it under `csrc/vendor/`.
"""
clap_src_path() = CLAP_SRC

"""
    CLAP_WAVE_SILENCE

Waveform code for `clp_in_tone`: a constant zero signal.
"""
const CLAP_WAVE_SILENCE = 0

"""
    CLAP_WAVE_SINE

Waveform code for `amp * sin(2π * freq * t)`.

The `CLAP_WAVE_*` codes are the `waveform` argument of the host's node-side
tone source, mirroring the `CLAP_WAVE_*` macros in `csrc/clap_host.h`. They are
integers rather than symbols because they are arguments to a clocked equation:
a test source is then described entirely by its own parameters, with nothing to
keep in sync driver-side. A code the host does not know produces silence.
"""
const CLAP_WAVE_SINE = 1

"""
    CLAP_WAVE_SQUARE

Waveform code for a square wave: `±amp`, following the sign of the sine of the
same phase. See [`CLAP_WAVE_SINE`](@ref) for the family.
"""
const CLAP_WAVE_SQUARE = 2

"""
    CLAP_WAVE_RAMP

Waveform code for `clp_in_tone`: a sawtooth rising from `-amplitude` to
`+amplitude` once per period.
"""
const CLAP_WAVE_RAMP = 3

"""
    CLAP_WAVE_IMPULSE

Waveform code for a single sample of `amp` at sample index 0 and zero
afterwards, for measuring an impulse response. See [`CLAP_WAVE_SINE`](@ref) for
the family.
"""
const CLAP_WAVE_IMPULSE = 4

"Path of a C compiler to build the test plugins with, or `nothing`."
function _c_compiler()
    for c in ("cc", "gcc", "clang")
        p = Sys.which(c)
        p === nothing || return p
    end
    return nothing
end

# The compiler targets the machine, but everything built here has to be loaded
# by *this* Julia process: a 32-bit Julia on x86_64 hardware needs -m32, or the
# artefact comes out ELFCLASS64 and will not dlopen.
_c_arch_flags() = (Sys.WORD_SIZE == 32 && Sys.ARCH === :i686) ? ["-m32"] : String[]

# Build a test bundle, folding the compiler's diagnostics into the exception.
# Plain `run` leaves them wherever the caller's stderr went, which in a CI log is
# far from the `failed process` error and gone entirely when the runner captures
# output -- leaving a failed link reported as a command line and no reason.
function _run_build(cmd::Cmd, what::AbstractString)
    err = IOBuffer()
    p = run(pipeline(ignorestatus(cmd); stderr = err))
    if !success(p)
        msg = strip(String(take!(err)))
        error(
            "$what: the compiler failed (exit $(p.exitcode)).\ncommand: $cmd\n" *
                (isempty(msg) ? "the compiler printed nothing to stderr." : msg)
        )
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
    Base.depwarn(
        "build_clap_host! is deprecated and a no-op: the CLAP host is shipped prebuilt by " *
            "CLAPHost_jll. Use clap_lib_path() for its location.", :build_clap_host!
    )
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
    dir = @get_scratch!("test_plugins-$(Sys.ARCH)")
    out = joinpath(dir, "ap_test.clap")
    if force || !isfile(out) || stat(src).mtime > stat(out).mtime
        cc = _c_compiler()
        cc === nothing && error(
            "clap_test_bundle: building the test plugins needs a C compiler on PATH " *
                "(tried cc, gcc, clang). Hosting itself does not: the host library comes " *
                "prebuilt from CLAPHost_jll."
        )
        _run_build(`$cc $(_c_arch_flags()) -O2 -fPIC -shared -Wall -Wextra -o $out $src`, "clap_test_bundle")
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

!!! warning
    Scanning **closes the default plugin**. Independent [`ClapInstance`](@ref)
    effects remain open.

[`register_bundle!`](@ref) is the same enumeration, remembered, so that a
plugin can be opened by id without naming its bundle again.
"""
function clap_scan(path::AbstractString)
    _COMP[] = nothing    # the C scan closes any open plugin; so does this one
    n = ccall((:clap_host_scan, CLAP_LIB), Clong, (Cstring,), path)
    n < 0 && error("clap_scan($(repr(path))) failed: $(clap_last_error())")
    return [
        (
            id = unsafe_string(ccall((:clap_host_scan_id, CLAP_LIB), Cstring, (Clong,), i)),
            name = unsafe_string(ccall((:clap_host_scan_name, CLAP_LIB), Cstring, (Clong,), i)),
        )
            for i in 0:(n - 1)
    ]
end

"""
    clap_descriptors(path) -> Vector{@NamedTuple{id, name, vendor, version, description, features}}

Everything a `.clap` bundle's descriptors declare, for the plugins in it, in
factory order. [`clap_scan`](@ref) is the same enumeration reduced to what
opening a plugin needs; this is what a *generator* needs — a per-effect
component library takes its docstrings, its grouping and its version pin from
these fields, and reading them from the module is what keeps the generated
library honest about which build it was generated from.

`features` is the plugin's CLAP feature list, which is where a collection's own
taxonomy usually appears: the Airwindows adapter emits `airwindows:<category>`
alongside the standard keywords. It truncates at twelve entries.

Scans, so it closes the default plugin — the same caveat as
[`clap_scan`](@ref).
"""
function clap_descriptors(path::AbstractString)
    _COMP[] = nothing
    n = ccall((:clap_host_scan, CLAP_LIB), Clong, (Cstring,), path)
    n < 0 && error("clap_descriptors($(repr(path))) failed: $(clap_last_error())")
    # Written out rather than looped over a symbol, for the reason clap_params
    # gives: a ccall's function name is part of its syntax and cannot come from
    # a variable.
    return [
        (
            id = unsafe_string(ccall((:clap_host_scan_id, CLAP_LIB), Cstring, (Clong,), i)),
            name = unsafe_string(ccall((:clap_host_scan_name, CLAP_LIB), Cstring, (Clong,), i)),
            vendor = unsafe_string(ccall((:clap_host_scan_vendor, CLAP_LIB), Cstring, (Clong,), i)),
            version = unsafe_string(ccall((:clap_host_scan_version, CLAP_LIB), Cstring, (Clong,), i)),
            description = unsafe_string(
                ccall((:clap_host_scan_description, CLAP_LIB), Cstring, (Clong,), i)
            ),
            features = [
                unsafe_string(
                    ccall((:clap_host_scan_feature, CLAP_LIB), Cstring, (Clong, Clong), i, k)
                )
                    for k in 0:(ccall((:clap_host_scan_n_features, CLAP_LIB), Clong, (Clong,), i) - 1)
            ],
        )
            for i in 0:(n - 1)
    ]
end

"""
    clap_open!(path; plugin_id = "", sample_rate = 48000, block_size = 512,
               channels = 1, compensate_latency = false)
    clap_open!(plugin_id; sample_rate = 48000, block_size = 512,
               channels = 1, compensate_latency = false)

Instantiate and activate a plugin at a **fixed** block size: the plugin is
activated with `min == max == block_size`, so one that cannot work at a fixed
block fails here, loudly, rather than at the first tick.

`block_size` is the editing-chain contract — it must equal the number of frames
each tick carries, or the stream is not contiguous.

The first argument is a bundle path, or — when no `plugin_id` is given and it
is not a path — the id of a plugin some [`register_bundle!`](@ref) has put in
the registry, which is how a plugin collection shipped as a JLL is opened
without naming a file. With an empty registry the two are the same thing and
this behaves exactly as it always did.

`compensate_latency` opts in to Julia-side latency compensation — see
[`clap_out`](@ref) and [`clap_flush!`](@ref). It is off by default: the
documented behaviour is that [`clap_latency`](@ref) is surfaced, not
compensated, and a generated C program linking `csrc/` never sees the mode.

`channels` is the number of **host** channels — the width of the block
[`clap_fill!`](@ref) supplies and [`clap_out`](@ref) returns — not the number
of channels the plugin declares. At open, while still deactivated, the plugin
is asked for its `clap.audio-ports` layout and is handed buffers matching it
exactly, but only the **main** port (port 0) is routed: main input channel *k*
receives host channel `min(k, channels-1)`, and every non-main input — a
sidechain that was never routed — reads silence, as it would in a DAW. Main
output channel *k* lands on host channel *k* when `k < channels` and is
discarded past it (a stereo plugin at `channels = 1` loses its right
channel), and every non-main output is discarded. A mono main output at
`channels = 2` is duplicated onto both host channels, and a plugin with no
output ports produces silence. Asking for more input channels than the main
input declares — a mono-input plugin at `channels = 2` — fails at open with
an error naming the layout. A plugin without `clap.audio-ports` has, per the
spec, no audio ports and produces silence. [`clap_n_audio_in`](@ref) and
[`clap_n_audio_out`](@ref) report the declared channel totals of the plugin
that ended up wired.
"""
function clap_open!(
        path::AbstractString; plugin_id::AbstractString = "",
        sample_rate::Real = 48000, block_size::Integer = 512,
        channels::Integer = 1, compensate_latency::Bool = false
    )
    bundle, id = _resolve_plugin(path, plugin_id)
    _COMP[] = nothing    # a failed open below still closed whatever was open
    r = ccall(
        (:clap_host_open, CLAP_LIB), Cint,
        (Cstring, Cstring, Cdouble, Cdouble, Cdouble),
        bundle, id, sample_rate, block_size, channels
    )
    r == 0 || error("clap_open!($(repr(path))) failed: $(clap_last_error())")
    compensate_latency &&
        (_COMP[] = _LatencyComp(Int(clap_latency()), Int(channels)))
    return nothing
end

# Which bundle, and which plugin in it. An explicit `plugin_id`, or a string
# that names a file, is taken at face value: a caller who said where to look is
# never second-guessed, and a registry that is empty -- the default, since no
# collection ships with this package -- cannot change any existing behaviour.
function _resolve_plugin(path::AbstractString, plugin_id::AbstractString)
    (!isempty(plugin_id) || ispath(path)) && return (path, plugin_id)
    hit = find_plugin(path)
    hit === nothing && return (path, plugin_id)   # let the host give its own error
    return (hit.bundle, hit.id)
end

"Deactivate, destroy and unload. Safe when nothing is open."
function clap_close!()
    _COMP[] = nothing
    ccall((:clap_host_close, CLAP_LIB), Cvoid, ())
    return nothing
end

"""
    clap_last_error() -> String

Human-readable reason for the last host failure, or `""` when there has not
been one. [`clap_open!`](@ref) and [`clap_scan`](@ref) already fold this into
the error they throw.
"""
clap_last_error() = unsafe_string(ccall((:clap_host_last_error, CLAP_LIB), Cstring, ()))

"""
    clap_plugin_name() -> String

Name the open plugin reports for itself, or an empty string when nothing is open.
"""
clap_plugin_name() = unsafe_string(ccall((:clap_host_plugin_name, CLAP_LIB), Cstring, ()))

"""
    clap_plugin_index() -> Int

Index of the open plugin within the bundle it was opened from — the `index`
field [`plugins`](@ref) reports — or `-1` when nothing is open. It is what
[`clp_expect`](@ref AudioPlugins.clp_expect) compares against, so a driver can
assert node-side and driver-side agree on which plugin is meant.
"""
clap_plugin_index() = Int(ccall((:clap_host_open_index, CLAP_LIB), Clong, ()))

"""
    clap_is_open() -> Bool

Whether a plugin is currently open. A failed [`clap_open!`](@ref) leaves this
`false`: there is no half-open state.
"""
clap_is_open() = ccall((:clap_host_is_open, CLAP_LIB), Cdouble, ()) > 0.5

"""
    clap_block_size() -> Int

Block size in force, in frames — the `block_size` [`clap_open!`](@ref) was
called with. The plugin was activated with `min == max == block_size`, so this
is a fixed contract rather than a maximum, and it is what a caller must feed
per tick for the stream to stay contiguous.
"""
clap_block_size() = Int(ccall((:clap_host_block_size, CLAP_LIB), Cdouble, ()))

"""
    clap_sample_rate() -> Float64

Sample rate the open plugin was activated with, in Hz.
"""
clap_sample_rate() = ccall((:clap_host_sample_rate, CLAP_LIB), Cdouble, ())

"""
    clap_n_process() -> Int

Number of `process()` calls made since the plugin was opened, or since the last
[`clap_reset_counters!`](@ref). Exactly one per tick is the property the tests
assert: a second call would advance the plugin's internal state twice for one
block of time.
"""
clap_n_process() = ccall((:clap_host_n_process, CLAP_LIB), Clong, ())

"""
    clap_param_count() -> Int

Number of parameters the open plugin exposes, i.e. `length(clap_params())`.
"""
clap_param_count() = ccall((:clap_host_n_params, CLAP_LIB), Clong, ())

"""
    clap_n_audio_in() -> Int

Total number of audio channels the open plugin declared in its input
direction, summed across its `clap.audio-ports` ports. A compressor with a
mono main input and a mono sidechain reports `2` here even when
[`clap_open!`](@ref) was called with `channels = 1`: the host channel count
is how many channels the driver supplies, this is how many the plugin was
wired for. `0` when nothing is open — and for a plugin without
`clap.audio-ports`, which declares no audio ports at all.
"""
clap_n_audio_in() = Int(ccall((:clap_host_n_audio_in, CLAP_LIB), Cdouble, ()))

"""
    clap_n_audio_out() -> Int

Total number of audio channels the open plugin declared in its output
direction, summed across its `clap.audio-ports` ports. See
[`clap_n_audio_in`](@ref): this is the same number on the output side.
`0` when nothing is open.
"""
clap_n_audio_out() = Int(ccall((:clap_host_n_audio_out, CLAP_LIB), Cdouble, ()))

"""
    clap_latency() -> Float64

Latency the plugin reports, in samples. **Not compensated by default** —
hosting a lookahead plugin leaves its output shifted by this many samples
relative to the input, and a model that cares must align downstream itself or
open with `compensate_latency = true` (see [`clap_open!`](@ref)).
"""
clap_latency() = ccall((:clap_host_latency, CLAP_LIB), Cdouble, ())

"""
    clap_reset_counters!()

Zero the host's call counters, so [`clap_n_process`](@ref) counts from here.
Does not touch the plugin's own state.
"""
function clap_reset_counters!()
    ccall((:clap_host_reset_counters, CLAP_LIB), Cvoid, ())
    return nothing
end

"""
    clap_params() -> Vector{@NamedTuple{id, name, min, max, default}}

Every parameter the open plugin reports. The `id`s are what a model passes to
`ClapEffect`'s slots, which is why they are numbers: a `clap_id` is a `uint32`
and every `uint32` is exactly representable as a `Float64`, so a model can name
its own parameters with nothing to keep in sync driver-side.
"""
function clap_params()
    n = clap_param_count()
    # Written out rather than looped over a symbol: a ccall's function name and
    # library must be literal, not a local variable.
    return [
        (
            id = ccall((:clap_host_param_id, CLAP_LIB), Cdouble, (Clong,), i),
            name = unsafe_string(
                ccall(
                    (:clap_host_param_name, CLAP_LIB),
                    Cstring, (Clong,), i
                )
            ),
            min = ccall((:clap_host_param_min, CLAP_LIB), Cdouble, (Clong,), i),
            max = ccall((:clap_host_param_max, CLAP_LIB), Cdouble, (Clong,), i),
            default = ccall((:clap_host_param_default, CLAP_LIB), Cdouble, (Clong,), i),
        )
            for i in 0:(n - 1)
    ]
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
    return ccall(
        (:clap_in_fill, CLAP_LIB), Cdouble, (Ptr{Cdouble}, Clong, Clong),
        v, length(v) ÷ channels, channels
    )
end

"""
    clap_out(token; channel = 0) -> Vector{Float64}

The output block named by `token`. Empty when the token is stale — the same
refusal the node-side accessors make, so a test cannot accidentally check
yesterday's audio. (Under latency compensation a stale token is an error
instead — see below.)

When the plugin was opened with `compensate_latency = true` this is instead the
next block of the *aligned* stream: the plugin's reported latency is removed by
discarding its first `clap_latency()` output samples, so output block `k`
corresponds to input block `k`. A shift that is not a whole number of blocks
cannot land in one call, so for `0 < clap_latency()` the first
`ceil(clap_latency() / block_size)` reads return `Float64[]` while the shift is
absorbed, and [`clap_flush!`](@ref) yields the tail at end of stream. Two extra
rules apply under the mode, both enforced loudly: every processed block must be
read once (a skipped block would silently misalign the stream), and a plugin
that changes its latency mid-stream errors on the next read.
"""
function clap_out(token::Real; channel::Integer = 0)
    comp = _COMP[]
    comp === nothing && return _raw_out(token, channel)
    return _comp_out(comp, token, Int(channel))
end

function _raw_out(token, channel)
    n = ccall((:clap_out_count, CLAP_LIB), Cdouble, (Cdouble,), token)
    isnan(n) && return Float64[]
    return [
        ccall(
            (:clap_out_sample, CLAP_LIB), Cdouble, (Cdouble, Cdouble, Cdouble),
            token, i, channel
        ) for i in 0:(Int(n) - 1)
    ]
end

# ---------------------------------------------------------------------------
# Latency compensation (driver-side, opt-in)
#
# A plugin with latency N returns N samples of pre-roll at the head of its
# output: its output block k corresponds to input samples kB..kB+B-1 shifted
# back by N. Aligned output block k therefore needs input samples through
# kB+B-1+N — the future, when block k is the one just fed. So the aligned
# stream lags by ceil(N/B) blocks no matter what; the mode discards the first
# N output samples (the pre-roll; feeding silence first changes only which
# samples are discarded, not the lag), buffers the rest, emits a block once one
# is whole, and clap_flush! feeds the N samples of zeros that collect the tail.
# Compensation belongs to the default instance only; its state is one
# module-level value reset by open/close.
# ---------------------------------------------------------------------------

mutable struct _LatencyComp
    latency::Int             # N, captured at open; verified on every drain
    skip::Int                # raw output samples still to discard (the pre-roll)
    pending::Vector{Vector{Float64}}      # aligned samples not yet emitted, per channel
    emitted::Vector{Int}                  # aligned samples handed out, per channel
    fed::Int                 # input samples processed (per channel), flush zeros excluded
    last_drained::Float64    # newest output token folded into `pending`
    blocks::Vector{Union{Nothing, Vector{Float64}}}  # this token's emitted block, per channel
    tails::Vector{Union{Nothing, Vector{Float64}}}   # flush output served, per channel
    zeros_fed::Bool                  # flush silence already pushed through
end

_LatencyComp(latency::Int, channels::Int) = _LatencyComp(
    latency, latency, [Float64[] for _ in 1:channels], zeros(Int, channels), 0, 0.0,
    Union{Nothing, Vector{Float64}}[nothing for _ in 1:channels],
    Union{Nothing, Vector{Float64}}[nothing for _ in 1:channels], false
)

const _COMP = Ref{Union{Nothing, _LatencyComp}}(nothing)

"""
    clap_compensating() -> Bool

Whether the open plugin was opened with `compensate_latency = true` — i.e.
whether [`clap_out`](@ref) returns the aligned stream rather than the raw
output block.
"""
clap_compensating() = _COMP[] !== nothing

function _drain!(comp::_LatencyComp, token; real_input::Bool = true)
    token == comp.last_drained + 1 || error(
        "latency compensation: output block(s) $(Int(comp.last_drained) + 1):" *
            "$(round(Int, token) - 1) were processed but never read; the " *
            "compensated stream cannot skip a block"
    )
    live = Int(clap_latency())
    live == comp.latency || error(
        "latency compensation: plugin latency changed mid-stream " *
            "($(comp.latency) -> $live samples); reopen to compensate the new latency"
    )
    # Every channel discards the same N-sample pre-roll: drop once per drain,
    # not once per channel.
    drop = min(comp.skip, clap_block_size())
    comp.skip -= drop
    for c in eachindex(comp.pending)
        raw = _raw_out(token, c - 1)
        append!(comp.pending[c], raw[(drop + 1):end])
    end
    real_input && (comp.fed += clap_block_size())
    comp.last_drained = token
    fill!(comp.blocks, nothing)
    return nothing
end

function _comp_out(comp::_LatencyComp, token::Real, channel::Int)
    clap_is_open() || error("clap_out: no plugin is open")
    isnan(token) && error("clap_out: the process call failed (token is NaN)")
    if clp_out_valid(token) != 1.0
        error(
            "clap_out: token $token is not the current output block; under latency " *
                "compensation every processed block must be read once, in order"
        )
    end
    token == comp.last_drained || _drain!(comp, token)
    c = channel + 1
    1 <= c <= length(comp.pending) ||
        error("clap_out: channel $channel out of range (the plugin was opened with $(length(comp.pending)))")
    out = comp.blocks[c]
    if out === nothing
        out = length(comp.pending[c]) >= clap_block_size() ?
            splice!(comp.pending[c], 1:clap_block_size()) : Float64[]
        comp.emitted[c] += length(out)
        comp.blocks[c] = out
    end
    return out
end

"""
    clap_flush!(; channel = 0) -> Vector{Float64}

The tail of the aligned stream under latency compensation: feeds the plugin
`ceil(clap_latency() / block_size)` blocks of zeros — held parameter values
persist, so no parameter events are sent — and returns the aligned samples for
`channel` that [`clap_out`](@ref) has not yielded yet: `ceil(latency /
block_size) * block_size` of them after a run of full input blocks, so the
aligned samples from `clap_out` plus this tail total exactly the number fed in.

Errors when the plugin was not opened with `compensate_latency = true`. The
silence is pushed through once; each channel's tail is computed on that
channel's first call, and a repeat call returns it again — the same
read-is-idempotent convention as [`clap_out`](@ref).
"""
function clap_flush!(; channel::Integer = 0)
    comp = _COMP[]
    comp === nothing && error(
        "clap_flush!: the open plugin was not opened with `compensate_latency = true`"
    )
    c = Int(channel) + 1
    1 <= c <= length(comp.pending) ||
        error("clap_flush!: channel $channel out of range (the plugin was opened with $(length(comp.pending)))")
    if !comp.zeros_fed
        comp.zeros_fed = true
        block = clap_block_size()
        for _ in 1:cld(comp.latency, block)
            tok = clap_fill!(zeros(block))
            isnan(tok) && error("clap_flush!: $(clap_last_error())")
            out = clp_process(tok, -1.0, 0.0, -1.0, 0.0, -1.0, 0.0, -1.0, 0.0)
            isnan(out) && error("clap_flush!: $(clap_last_error())")
            _drain!(comp, out; real_input = false)
        end
    end
    tail = comp.tails[c]
    if tail === nothing
        owed = comp.fed - comp.emitted[c]
        tail = splice!(comp.pending[c], 1:min(owed, length(comp.pending[c])))
        comp.emitted[c] += length(tail)
        comp.tails[c] = tail
    end
    return tail
end

# ---------------------------------------------------------------------------
# Node-side operators. One equation, one call. Each takes the token it
# depends on, so nothing can be scheduled before the block it reads.
# ---------------------------------------------------------------------------

"""
    AudioPlugins.clp_in_tone(t, waveform, freq, amp) -> token

Generate one block of a test waveform ending at source time `t` seconds, fill
the input block with it on every channel, and return its token. `waveform` is a
`CLAP_WAVE_*` code (see [`CLAP_WAVE_SINE`](@ref)), `freq` is in Hz and `amp` in
`[0, 1]`. The alternative to [`clap_fill!`](@ref) when the source should live
node-side: every argument is a number, so a model can be exercised with no
driver-side setup and a test can state its expected output in closed form.

Returns `NaN` when no plugin is open.
"""
clp_in_tone(t, waveform, freq, amp) =
    ccall(
    (:clap_in_tone, CLAP_LIB), Cdouble, (Cdouble, Cdouble, Cdouble, Cdouble),
    t, waveform, freq, amp
)

"""
    AudioPlugins.clp_process(dep, id0, v0, id1, v1, id2, v2, id3, v3) -> token

Run the plugin over the input block named by `dep` and return the output
block's token, which [`clap_out`](@ref) and the `clp_out_*` readers then take.
This is the whole of the processing path: one equation, one call.

Up to four parameters are driven per block by the four `(id, value)` slots,
pushed into the plugin's input event list as `CLAP_EVENT_PARAM_VALUE` — the
mechanism CLAP defines, rather than poking the controller behind the
processor's back. A negative id means the slot is unused, and a value is only
sent when it differs from the last one sent for that id, so a held-constant
parameter costs one event on the first block and none afterwards. The ids are
the `id` field of [`clap_params`](@ref).

Returns `NaN` when nothing is open, or when `dep` does not name the *current*
input block: a stale token is refused rather than answered from whatever the
buffer still holds.
"""
clp_process(dep, id0, v0, id1, v1, id2, v2, id3, v3) =
    ccall(
    (:clap_process, CLAP_LIB), Cdouble,
    (
        Cdouble,                                      # dep
        Cdouble, Cdouble, Cdouble, Cdouble,           # slots 0, 1
        Cdouble, Cdouble, Cdouble, Cdouble,
    ),          # slots 2, 3
    dep, id0, v0, id1, v1, id2, v2, id3, v3
)

"""
    AudioPlugins.clp_set(dep, id, value) -> token

Queue parameter `id` of the open plugin at `value` for the block named by
`dep`, and return `dep`, so that driving parameters is a *chain*: one equation
per parameter, each taking the previous one's result, with the last one's
result passed to [`clp_process`](@ref AudioPlugins.clp_process).

This is what lifts the four-slot limit of `clp_process`. A plugin with thirteen
parameters is thirteen equations, not four, and it is a chain rather than
thirteen independent calls because a synchronous program orders by data
dependency and by nothing else — an unchained call could be scheduled after the
`process` it was meant to precede.

The change-detection rule is `clp_process`'s, keyed by id: a value equal to the
last one sent for that id queues no event, so a held parameter costs one event
on the first block and none afterwards.

Returns `NaN` when nothing is open, when `dep` does not name the current input
block, when `id` is not a parameter the open plugin declares, when `value` is
`NaN`, or when the queue is full. Filling a new input block abandons a pending
chain, so a refused chain cannot leak into the next block.
"""
clp_set(dep, id, value) =
    ccall(
    (:clap_set_param, CLAP_LIB), Cdouble, (Cdouble, Cdouble, Cdouble),
    dep, id, value
)

"""
    AudioPlugins.clp_expect(dep, index) -> token

Return `dep` when the open plugin is the one at `index` in its bundle — the
`index` field of [`plugins`](@ref) — and `NaN` otherwise.

The guard a generated per-plugin component puts in front of its parameter
chain. The default API holds one plugin at a time and the driver opens it, so
without this a model built for one effect processes through whichever effect
happens to be open and returns numbers that look fine. `NaN` in, `NaN` out, so
it composes with the rest of the refusal path.
"""
clp_expect(dep, index) =
    ccall((:clap_expect, CLAP_LIB), Cdouble, (Cdouble, Cdouble), dep, index)

"""
    AudioPlugins.clp_out_rms(dep) -> Float64

Root-mean-square of the output block named by `dep`, over every channel.
`NaN` when `dep` is not the current output token.
"""
clp_out_rms(dep) = ccall((:clap_out_rms, CLAP_LIB), Cdouble, (Cdouble,), dep)

"""
    AudioPlugins.clp_out_peak(dep) -> Float64

Largest absolute sample in the output block named by `dep`, over every channel.
`NaN` when `dep` is not the current output token.
"""
clp_out_peak(dep) = ccall((:clap_out_peak, CLAP_LIB), Cdouble, (Cdouble,), dep)

"""
    AudioPlugins.clp_out_valid(dep) -> Float64

`1.0` when `dep` names the current output block and `0.0` when it does not, for
a model that wants to branch on freshness instead of propagating a `NaN`.
"""
clp_out_valid(dep) = ccall((:clap_out_valid, CLAP_LIB), Cdouble, (Cdouble,), dep)

"""
    AudioPlugins.clp_in_sample(dep, i, ch) -> Float64

Sample `i` of channel `ch` of the input block named by `dep`, zero-based in
both. `NaN` when `dep` is not the current input token.
"""
clp_in_sample(dep, i, ch) =
    ccall((:clap_in_sample, CLAP_LIB), Cdouble, (Cdouble, Cdouble, Cdouble), dep, i, ch)

"""
    AudioPlugins.clp_out_sample(dep, i, ch) -> Float64

Sample `i` of channel `ch` of the output block named by `dep`, zero-based in
both. `NaN` when `dep` is not the current output token — which is what makes a
stale read a visible error rather than a plausible-looking wrong answer.
[`clap_out`](@ref) is the vector-at-a-time form.
"""
clp_out_sample(dep, i, ch) =
    ccall((:clap_out_sample, CLAP_LIB), Cdouble, (Cdouble, Cdouble, Cdouble), dep, i, ch)
