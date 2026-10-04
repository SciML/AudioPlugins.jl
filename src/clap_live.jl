using Libdl: dlopen, dlclose, dlsym, RTLD_NOW, RTLD_LOCAL

export ClapLiveSession, LiveEvent, LiveStats, live_available, open_live
export start!, stop!, poll!, try_write!, try_read!, live_stats

struct _LiveConfig
    sample_rate::Float64
    block_size::UInt32
    channels::UInt32
    queue_blocks::UInt32
    driver::UInt32
    priority::UInt32
end

"""
    LiveEvent(; frame = 0, param_id = 0, value)
    LiveEvent(midi::NTuple{3, UInt8}; frame = 0)

A sample-offset parameter change (plain units) or three-byte MIDI 1.0 message
for [`try_write!`](@ref). Events must be ordered by `frame`, starting at zero,
within the accompanying block. At most 64 events fit in one block.
"""
struct LiveEvent
    type::UInt32
    frame::UInt32
    param_id::UInt32
    midi::NTuple{4, UInt8}
    value::Float64
end
LiveEvent(; frame::Integer = 0, param_id::Integer = 0, value::Real) =
    LiveEvent(0, frame, param_id, (0x00, 0x00, 0x00, 0x00), value)
LiveEvent(midi::NTuple{3, UInt8}; frame::Integer = 0) =
    LiveEvent(1, frame, 0, (midi..., 0x00), 0.0)

"""
    LiveStats

Native live-session counters returned by [`live_stats`](@ref). `state` is 0
(prepared), 1 (running), or 2 (audio thread finished). `priority_granted` is
one only when the requested priority was granted. `error` is a native status
code (zero on success). Counters wrap modulo 2^32 and are independently read,
so a snapshot is not a transactionally consistent view of a block boundary.
"""
struct LiveStats
    state::UInt32
    priority_granted::UInt32
    blocks::UInt32
    underruns::UInt32
    overruns::UInt32
    deadline_misses::UInt32
    process_errors::UInt32
    output_events_dropped::UInt32
    restart_requested::UInt32
    callback_requested::UInt32
    error::Int32
end

const _LIVE_SYMBOLS = (
    :ap_live_open, :ap_live_start, :ap_live_stop, :ap_live_close, :ap_live_poll,
    :ap_live_try_write, :ap_live_try_read, :ap_live_get_stats,
)
const _LiveFunctions = NamedTuple{_LIVE_SYMBOLS, NTuple{8, Ptr{Cvoid}}}

"""
    ClapLiveSession

An experimental Linux native CLAP session, created by [`open_live`](@ref).
Owns its plugin and library independently of the offline host. Control and
queue calls must stay on Julia thread 1. Stop and close explicitly, preferably
with the `open_live` do-block: there is deliberately no finalizer that might
unload a running plugin or invoke its lifecycle from the wrong thread.
"""
mutable struct ClapLiveSession
    handle::Ptr{Cvoid}
    library::Ptr{Cvoid}
    functions::_LiveFunctions
    samples::Int
    sequence::UInt64
end

"""
    live_available(; library = clap_lib_path())

Whether `library` supplies experimental live ABI version 1 on Linux. Returns
false with the currently released JLL. No new JLL version is required to load
AudioPlugins; pass a source-built `clap_live.c` library to opt in.
"""
function live_available(; library::AbstractString = CLAP_LIB)
    Sys.islinux() || return false
    lib = dlopen(library; throw_error = false)
    lib === nothing && return false
    try
        abi = dlsym(lib, :ap_live_abi_version; throw_error = false)
        abi === nothing && return false
        ccall(abi, UInt32, ()) == 1 || return false
        return all(name -> dlsym(lib, name; throw_error = false) !== nothing, _LIVE_SYMBOLS)
    finally
        dlclose(lib)
    end
end

function _live_result(result::Integer)
    result == 0 && return nothing
    message = if result == -1
        "invalid configuration, buffer, or event"
    elseif result == -2
        "call must run on the session's original OS control thread"
    elseif result == -3
        "invalid session state; stop before close, and reopen after a restart request"
    elseif result == -4
        "plugin load, initialization, activation, or processing failed"
    elseif result == -5
        "requested realtime priority was denied"
    elseif result == -6
        "unsupported plugin layout, event, CLAP version, or platform"
    else
        "native status $result"
    end
    error("CLAP live: $message")
end

function _live_control()
    Threads.threadid() == 1 || error("CLAP live control calls must run on Julia thread 1")
    return nothing
end
function _live_check(session::ClapLiveSession)
    _live_control()
    isopen(session) || error("CLAP live session is closed")
    return nothing
end

"""
    open_live([f::Function,] path; plugin_id, library = clap_lib_path(),
              sample_rate = 48000, block_size = 512, channels = 2,
              queue_blocks = 8, priority = :normal)

Prepare an experimental Linux CLAP session with a native monotonic-clock
processing thread. Call [`start!`](@ref) after optionally queuing input. This
driver exercises live scheduling without hardware; it does not open an audio
device. Native device integration uses `csrc/clap_live.h` instead.

`path` is a CLAP binary (or registered plugin id). The first adapter accepts
one matching mono/stereo input/output bus and CLAP ≥ 1.2. `queue_blocks` must
be a power of two from 2 to 1024. `priority` is `:normal`, `:best_effort`, or
`:strict`; the latter fails startup if Linux FIFO priority is denied.

The currently released JLL lacks this ABI: pass an explicit source-built
`library`. Calls must stay on Julia thread 1. The do-block always stops and
closes; otherwise use `stop!` and `close` in `finally`. Service [`poll!`](@ref)
regularly for plugin main-thread callbacks. No finite queue or priority setting
guarantees deadlines for arbitrary plugin code. Use plugins whose processing
is realtime-safe; Julia-authored plugins with a Julia runtime are unsuitable.
"""
function open_live(
        path::AbstractString; plugin_id::AbstractString = "",
        library::AbstractString = CLAP_LIB, sample_rate::Real = 48000,
        block_size::Integer = 512, channels::Integer = 2,
        queue_blocks::Integer = 8, priority::Symbol = :normal
    )
    _live_control()
    Sys.islinux() || error("Experimental CLAP live sessions currently support Linux only")
    level = findfirst(==(priority), (:normal, :best_effort, :strict))
    level === nothing && throw(ArgumentError("priority must be :normal, :best_effort, or :strict"))
    config = _LiveConfig(sample_rate, block_size, channels, queue_blocks, 0, level - 1)
    bundle, id = _resolve_plugin(path, plugin_id)
    live_available(; library) || error("Live ABI v1 unavailable; build csrc/clap_live.c and pass library")
    lib = dlopen(library, RTLD_NOW | RTLD_LOCAL)
    handle = Ref{Ptr{Cvoid}}(C_NULL)
    try
        functions = _LiveFunctions(map(name -> dlsym(lib, name), _LIVE_SYMBOLS))
        _live_result(
            ccall(
                functions.ap_live_open, Cint,
                (Cstring, Cstring, Ref{_LiveConfig}, Ref{Ptr{Cvoid}}), bundle, id, config, handle
            )
        )
        return ClapLiveSession(handle[], lib, functions, Int(block_size * channels), 0)
    catch
        dlclose(lib)
        rethrow()
    end
end

function open_live(f::Function, path::AbstractString; kwargs...)
    session = open_live(path; kwargs...)
    try
        return f(session)
    finally
        if isopen(session)
            stop!(session)
            close(session)
        end
    end
end

Base.isopen(session::ClapLiveSession) = session.handle != C_NULL

"""
    start!(session::ClapLiveSession)

Activate and start native processing. Waits for the native startup result on
the control thread. The audio thread never calls Julia. Missing input becomes
silence with held parameters; full output queues discard the newest output.
"""
function start!(session::ClapLiveSession)
    _live_check(session)
    _live_result(ccall(session.functions.ap_live_start, Cint, (Ptr{Cvoid},), session.handle))
    return session
end

"""
    stop!(session::ClapLiveSession)

Join the native thread and deactivate on the control thread. Discards queued
audio, preserves plugin parameters, and permits starting again unless a plugin
restart request requires reopening. This can wait for plugin code to return.
"""
function stop!(session::ClapLiveSession)
    _live_check(session)
    _live_result(ccall(session.functions.ap_live_stop, Cint, (Ptr{Cvoid},), session.handle))
    return session
end

function Base.close(session::ClapLiveSession)
    _live_control()
    isopen(session) || return nothing
    _live_result(ccall(session.functions.ap_live_close, Cint, (Ptr{Cvoid},), session.handle))
    session.handle = C_NULL
    dlclose(session.library)
    session.library = C_NULL
    return nothing
end

"""
    poll!(session::ClapLiveSession) -> Bool

Dispatch one coalesced main-thread callback. Returns false after servicing a
restart request by stopping and deactivating; close and reopen to renegotiate
ports and parameters. Does not silently resume with stale configuration.
"""
function poll!(session::ClapLiveSession)
    _live_check(session)
    result = ccall(session.functions.ap_live_poll, Cint, (Ptr{Cvoid},), session.handle)
    result == 2 && return false
    _live_result(result)
    return true
end

"""
    try_write!(session, samples::Vector{Float32}; events = LiveEvent[], sequence = …) -> Bool

Copy one interleaved block and its timestamped events into native storage.
Returns false on a full queue without consuming input. `sequence` defaults to
an increasing tag and must be less than `typemax(UInt64)`. No Julia pointer is
retained. Calls belong on the control thread; Julia-side allocation is allowed.
"""
function try_write!(
        session::ClapLiveSession, samples::Vector{Float32};
        events::Vector{LiveEvent} = LiveEvent[], sequence::Integer = session.sequence
    )
    _live_check(session)
    length(samples) == session.samples || throw(DimensionMismatch("expected $(session.samples) samples"))
    0 <= sequence < typemax(UInt64) || throw(ArgumentError("sequence must fit UInt64 and not be typemax"))
    length(events) <= 64 || throw(ArgumentError("at most 64 events per block"))
    result = ccall(
        session.functions.ap_live_try_write, Cint,
        (Ptr{Cvoid}, Ptr{Cfloat}, Ptr{LiveEvent}, UInt32, UInt64),
        session.handle, samples, events, length(events), sequence
    )
    result == 1 && return false
    _live_result(result)
    session.sequence = UInt64(sequence) + 1
    return true
end

"""
    try_read!(session, samples::Vector{Float32}) -> metadata or nothing

Copy the oldest native output into one interleaved block. Returns `nothing`
when empty, otherwise `(tick, input_sequence)`. Gaps in `tick` expose dropped
output; `input_sequence === nothing` denotes a silence-input underrun. The
plugin may still produce a tail or synthesized output during an underrun.
"""
function try_read!(session::ClapLiveSession, samples::Vector{Float32})
    _live_check(session)
    length(samples) == session.samples || throw(DimensionMismatch("expected $(session.samples) samples"))
    tick, sequence = Ref{UInt64}(), Ref{UInt64}()
    result = ccall(
        session.functions.ap_live_try_read, Cint,
        (Ptr{Cvoid}, Ptr{Cfloat}, Ref{UInt64}, Ref{UInt64}), session.handle, samples, tick, sequence
    )
    result == 1 && return nothing
    _live_result(result)
    return (; tick = tick[], input_sequence = sequence[] == typemax(UInt64) ? nothing : sequence[])
end

"""
    live_stats(session::ClapLiveSession) -> LiveStats

Read native progress, underrun/overrun, deadline, callback and error counters.
No plugin method is invoked. A deadline miss reports slow processing or a
timer wakeup late by more than one block period; it is not a device xrun count.
"""
function live_stats(session::ClapLiveSession)
    _live_check(session)
    stats = Ref{LiveStats}()
    _live_result(
        ccall(
            session.functions.ap_live_get_stats, Cint, (Ptr{Cvoid}, Ref{LiveStats}), session.handle, stats
        )
    )
    return stats[]
end
