# Experimental live hosting

The direction approved in [issue #8](https://github.com/SciML/AudioPlugins.jl/issues/8#issuecomment-5956165702)
is an opt-in native live host. The first implementation is a source-built Linux
CLAP adapter, separate from the synchronous offline hosts. This is a development
ABI, not a completed v2 release or an audio-device backend. Existing JLL compat
floors and offline behavior are unchanged.

## Build and exercise it

From the repository root on Linux:

```sh
cc -std=c11 -O2 -fPIC -shared -Wall -Wextra -Werror \
   -o /tmp/libclap_live.so csrc/clap_live.c -pthread -ldl -lm
```

The native library has no dependency beyond the platform C/thread/dynamic-loader
facilities and the vendored MIT CLAP headers. Julia uses the Libdl standard library.
The current JLLs do not contain these symbols; availability is checked at use time.

```julia
using AudioPlugins

bundle = clap_test_bundle()
open_live(bundle; plugin_id = "ap.gain", library = "/tmp/libclap_live.so",
          block_size = 64, channels = 2, queue_blocks = 8) do session
    x = ones(Float32, 128)       # interleaved stereo
    y = similar(x)
    try_write!(session, x; events = [LiveEvent(; value = 0.5)])
    start!(session)
    sleep(0.05)                 # Julia may stall; the native thread keeps going
    metadata = try_read!(session, y)
    poll!(session)              # dispatch pending plugin main-thread work
    @show metadata live_stats(session)
end                            # join, deactivate, destroy, unload
```

Control and queue calls must execute on Julia thread 1, the same OS thread that
opened the session. Do not call them from migrating tasks. The C ABI enforces
the creator OS thread even when Julia checks are bypassed. Use the do-block or
explicit `stop!` and `close`; there is no finalizer that might unload an active
plugin on an arbitrary thread. Plugin code that never returns can prevent join.

## Control-thread decision

Linux CLAP permits a fixed designated control thread. Open, init, activation,
deactivation, destruction and `on_main_thread` use that thread. A native audio
thread exclusively performs start-processing, process and stop-processing.
`clap.thread-check` reports those identities, including calls from plugin workers.
No Julia callback or Julia-owned audio pointer reaches the processing thread.

`request_callback` publishes an atomic flag; `poll!` dispatches at most one
coalesced callback. Julia must service it regularly; a Julia stall may delay
control work even while audio continues. A restart request stops audio, joins
off the processing thread and deactivates on control. `poll!` returns false;
the caller must close and reopen to negotiate potentially changed ports and
parameters. It cannot continue with cached metadata from before the restart.

Sessions own separate plugin instances, buffers, queues and counters. They
never enter the offline registries or share their error/token state. CLAP 1.2
nested module initialization permits live and offline hosts to hold references
to the same binary. Serialize module lifecycle operations, including offline
operations, on the same control thread; arbitrary plugin-global thread safety
cannot be supplied by a host.

## Device-clock decision

The timer driver is for hardware-free development. A free-running monotonic
clock is **not** a playback/capture clock and cannot be synchronized to a device
merely by choosing the same nominal sample rate.

`AP_LIVE_DEVICE` in `csrc/clap_live.h` instead delegates cadence to a native
device owner. Control calls `ap_live_start`; the stable native audio thread
calls `ap_live_device_begin`, one `ap_live_device_process` per device block,
then `ap_live_device_end`. Processing optionally copies output directly to the
device buffer, independently of the observation ring. A full observation ring
does not drop that device output. These calls must never be Julia callbacks.

Shutdown requests stop on control, ends processing on the device thread, then
quiesces all callbacks before control deactivates/closes. `ap_live_stop` returns
`AP_LIVE_AGAIN` while device processing is owned by the audio thread; it never
destroys resources under an active callback. A device adapter must guarantee
no future callback before closing, including calls that would return an error.

This patch supplies the contract and a deterministic native device emulator.
It does not choose or open ALSA, JACK, PipeWire, CoreAudio or WASAPI devices.
Capture routing, variable device-block adaptation, clock drift/resampling and
device loss belong to that adapter. Adding a device library remains a separate
dependency/license decision. No hardware playback is claimed by the Julia API.

## Bounded processing contract

Audio and at most 64 sorted parameter/MIDI events travel together in preallocated
SPSC block rings, published with acquire/release atomics. Input sequence tags
associate output with submitted input. Output tick gaps expose discarded blocks.
The first adapter accepts one mono/stereo input/output bus and, optionally, one
MIDI 1.0 input port. Other layouts and unsupported events are rejected.

Missing input processes silence, retains the last parameters, and increments
underruns. Output saturation discards the newest observation block and counts
it. A parameter event takes effect at its frame offset in its queued block;
these are not absolute device timestamps. Output events are refused and counted.
Plugins may continue producing tails during silence input.
Queueing and plugin latency are not compensated; a live latency query is not
yet exposed, so device integration must add latency negotiation/reporting.

Storage is allocated and touched before start; the host's processing path does
not allocate, wait on Julia, log, or take mutexes. Atomic types are verified
lock-free on open; only 32-bit atomics are used in production, while 64-bit
sequence numbers are ring payloads. Counters are independent snapshots and wrap
modulo 2^32. Storage is not memory-locked, and page faults are still possible.
The timer sleeps between blocks; late wakeups never cause an unbounded catch-up
loop. Reported deadline misses include slow host/plugin processing and timer
wakeups late by more than a block period, not hardware xrun reports.

`:best_effort` requests Linux FIFO priority and reports denial through
`priority_granted`; `:strict` fails startup on denial. Priority is restored when
the native device thread ends processing. No CPU-affinity guarantee is made.
No finite queue, OS policy, or host can guarantee a third-party plugin meets its
deadline. In particular, Julia-authored plugins that initialize a Julia runtime
are not suitable for this path. Plugins must tolerate unavailable host extensions.

## Validation and remaining release work

`test/probe_clap_live.c` checks sample-accurate parameters/MIDI, delay across
blocks, queue wrap/saturation, sequence gaps, silence, control/audio identities,
callbacks, restart, failed init/activation/start/process, repeated start/stop,
coexisting sessions and priority denial. A fake clock makes deadline injection
deterministic. Linker allocation guards cover the host's device processing path;
they do not certify arbitrary shared-library plugin allocations. CI runs
address/undefined/thread sanitizers and a 32-bit native probe. `CLAPLive` builds
the source from Julia and verifies native progress across a producer GC/stall.

Before a general live release:

1. Implement an actual native device adapter with device-loss and clock tests.
2. Add isolated LV2 and VST3 adapters. LV2 needs bounded URID storage and feature
   validation; VST3 needs controller/main-thread dispatch and processing-side
   start/stop ordering. Wrapping their current singletons is insufficient.
3. Add macOS main-event-loop dispatch and Audio Workgroups scheduling, and
   Windows main-event-loop/COM dispatch and MMCSS registration/reversion.
   A Julia worker cannot stand in for those application main threads.
4. Extend native and Julia CI to every supported platform and target.
5. After the native source lands, update the Yggdrasil host recipes to a pinned
   merged commit, build/register the JLLs, then raise compat floors and enable
   the supported APIs. Do not depend on an unregistered JLL or mark #8 complete
   based on this first adapter.
