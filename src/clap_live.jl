using Libdl: dlopen, dlclose, dlsym, RTLD_NOW, RTLD_LOCAL

export LiveSession, ClapLiveSession, LiveEvent, LiveStats, live_available, open_live
export audio_devices, device_stats, DeviceStats
export start!, stop!, poll!, try_write!, try_read!, live_stats, live_latency

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

An experimental native audio-plugin session, created by [`open_live`](@ref).
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
    live_available(; library = clap_lib_path(), format = :clap)

Whether `library` supplies experimental live ABI version 1 for `format`. Returns
false with the currently released JLL. No new JLL version is required to load
AudioPlugins; pass a source-built native live library to opt in.
"""
function live_available(; format::Symbol = :clap, library::AbstractString = _live_library(format))
    format in (:clap, :lv2, :vst3) || return false
    lib = dlopen(library; throw_error = false)
    lib === nothing && return false
    try
        abi = dlsym(lib, :ap_live_abi_version; throw_error = false)
        abi === nothing && return false
        ccall(abi, UInt32, ()) == 1 || return false
        dlsym(lib, _live_open_symbol(format); throw_error = false) === nothing && return false
        return all(name -> dlsym(lib, name; throw_error = false) !== nothing, _LIVE_SYMBOLS)
    finally
        dlclose(lib)
    end
end

"Generic name for a native live session; `ClapLiveSession` remains a compatibility alias."
const LiveSession = ClapLiveSession
_live_library(format) = format == :lv2 ? LV2_LIB : format == :vst3 ? VST3_LIB : CLAP_LIB
_live_open_symbol(format) = format == :clap ? :ap_live_open : Symbol("ap_live_open_", format)

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
    error("AudioPlugins live: $message")
end

function _live_control()
    Threads.threadid() == 1 || error("AudioPlugins live control calls must run on Julia thread 1")
    return nothing
end
function _live_check(session::ClapLiveSession)
    _live_control()
    isopen(session) || error("AudioPlugins live session is closed")
    return nothing
end

"""
    open_live([f::Function,] path; plugin_id, library = clap_lib_path(),
              sample_rate = 48000, block_size = 512, channels = 2,
              queue_blocks = 8, priority = :normal, format = :clap,
              driver = :timer, backend = :default, capture = false)

Prepare an isolated native session for `format = :clap`, `:lv2`, or `:vst3`.
Call [`start!`](@ref) after optionally queuing input. `driver = :timer` uses a
native monotonic clock; `driver = :device` follows native playback/capture.
`backend` selects `:default`, `:alsa`, `:pulse`, `:jack`, `:coreaudio`, `:wasapi`,
or the explicit hardware-free `:null` backend. `capture = true` replaces queued
audio with device input while still accepting queued parameter/MIDI events.
Device indices default to -1; enumerate with [`audio_devices`](@ref).

`path` is a CLAP binary/bundle (or registered id), LV2 search path (use
[`lv2_default_path`](@ref)), or VST3 bundle. `plugin_id` identifies the plugin
(CLAP id, LV2 URI, or VST3 class id). One matching mono/stereo input/output bus
is supported. Unsupported required features and layouts fail during opening.
Parameters use plain units for CLAP/LV2 and normalized 0–1 values for VST3.
LV2 scalar parameters require frame zero. VST3 MIDI currently accepts note
on/off; other MIDI messages are rejected. CLAP requires version ≥ 1.2.

`queue_blocks` must be a power of two from 2 to 1024. `priority` is `:normal`,
`:best_effort`, or `:strict`; strict fails if native priority is unavailable.
Device output has a fixed host delay sized at opening for the negotiated device
buffer. Its transport queues grow independently of `queue_blocks`, up to 1024
blocks; unsupported buffer sizes fail to open. Missing or late output is silence
and counted separately in [`device_stats`](@ref).

The currently released JLL lacks this ABI: pass an explicit source-built
`library`. Calls must stay on Julia thread 1. The do-block always stops and
closes; otherwise use `stop!` and `close` in `finally`. Service [`poll!`](@ref)
regularly for plugin main-thread callbacks. No finite queue or priority setting
guarantees deadlines for arbitrary plugin code. Use plugins whose processing
is realtime-safe; Julia-authored plugins with a Julia runtime are unsuitable.
"""
function open_live(
        path::AbstractString; plugin_id::AbstractString = "",
        format::Symbol = :clap, library::AbstractString = _live_library(format), sample_rate::Real = 48000,
        block_size::Integer = 512, channels::Integer = 2,
        queue_blocks::Integer = 8, priority::Symbol = :normal,
        driver::Symbol = :timer, backend::Symbol = :default,
        capture::Bool = false, playback_device::Integer = -1, capture_device::Integer = -1
    )
    _live_control()
    format in (:clap, :lv2, :vst3) || throw(ArgumentError("format must be :clap, :lv2, or :vst3"))
    driver in (:timer, :device) || throw(ArgumentError("driver must be :timer or :device"))
    driver == :timer && capture && throw(ArgumentError("capture requires driver = :device"))
    level = findfirst(==(priority), (:normal, :best_effort, :strict))
    level === nothing && throw(ArgumentError("priority must be :normal, :best_effort, or :strict"))
    config = _LiveConfig(sample_rate, block_size, channels, queue_blocks, driver == :device ? 2 : 0, level - 1)
    bundle, id = format == :clap ? _resolve_plugin(path, plugin_id) : (String(path), String(plugin_id))
    live_available(; library, format) || error("Live ABI v1 for $format unavailable; build the native live host and pass library")
    lib = dlopen(library, RTLD_NOW | RTLD_LOCAL)
    handle = Ref{Ptr{Cvoid}}(C_NULL)
    try
        functions = _LiveFunctions(map(name -> dlsym(lib, name), _LIVE_SYMBOLS))
        _live_result(
            ccall(
                dlsym(lib, _live_open_symbol(format)), Cint,
                (Cstring, Cstring, Ref{_LiveConfig}, Ref{Ptr{Cvoid}}), bundle, id, config, handle
            )
        )
        if driver == :device
            configure = dlsym(lib, :ap_live_configure_device; throw_error = false)
            configure === nothing && error("Native device support unavailable; rebuild with AP_LIVE_WITH_DEVICE")
            _live_result(
                ccall(
                    configure, Cint, (Ptr{Cvoid}, Cstring, Cint, Cint, Cint),
                    handle[], String(backend), capture, playback_device, capture_device
                )
            )
        end
        return ClapLiveSession(handle[], lib, functions, Int(block_size * channels), 0)
    catch
        if handle[] != C_NULL
            ccall(dlsym(lib, :ap_live_close), Cint, (Ptr{Cvoid},), handle[])
        end
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

"""
    live_latency(session::ClapLiveSession) -> UInt32

Plugin latency in frames, cached on the control thread after each successful
activation (CLAP/VST3). Call after [`start!`](@ref) and before [`stop!`](@ref). Returns zero
when the plugin reports no latency. LV2 latency outputs are atomically
refreshed after processing each block. This query invokes no plugin
method while audio runs. Queueing and device latency are excluded, and no
latency compensation is performed.

Requires the optional `ap_live_get_latency` symbol in the source-built library.
Older live ABI v1 libraries remain usable but do not support this query. After
a plugin restart request, close and reopen to renegotiate the configuration.
"""
function live_latency(session::ClapLiveSession)
    _live_check(session)
    query = dlsym(session.library, :ap_live_get_latency; throw_error = false)
    query === nothing && error("AudioPlugins live latency unavailable; rebuild csrc/clap_live.c")
    frames = Ref{UInt32}()
    _live_result(ccall(query, Cint, (Ptr{Cvoid}, Ref{UInt32}), session.handle, frames))
    return frames[]
end

"""
    DeviceStats

Native device counters and buffering information. `buffering_frames` is the
fixed host pipeline delay at the session sample rate. `device_period_frames`
uses `device_sample_rate`; it is a negotiated period, not a measured DAC delay.
`lost != 0` means the device stopped, was interrupted, or changed routing; call
`poll!`, then close and reopen. Counter snapshots are independent 32-bit reads.
"""
struct DeviceStats
    underruns::UInt32
    overruns::UInt32
    lost::UInt32
    buffering_frames::UInt32
    device_period_frames::UInt32
    device_sample_rate::UInt32
    capture::UInt32
    backend::UInt32
    workgroup_joined::UInt32
end

"""
    device_stats(session) -> DeviceStats

Read device progress and buffering metadata without invoking the plugin.
Requires a session opened with `driver = :device`.
"""
function device_stats(session::ClapLiveSession)
    _live_check(session)
    query = dlsym(session.library, :ap_live_get_device_stats)
    stats = Ref{DeviceStats}()
    _live_result(ccall(query, Cint, (Ptr{Cvoid}, Ref{DeviceStats}), session.handle, stats))
    return stats[]
end

"""
    audio_devices(; library, backend = :default, capture = false)

Enumerate native playback or capture devices as `(index, name)` entries.
Indices are zero-based; `-1` in `open_live` selects the system default.
Enumeration is a control-thread operation. Re-enumerate after device changes.
Backend `:null` is an explicit hardware-free test driver, never a fallback.
"""
function audio_devices(; library::AbstractString = CLAP_LIB, backend::Symbol = :default, capture::Bool = false)
    _live_control()
    lib = dlopen(library, RTLD_NOW | RTLD_LOCAL)
    try
        enumerate = dlsym(lib, :ap_live_audio_devices)
        count = ccall(enumerate, Cint, (Cstring, Cint, Cint, Ptr{UInt8}, UInt32), String(backend), capture, -1, C_NULL, 0)
        count < 0 && _live_result(count)
        buffer = zeros(UInt8, 1024)
        return map(0:(count - 1)) do index
            result = ccall(
                enumerate, Cint, (Cstring, Cint, Cint, Ptr{UInt8}, UInt32),
                String(backend), capture, index, buffer, length(buffer)
            )
            result < 0 && _live_result(result)
            (; index, name = GC.@preserve buffer unsafe_string(pointer(buffer)))
        end
    finally
        dlclose(lib)
    end
end
