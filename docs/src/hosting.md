# Hosting a plugin

The host loads a plugin bundle, activates it at a fixed block size, and runs blocks of
samples through it. It holds **one plugin at a time**: there is no handle, and
[`clap_is_open`](@ref) is the whole of its lifecycle state.

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

Two caveats follow from the host holding one module at a time:

  - **Registering closes whatever is open.** A scan has to load the bundle it is
    scanning, and that evicts the open plugin. Register before you open, not between
    blocks.
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

## How the host is shipped

The C host (`csrc/clap_host.c`) is built by
[Yggdrasil](https://github.com/JuliaPackaging/Yggdrasil) and shipped prebuilt as
`CLAPHost_jll`, so hosting needs no C toolchain and writes nothing into the package
directory. The sources stay in the repository anyway, because a generated standalone C
program links `clap_host.c` directly with no Julia present:
[`clap_lib_path`](@ref) gives the prebuilt library's absolute path and
[`clap_src_path`](@ref) the source it was built from.

[`clap_host_available`](@ref) says whether the JLL has a build for this platform.
`CLAPHost_jll` 1.0.1 builds for Linux, macOS and Windows, so in-process hosting runs on
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

## Testing without third-party binaries

`test/plugins/ap_test_plugins.c` is a CLAP bundle written for this repository. Hosting is
only proved by hosting something, and depending on a third-party plugin would make the
suite rest on a binary whose arithmetic cannot be checked and may not even be fetchable.
Because these are ours, every expectation is arithmetic rather than a recording:

  - `ap.gain` — output equals input times the gain, sample-exactly;
  - `ap.onepole` — two consecutive blocks equal one continuous run over the concatenated
    input, which is what proves state survives block boundaries;
  - `ap.lookahead` — reported latency is real, and is surfaced rather than silently
    absorbed.

## LV2 discovery goes through lilv

LV2 metadata — which port index is audio in, which is a control and what its range is —
lives in Turtle/RDF manifests next to the binary rather than in the binary, which in
practice means `lilv`. `csrc/lv2_host.c` uses it, and does rather more than the audio
path:

  - `lv2_host_scan(lv2_path)` loads every bundle under a search path and enumerates the
    plugins, readable back by URI and name;
  - `lv2_host_open(lv2_path, uri, …)` classifies every port with `lilv_port_is_a`, reads
    control ranges out of the manifest, finds the designated `lv2:latency` port, and
    connects every port itself — so a caller names a plugin by URI and never sees a port
    index;
  - a plugin that requires a host feature this host does not provide, or an atom, CV or
    event port that is not `connectionOptional`, is refused at open with a message saying
    which. An unconnected required port is undefined behaviour in the LV2 spec, so
    refusing is the honest answer.

Every JLL that needs exists today — `Lilv_jll`, `Serd_jll`, `Sord_jll`, `Sratom_jll`,
`lv2_jll` and `Zix_jll` — so nothing about the format is blocked. The Julia layer over
this host is not on `main` yet; until it is, LV2 is reached the way `test/probe_lv2.c`
reaches it, from C linking `csrc/lv2_host.c` against lilv, which the `C probes` workflow
runs on every pull request.
