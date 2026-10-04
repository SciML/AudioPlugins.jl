# Known limits

These are properties of the design as it stands, not bugs waiting on a fix in the next
patch release. Each is tracked as an issue.

## Plugins run in-process

Third-party binary code is loaded into the Julia process and called directly. There is no
sandbox: a plugin that segfaults takes the Julia process down with it, and a plugin that
corrupts memory corrupts yours.

Out-of-process hosting is the robust answer, and it is a much larger project — a
separate process, a shared-memory audio transport, and a protocol for the lifecycle and
the parameter events. In-process is fine for offline work, and offline work is what this
is for.

Tracked as [issue #7](https://github.com/SciML/AudioPlugins.jl/issues/7).

## [No realtime discipline in the synchronous hosts](@id No-realtime-discipline)

CLAP asks a host to call `process()` from a realtime thread that never blocks, never
allocates and never takes a lock, and it asks the host to honour the same discipline. A
garbage-collected process driving a solver that may retry a step cannot promise any of
that: the call may come from anywhere, a step may be recomputed, and the surrounding
Julia process allocates and collects freely.

Offline this is harmless — nobody is listening as the output is produced, so a late block
is just a slow run. It stops being harmless the moment this is driven from a live capture
with a deadline, where a plugin that allocates or blocks inside `process()` produces
dropouts that look like a modelling error.

Tracked as [issue #8](https://github.com/SciML/AudioPlugins.jl/issues/8).

An opt-in [experimental native live host](live.md) now exists from source for
Linux CLAP. It provides a separate processing path and a native device-clock
interface; actual audio-device integration and the other formats/platforms
remain pending. It does not change the synchronous contract described here.

## Reported latency is surfaced, not compensated, by default

[`clap_latency`](@ref) returns the latency the plugin declares, in samples. The host
reports that number and does nothing with it. Hosting a lookahead limiter therefore leaves
its output shifted by that many samples relative to the input.

The C hosts keep exactly that behaviour — they stay minimal, because a generated standalone
program links `csrc/` directly and gets only what is documented there. The Julia layer does
offer compensation as an opt-in: open with `compensate_latency = true` and [`clap_out`](@ref)
returns the aligned stream instead — the plugin's first `clap_latency()` output samples are
discarded, output block `k` corresponds to input block `k`, and [`clap_flush!`](@ref) yields
the tail at end of stream. [`clap_compensating`](@ref) says which mode is in force. Under the
mode every processed block must be read once, in order — a skipped block would silently
misalign the stream, so it is an error rather than an empty read — and a plugin that changes
its latency mid-stream errors on the next read.

The `ap.lookahead` test plugin keeps both honest: its reported latency is real, and the
tests assert that it is surfaced by default and compensated under the mode, sample-exactly.

This was tracked as [issue #9](https://github.com/SciML/AudioPlugins.jl/issues/9).

## Contiguity is unchecked

The host knows the block size and the sample rate but cannot see the caller's clock, so it
cannot check that it is being fed every sample exactly once. A plugin fed a
non-contiguous stream returns a perfectly valid-looking result that is not continuous
audio. The token discipline described in [Tokens, and why the API has them](@ref)
catches a *stale* block, not a *skipped* one.
