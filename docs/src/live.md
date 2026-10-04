# Experimental live hosting

The native live engine implements the direction approved in
[issue #8](https://github.com/SciML/AudioPlugins.jl/issues/8#issuecomment-5956165702).
It has isolated CLAP, LV2, and VST3 adapters, a timer driver, and native device
playback/capture for Linux, macOS, and Windows. This is a source-built development
API. The released JLLs still lack it; loading AudioPlugins with them remains
supported. Binary publication and physical-device validation are release gates.

## Build

The CMake build uses vendored CLAP/LV2 headers and miniaudio 0.11.23 (MIT-0;
see `csrc/vendor/PROVENANCE.md`). LV2 additionally links lilv. VST3 requires a
built MIT-licensed SDK 3.8 or later. Specify its include tree and library folder:

```sh
cmake -S csrc -B build/live -DCMAKE_BUILD_TYPE=Release \
  -DVST3_SDK_ROOT=/path/to/vst3sdk/include/vst3sdk \
  -DVST3_SDK_LIBDIR=/path/to/vst3sdk/lib/vst3sdk \
  -DLILV_INCLUDE_DIR=/path/to/lilv/include/lilv-0 \
  -DLILV_LIBRARY=/path/to/lilv/lib/liblilv-0.so
cmake --build build/live --parallel 2
```

Use the platform's lilv library filename. The output is `libaudioplugins_live.so`
on Linux, `libaudioplugins_live.dylib` on macOS, or `audioplugins_live.dll`
(possibly prefixed with `lib`) on Windows. Windows uses a C11-capable MinGW
compiler. The SDK and lilv paths can come from `vst3sdk_jll` and `Lilv_jll`;
`test/build_live.jl` demonstrates this without replacing installed libraries.

Optional `-DAP_LIVE_LV2=OFF`, `-DAP_LIVE_VST3=OFF`, and
`-DAP_LIVE_DEVICE=OFF` omit those capabilities. A minimal Linux CLAP timer
library can still be built directly:

```sh
cc -std=c11 -O2 -fPIC -shared -o /tmp/libclap_live.so \
   csrc/clap_live.c -pthread -ldl -lm
```

## Julia usage

```julia
using AudioPlugins

library = "/absolute/path/to/libaudioplugins_live.so"
bundle = clap_test_bundle()
open_live(bundle; plugin_id = "ap.gain", library,
          channels = 2, block_size = 64) do session
    input = ones(Float32, 128) # interleaved stereo
    output = similar(input)
    try_write!(session, input; events = [LiveEvent(; value = 0.5)])
    start!(session)
    sleep(0.05)                # native processing continues through Julia stalls
    metadata = try_read!(session, output)
    poll!(session)             # service plugin control work regularly
    @show metadata live_stats(session) live_latency(session)
end                           # stop, join, deactivate, destroy, unload
```

Select `format = :lv2` with an LV2 URI as `plugin_id` and an
`lv2_default_path(...)` search path. Select `format = :vst3` with a VST3 bundle
and class id. `live_available(; library, format)` checks the optional ABI.
`LiveSession` and the original `ClapLiveSession` name refer to the same type.

For playback, add `driver = :device`. For input from a microphone or other
capture device, also add `capture = true`. Queued audio is then replaced by
captured audio; queued parameter/MIDI events remain effective. Device input does
not need a Julia producer to keep running. Enumerate playback or capture with
`audio_devices(; library, capture = false)`, and pass a zero-based
`playback_device` or `capture_device` index; `-1` uses the system default.
Re-enumerate after devices change. Device enumeration and opening are separate,
so indices should be used promptly rather than stored as permanent identities.

The default selects native APIs: PulseAudio/ALSA/JACK on Linux, CoreAudio on
macOS, or WASAPI on Windows. Explicit `backend` choices are `:pulse`, `:alsa`,
`:jack`, `:coreaudio`, and `:wasapi`. `:null` is only an explicit hardware-free
test backend; there is no silent fallback to a fake device. Device opening
fails if the backend, device, channel layout, or configuration is unavailable.
Use a single duplex device when capture and playback must share a physical
clock; automatic drift correction across unrelated devices is not supplied.

## Thread and lifecycle contract

All Julia control and queue calls belong on Julia thread 1, the OS thread that
opened the session. Native calls enforce creator-thread identity. macOS also
requires the application main thread. Do not use migrating Julia tasks. Use the
do-block or explicit `stop!` and `close` in `finally`; there is no unsafe
finalizer. A plugin or device driver that never returns can prevent shutdown.

Open, initialization, activation, deactivation and destruction run on control.
CLAP main-thread requests are coalesced atomically and dispatched by `poll!`.
VST3 component/controller creation and setup stay on control; `setProcessing`
and `process` run on the dedicated native worker. Windows initializes COM on
control and processing threads and pumps pending Windows messages in `poll!`.
macOS pumps the main CFRunLoop there. LV2 has a private lilv world, instance,
port storage and bounded allocation-free URID mapper per session.

The dedicated worker has fixed identity. No Julia callback or Julia-owned
buffer enters its processing path. Sessions do not call into offline host
registries, token counters, or error storage. Serialize module lifecycle work,
including offline calls, on the same control thread: arbitrary plugin-global
thread safety is outside the host's control.

Restart requests, process failures, and device loss stop the worker. Call
`poll!` to join and deactivate; it returns false when the session has stopped.
After a restart request or device loss, close and reopen to renegotiate ports,
parameters, and routing. No stale configuration is silently resumed.

## Device clock and bounded queues

The timer is a development driver, not a hardware clock. Device mode instead
reblocks native device callbacks to the configured processing block size. The
callback copies capture/clock information into a bounded ring and signals the
worker; it never invokes a plugin or waits for Julia or processing. Capture and
playback are converted by miniaudio to the configured interleaved Float32 format.

The worker's output is due exactly two processing blocks after capture. Missing
or late output becomes silence; stale output is discarded, keeping host
pipeline latency fixed instead of accumulating delay after a stall. Shutdown
quiesces device callbacks, wakes and joins the worker, then deactivates on
control. Device interruption, unexpected stop, or routing change marks the
device lost and wakes the worker. `device_stats` exposes loss, transport
underruns/overruns, buffering and negotiated device period/rate. A negotiated
period is not a measurement of DAC or capture latency.

Native integrators can alternatively use the original `AP_LIVE_DEVICE` ABI in
`clap_live.h`: `ap_live_device_begin/process/end` must all run on one stable
native thread. Never implement these callbacks in Julia. The integrator owns
callback quiescence; control-side stop returns `AP_LIVE_AGAIN` while that owner
is active. The built-in miniaudio driver uses `AP_LIVE_HARDWARE` instead.

Julia exchanges complete blocks and at most 64 sorted events through SPSC rings
with acquire/release publication. Input tags associate outputs with inputs;
output ticks expose dropped blocks. With capture, tags instead identify the
captured block's device tick. Missing queued input processes silence, holds
parameters and counts an underrun. A full observation ring discards the newest
observation and counts it, without interrupting hardware playback.

One matching mono/stereo input/output bus is supported. Sidechains, extra buses,
and unsupported required features are rejected. Parameters use plain units for
CLAP/LV2 and normalized 0–1 values for VST3. CLAP and VST3 parameter offsets are
sample-based; LV2 scalar controls require frame zero. Optional CLAP/LV2 MIDI 1.0
input is supported. VST3 currently accepts note-on/off messages and rejects
other MIDI. Events accompany a queued block; they are not absolute device
wall-clock timestamps. Plugin output events are dropped and counted.

`live_latency` reports plugin latency in frames after start. CLAP/VST3 cache it
after activation; LV2 publishes its latency control output atomically after
each processing block. No plugin query runs concurrently on control. This value
excludes input queue depth, the two-block hardware pipeline, and device latency;
no latency compensation is performed.

All host processing storage is allocated and touched before starting. Processing
does not allocate, log, take mutexes, or wait on Julia. The device worker waits
on a native semaphore **between** blocks. Only lock-free 32-bit atomics enter
production processing; 64-bit sequence tags are protected ring payloads.
Counters wrap modulo 2^32 and snapshots are independent reads. Memory is not
locked, so page faults remain possible. Deadline counters report slow processing
or late timer wakeups; device underrun counters are reported separately.

`:best_effort` requests Linux FIFO, macOS time-constraint scheduling, or Windows
MMCSS Pro Audio registration. `:strict` fails if the requested service is denied;
`priority_granted` reports the result. On supported macOS systems, the device
worker joins the playback device's Audio Workgroup and leaves before exit;
`device_stats(...).workgroup_joined` exposes membership. Priority registration
is reverted on exit. No CPU-affinity or hard-deadline guarantee is made.
Plugins must tolerate unsupported optional host features and provide realtime-safe
processing. Plugins embedding a Julia runtime are unsuitable for this path.

## Validation and release sequence

`Live` builds all adapters from source and exercises arithmetic, delay, latency,
isolation, capture, restart/failure cleanup, repeated start/stop and native
progress during Julia GC on Linux/macOS/Windows. Native probes check control/audio
identity, event offsets, saturation, index rollover, deadline injection, denied
priority, irregular device callback sizes, late playback, and device loss.
Linux probes also run address/undefined/thread sanitizers and 32-bit builds.
Allocation guards check the linked host path; they cannot certify allocations
inside arbitrary shared-library plugins.

Before a general binary release, require passing CI for supported targets and
physical playback/capture checks (including unplug/replug) on each native
backend. After the source merges, pin the Yggdrasil host recipes to that commit,
build and register the JLLs, then raise compatibility floors and make live mode
available from those artifacts. The source PR does not publish a v2 release or
claim completion of hardware validation.
