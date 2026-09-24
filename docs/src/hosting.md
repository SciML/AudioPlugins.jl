# Hosting a plugin

The host loads a plugin bundle, activates it at a fixed block size, and runs blocks of
samples through it. The default API holds one plugin at a time;
[`ClapInstance`](@ref) opens independent effects that can coexist in a chain.

## A first block

The package ships its own CLAP bundle for testing — a gain, a one-pole filter and a
16-sample lookahead — so there is something to host without fetching a third-party
binary. [`clap_test_bundle`](@ref) compiles it (this is one of the two places a C
compiler is needed) and returns its path:

```julia
using AudioPlugins
const AP = AudioPlugins

bundle = clap_test_bundle()
clap_scan(bundle)
# 3-element Vector{@NamedTuple{id::String, name::String}}:
#  (id = "ap.gain",       name = "AudioPlugins Test Gain")
#  (id = "ap.onepole",    name = "AudioPlugins Test One Pole")
#  (id = "ap.lookahead",  name = "AudioPlugins Test Lookahead")
```

[`clap_scan`](@ref) enumerates a bundle without instantiating anything. Pick one and open
it:

```julia
clap_open!(bundle; plugin_id = "ap.gain", sample_rate = 48000, block_size = 64, channels = 1)

clap_plugin_name()   # "AudioPlugins Test Gain"
clap_block_size()    # 64
clap_sample_rate()   # 48000.0
```

The plugin is activated with `min == max == block_size`, so a plugin that cannot work at
a fixed block fails here, loudly, rather than at the first block of audio.

`channels` is the number of **host** channels the block carries, not the plugin's own
layout. The plugin's declared `clap.audio-ports` layout is handed over exactly, but only
its **main** port is routed: main input channel *k* receives host channel
`min(k, channels-1)`, every non-main input (an unrouted sidechain) reads silence, main
output channel *k* lands on host channel *k* when `k < channels` and is discarded past it
(a stereo plugin at `channels = 1` loses its right channel), and every non-main output is
discarded. A mono main output at `channels = 2` is duplicated onto both host channels, and
a plugin with no output ports produces silence. A main input narrower than `channels` —
a mono-input plugin at `channels = 2` — fails at open with an error naming the layout,
and a plugin without `clap.audio-ports` declares no audio ports at all.

Parameters are discovered by number, not by name:

```julia
clap_params()
# 1-element Vector:
#  (id = 0.0, name = "Gain", min = 0.0, max = 4.0, default = 1.0)
```

Then push a block through. This is two calls: fill the input block and take its token,
then process that token and take the output block's token.

```julia
tok = clap_fill!(fill(1.0, 64))                              # -> input token
out = AP.clp_process(tok, 0.0, 0.5, -1, 0, -1, 0, -1, 0)     # -> output token, gain = 0.5
y   = clap_out(out)                                          # 64-element Vector{Float64}
y[1]                                                         # 0.5
```

and when you are done:

```julia
clap_close!()
```

[`clap_close!`](@ref) deactivates, destroys and unloads, and is safe to call when nothing
is open.

## Tokens, and why the API has them

`AudioPlugins.clp_process` takes the token of the block it is to read, and every reader
takes the token of the block it is to read back. A token that does not name the *current*
block is refused — `NaN` from the scalar readers, an empty vector from
[`clap_out`](@ref) — rather than answered from whatever the buffer still holds.

That is not defensive programming for its own sake. The host's first consumer is a
synchronous modelling compiler in which the order of two equations is not otherwise
pinned down, and the token is what makes "read the output of the block you just
processed" expressible as a data dependency. For a Julia caller writing statements in
order, the effect is simply that a stale read is a visible error instead of a
plausible-looking wrong answer.

Two properties follow from the same design, and both matter if you drive this from a
solver rather than from a script:

 1. **Exactly one `clp_process` per tick.** A second call advances the plugin's internal
    state twice for one block of time. [`clap_n_process`](@ref) counts the calls so a test
    can assert it, and [`clap_reset_counters!`](@ref) zeroes the counter.
 2. **Contiguity is yours to guarantee.** The host knows the block size and the sample
    rate but cannot see your clock. Feed it every sample exactly once, or the output is
    valid-looking audio that is not continuous.

## Driving parameters

`AudioPlugins.clp_process` carries four `(id, value)` slots. Each is a parameter id from
[`clap_params`](@ref) and the value to set for this block; a negative id leaves the slot
unused:

```julia
out = AP.clp_process(tok, 0.0, gain, 1.0, cutoff, -1, 0, -1, 0)
```

Values go into the plugin's input event list as `CLAP_EVENT_PARAM_VALUE`, which is the
mechanism CLAP defines, rather than writing to the controller behind the processor's
back. A value is only sent when it differs from the last one sent for that id, so holding
a parameter constant costs one event on the first block and none afterwards.

Ids are numbers rather than strings because a `clap_id` is a `UInt32` and every `UInt32`
is exactly representable as a `Float64`: a model can name its own parameters, and there
is nothing to keep in sync driver-side. The price is that you look the numbers up once,
with [`clap_params`](@ref), and pass them around yourself.

### More parameters than there are slots

Four slots is a floor. Plenty of plugins have more than four parameters — in the
Airwindows collection 136 of the 504 do, and the widest has thirteen — so
`AudioPlugins.clp_set` drives them by chaining instead:

```julia
tok = clap_fill!(x)
for (i, v) in enumerate(values)          # one call per parameter
    tok = AP.clp_set(tok, i - 1, v)
end
out = AP.clp_process(tok, -1, 0, -1, 0, -1, 0, -1, 0)
```

Each call returns the token it was given, so the calls form a *chain*, and that is the
whole design: a synchronous program orders by data dependency and by nothing else, so
unchained calls could be scheduled after the `clp_process` they were meant to precede. In
a generated model the chain is one equation per parameter through intermediate variables.

The two mechanisms compose — slots and chain can both drive a block — and each keeps its
own change-detection state, so the one combination worth avoiding is driving the *same*
id through both in one block, which sends two events for it.

A refused link (`NaN`) poisons the rest of the chain rather than being skipped, and
filling a new input block abandons whatever chain was pending, so a refusal cannot leak
into the next block's parameters.

### Refusing the wrong plugin

The default API holds one plugin at a time and the driver opens it, so a model built
against one plugin will happily process through another and return numbers that look
fine. `AudioPlugins.clp_expect` is the guard: it returns its token when the open plugin
is the one at the given index of its bundle — the `index` field of [`plugins`](@ref) —
and `NaN` otherwise.

```julia
tok = AP.clp_expect(clap_fill!(x), 137)   # 137 = plugins(Coll_jll)[k].index
```

[`clap_plugin_index`](@ref) is the driver-side counterpart, for asserting the same thing
from outside the model.

## Reading the output

[`clap_out`](@ref) is the vector-at-a-time reader. The scalar readers exist for the same
reason the rest of the API is scalar — they are callable from a generated equation:

```julia
AP.clp_out_sample(out, i, ch)   # one sample, zero-based index and channel
AP.clp_out_rms(out)             # RMS over the block, all channels
AP.clp_out_peak(out)            # largest absolute sample
AP.clp_out_valid(out)           # 1.0 if `out` is still the current output block
```

[`clap_latency`](@ref) reports the latency the plugin declares, in samples. It is
**not compensated by default**: hosting a lookahead plugin leaves its output shifted by
that many samples relative to the input. `clap_open!` accepts
`compensate_latency = true`, an opt-in Julia-side mode in which [`clap_out`](@ref)
returns the aligned stream — output block `k` corresponds to input block `k` — and
[`clap_flush!`](@ref) yields the tail at end of stream. See
[Reported latency is surfaced, not compensated, by default](@ref).

## Generating input node-side

`clap_fill!` is driver-side: it takes a Julia vector. When the source should live inside
the model instead, `AudioPlugins.clp_in_tone` generates a block from its arguments alone:

```julia
tok = AP.clp_in_tone(t, CLAP_WAVE_SINE, 1000.0, 0.5)   # block ending at source time t
```

`t` is in seconds and names the *end* of the block, so the frame is a pure function of
the arguments and two reads in one tick cannot disagree. The waveform codes are
[`CLAP_WAVE_SILENCE`](@ref), [`CLAP_WAVE_SINE`](@ref), [`CLAP_WAVE_SQUARE`](@ref),
[`CLAP_WAVE_RAMP`](@ref) and [`CLAP_WAVE_IMPULSE`](@ref).

## Plugin collections: the bundle registry

Opening a plugin by path works, but a collection shipped as a JLL has a path only the JLL
knows. The registry is the seam: a small sublibrary package wrapping the JLL calls
[`register_bundle!`](@ref) with the path it exposes, and from then on every plugin in that
bundle is openable by its id alone.

```julia
using AudioPlugins, AudioPluginsSomeCollection   # the sublibrary registers on load

plugins()                    # every plugin from every registered bundle
plugins(SomeCollection_jll)  # just this collection's

clap_open!("org.example.galactic"; sample_rate = 48000, block_size = 256, channels = 2)
```

[`plugins`](@ref) returns `(id, name, bundle, index, source)` per plugin: `index` is its
position in its own bundle's factory, and `source` the module that registered it.
[`bundles`](@ref) is the same view one level up — path, source, and plugin count.

**Nothing is registered by default.** No plugin collection ships with this package and
none is downloaded; with no sublibrary installed, `plugins()` is empty and `clap_open!`
behaves exactly as it always did. That is deliberate rather than incidental: the
collections worth bundling are mostly GPL, and an MIT package that merely knows how to
host them must not make that a transitive obligation of everyone who installs it. A
collection is opt-in — you install it by name and `using` it — and what you load runs **in
your process**, so its licence and its stability are both yours (see
[Plugins run in-process](@ref)).

Registering by hand is the same call:

```julia
register_bundle!("/path/to/Collection.clap")            # -> number of plugins registered
register_bundle!(path; source = MyCollection_jll)       # label it, for plugins(mod)
unregister_bundle!(path)
```

A bundle that cannot be loaded is refused **at registration**, with the host's own
message, rather than part-way through a render — which is the other thing the registry
buys: a collection whose descriptors disagree with reality is caught at `using` time.

Two caveats apply to discovery and the default plugin (independent `ClapInstance`
effects stay open):

  - **Registering closes the default plugin.** Register before opening the
    default plugin, not between its blocks. Independent instances remain open.
  - **An id two registered bundles both declare is refused, not guessed.** Opening it
    throws and names both bundles; say which you mean with
    `clap_open!(bundle; plugin_id = id)`.

Wrapping a collection is about six lines:

```julia
module AudioPluginsSomeCollection
using AudioPlugins: register_bundle!
using SomeCollection_jll: SomeCollection_jll
__init__() = register_bundle!(SomeCollection_jll.collection_clap; source = SomeCollection_jll)
end
```

Such a wrapper lives under `lib/` in this repository as its own package, so that
installing it is what pulls the JLL in — nothing about depending on AudioPlugins does.
A package that already depends on the JLL for its own reasons can register from a package
extension instead; the registry does not care which. The `.clap` module is a `FileProduct`
in the JLL — it is a `.so`/`.dll` on Linux and Windows and a bundle directory on macOS,
and `FileProduct` covers both.

[Plugin collections](@ref) lists which collections exist, what each is licensed under, and
what writing a sublibrary for one involves.

## How the hosts are shipped

The C hosts (`csrc/clap_host.c`, `csrc/lv2_host.c`) are built by
[Yggdrasil](https://github.com/JuliaPackaging/Yggdrasil) and shipped prebuilt as
`CLAPHost_jll` and `LV2Host_jll`, so hosting needs no C toolchain and writes nothing into
the package directory. The sources stay in the repository anyway, because a generated
standalone C program links them directly with no Julia present:
[`clap_lib_path`](@ref) / [`lv2_lib_path`](@ref) give the prebuilt libraries' absolute
paths and [`clap_src_path`](@ref) / [`lv2_src_path`](@ref) the sources they were built
from.

[`clap_host_available`](@ref) says whether the JLL has a build for this platform.
`CLAPHost_jll` builds for Linux, macOS and Windows, so in-process hosting runs on
all three. Where a platform has no build, the package still loads and
[`export_plugin`](@ref) still works, but the `clap_*` hosting calls cannot load the host;
host from a C program over `csrc/clap_host.c` instead, as the test probes do.

To work on `csrc/clap_host.c` itself, build it locally and point the JLL at your build
through a preference, then restart Julia:

```julia
using Preferences, CLAPHost_jll
set_preferences!(CLAPHost_jll, "libclap_host_path" => "/path/to/libclap_host.so")
```

[`build_clap_host!`](@ref) is a deprecated no-op kept for callers written against the 1.0
API; it returns [`clap_lib_path`](@ref).

The VST3 host ships the same way, as `VST3Host_jll`: [`vst3_lib_path`](@ref) and
[`vst3_src_path`](@ref) are its equivalents. The VST3 SDK itself is **not** vendored —
it is a source tree of a different order of magnitude than the CLAP and LV2 headers.
`vst3sdk_jll` builds it once (pinned to `v3.8.1_build_84`, MIT) and `VST3Host_jll`
compiles `csrc/vst3_host.cpp` against it as a build dependency, so the host library is
self-contained and exports only its `extern "C"` surface.

## Testing without third-party binaries

`test/plugins/ap_test_plugins.c` is a CLAP bundle written for this repository, and
`test/plugins/ap_test_lv2.c` an LV2 bundle with those three plus a two-input
merge and a sidechain fixture for the channel policy. Hosting is only proved
by hosting something, and depending on a third-party plugin would make the
suite rest on a binary whose arithmetic cannot be checked and may not even be fetchable.
Because these are ours, every expectation is arithmetic rather than a recording:

  - `ap.gain` — output equals input times the gain, sample-exactly;
  - `ap.onepole` — two consecutive blocks equal one continuous run over the concatenated
    input, which is what proves state survives block boundaries;
  - `ap.lookahead` — reported latency is real, and is surfaced rather than silently
    absorbed.

## Channels

`channels` is the width of the host's audio block, and the rule for matching it
to a plugin is the same whichever format the plugin ships in — CLAP audio
ports, VST3 buses and LV2 port groups are different words for the same
arrangement:

  - A host channel is never silently dropped: opening a plugin with fewer main
    audio inputs than `channels` is refused, rather than leave a host channel
    connected to nothing. A plugin with no audio inputs is a generator and
    opens at any `channels`.
  - The k-th main audio input is fed host channel `min(k, channels-1)`, so a
    mono host feeds every input of a stereo plugin by repeating its one
    channel.
  - An input the plugin marks as non-main — a sidechain or an auxiliary bus —
    reads silence, and a non-main output is discarded; neither counts toward
    the rule above.
  - The k-th main output writes host channel `k` and further outputs are
    discarded. Fewer main outputs than `channels` repeats the last one — a mono
    plugin comes out centred — and a plugin with no audio outputs is silent.

## VST3

A `.vst3` bundle is a shared object with a class factory, so discovery is a factory
walk rather than a manifest read and needs no extra library. [`vst3_scan`](@ref) lists
the audio-effect classes it exports:

```julia
using AudioPlugins

vst3_scan("/usr/lib/vst3/SomePlugin.vst3")
# Vector{@NamedTuple{id::String, name::String, category::String}}
```

`id` is the class id as 32 hex characters, and is what [`vst3_open!`](@ref) takes;
`""` opens the first audio-effect class:

```julia
vst3_open!(path; class_id = id, sample_rate = 48000, block_size = 64, channels = 1)

vst3_plugin_name()
vst3_params()   # id, name, normalized/plain range and default, step count, flags
```

VST3 parameters are **normalised**: the plugin sees every parameter as a `Float64` in
`[0, 1]`, and the controller owns the mapping to the plain value a user would read.
[`vst3_params`](@ref) reports the plain range and default, and
[`vst3_param_plain`](@ref) / [`vst3_param_normalized`](@ref) convert between the two,
so a caller can work in whichever it prefers. Parameter changes are queued as
`inputParameterChanges` on the block they are passed with, which is how VST3 wants
them delivered.

[`vst3_latency`](@ref) is `IAudioProcessor::getLatencySamples()`, re-read whenever the
plugin asks for a restart with `kLatencyChanged`; as with CLAP by default and with LV2,
it is surfaced rather than compensated.

The host asks for mono or stereo on the plugin's main buses and deactivates the others
(a sidechain input, event buses). It processes 32-bit float only, and a plugin that
refuses the arrangement, refuses `setupProcessing`, or refuses `setActive` fails at
[`vst3_open!`](@ref), loudly. No editor is ever created, and everything is called from
whichever thread calls it — VST3's main-thread/processing-thread split is not modelled.

[`vst3_test_bundle`](@ref) compiles the same three test plugins as a VST3 bundle,
against the SDK from `vst3sdk_jll`, so the VST3 path is proved against arithmetic that
can be checked rather than against a third-party binary.

The bundle registry above is CLAP-only: [`register_bundle!`](@ref) scans a `.clap`
module and [`plugins`](@ref) reports what CLAP declares. Name the bundle path at
[`vst3_open!`](@ref) instead.

## LV2 discovery goes through lilv

LV2 metadata lives in Turtle manifests next to the binary, so discovery means reading
RDF. This host reads it through [lilv](https://gitlab.com/lv2/lilv), the reference
reader, rather than a hand-rolled parser that could silently mis-map a port — and
`LV2Host_jll` brings lilv, `serd`, `sord`, `sratom` and `zix` with it, so nothing needs
installing by hand.

A search path is a list of directories joined by the platform's separator.
[`lv2_default_path`](@ref) builds one and appends the LV2 specification bundles from
`lv2_jll`, which is what gives lilv the vocabulary to classify what it finds:

```julia
using AudioPlugins

path = lv2_default_path("/usr/lib/lv2")
lv2_scan(path)
# Vector{@NamedTuple{uri::String, name::String}}, one entry per plugin lilv found
```

A plugin is named by URI rather than by an id local to a bundle, so
[`lv2_open!`](@ref) takes the search path and the URI:

```julia
lv2_open!(path; uri = "http://lv2plug.in/plugins/eg-amp", block_size = 64, channels = 1)

lv2_plugin_name()
lv2_params()   # control input ports: id (= port index), name, symbol, min, max, default
```

`id` is the port index, and [`lv2_param_value`](@ref) and the `lv2_fill!` /
[`lv2_out`](@ref) pair work exactly as their CLAP counterparts do — same token
discipline, same fixed block size, same one-plugin-at-a-time lifecycle.
[`lv2_latency`](@ref) reports the plugin's designated `lv2:latency` port and is
surfaced rather than compensated; the opt-in `compensate_latency` mode
[`clap_open!`](@ref) offers has no LV2 counterpart.

The host offers four features — `urid:map`, `urid:unmap`, `bufsz:fixedBlockLength` and
`bufsz:boundedBlockLength` — and connects audio, control and `atom:AtomPort` ports.
An atom port gets one `atom:Sequence` buffer, sized from its declared
`rsz:minimumSize`; timestamped MIDI events reach an input port through
[`lv2_midi!`](@ref) with sample-accurate frame offsets, and an output sequence is kept
valid for the plugin to write (its contents are not decoded). A plugin that *requires*
anything else (an atom buffer type other than Sequence, a CV or event port, or another
host feature such as `worker:schedule`) is refused at [`lv2_open!`](@ref) with a
message naming what it asked for. An unconnected required port is undefined behaviour
in the LV2 specification, so refusing is the honest answer; a plugin whose extras are
optional opens fine.

Channel arrangement follows the shared rule in [Channels](@ref); the LV2 part
is telling main from non-main ports. A port marked `lv2:isSideChain`, carrying
`pg:sideChainOf`, or outside a declared `pg:mainInput`/`pg:mainOutput` group is
non-main; everything else is main.

[`lv2_test_bundle`](@ref) compiles the same test plugins as an LV2 bundle, so the
LV2 path is proved against arithmetic that can be checked rather than against a
third-party binary.

The bundle registry above is CLAP-only: [`register_bundle!`](@ref) scans a `.clap`
module, and an LV2 search path — a list of directories rather than one module — does not
fit that shape. Name the search path at [`lv2_open!`](@ref) instead.

## Several CLAP effects at once

`ClapInstance` opens an independent effect. Pass it first to the CLAP readers,
fill/output functions and `AudioPlugins.clp_*` operators. `clap_copy!` transfers
one effect's output directly into another's input; both must have matching
sample rates, block sizes and host channel counts. For example, a gain followed
by a stateful one-pole filter:

<!-- illustrative -->
```julia
using AudioPlugins
const AP = AudioPlugins
bundle = clap_test_bundle()

ClapInstance(bundle; plugin_id = "ap.gain", block_size = 64) do gain
    ClapInstance(bundle; plugin_id = "ap.onepole", block_size = 64) do filter
        for _ in 1:2
            input = clap_fill!(gain, ones(64))
            amplified = AP.clp_process(gain, input, 0, 0.5, -1, 0, -1, 0, -1, 0)
            filtered = AP.clp_process(filter, clap_copy!(filter, gain, amplified),
                                     0, 0.25, -1, 0, -1, 0, -1, 0)
            y = clap_out(filter, filtered)
            # First block starts at 0.125; the second continues the filter state.
        end
    end
end
```

The do-block closes its instance even on an exception. Without it, close with
`clap_close!(instance)` in `finally`. Each instance has its own buffers,
parameters and processing state; scanning, reopening or closing the default
plugin leaves independent instances alone. The existing no-handle API keeps
its replacement-on-open behavior. All host calls must be serialized: several
effects can remain live, but simultaneous calls from multiple threads are not
supported. Instance latency is reported by `clap_latency(instance)` and is not
compensated.

For generated nodes, pass `instance.handle` (an exact integer `Float64`) as
the extra first argument to the `clp_*` operators; `AP.clp_copy(destination,
source, token)` is the scalar counterpart of `clap_copy!`. The C API adds
`clap_host_open_instance`, `clap_host_close_instance` and `_for` entry points
without changing the existing symbols. Handles are never reused, and stale
or foreign block tokens are refused. Tokens are opaque: do not do arithmetic
on them. Opening still happens driver-side, where bundle paths are strings.

The instance ABI needs a `CLAPHost_jll` build from these C sources; the currently
required 1.2 build does not provide it. Until that JLL is released and the
compat floor updated, use the local-build preference described in [`clap_lib_path`](@ref). The
`CLAPInstances` test group compiles the shipped sources directly so CI can
exercise the new ABI before its JLL release.
