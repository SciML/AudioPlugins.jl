# AudioPlugins.jl

Headless hosting of third-party audio plugins from Julia, behind a C ABI of scalar doubles.

```julia
using AudioPlugins
bundle = clap_test_bundle()            # builds the test plugins shipped here
clap_host_open(bundle, "ap.gain", 48000, 64, 1)
```

Three formats:

| Format | Licence | State |
|---|---|---|
| **CLAP** | MIT, header-only | host implemented and tested — discovery, instantiation, parameters, block processing, latency |
| **LV2** | ISC | audio path implemented (`connect_port` / `run`); **discovery is not**, see below |
| **VST3** | MIT since SDK 3.8 | host implemented and tested — discovery, instantiation, parameters, block processing, latency |

## How the hosts are shipped

The hosts (`csrc/clap_host.c`, `csrc/vst3_host.cpp`) are built by
[Yggdrasil](https://github.com/JuliaPackaging/Yggdrasil) and shipped prebuilt as
`CLAPHost_jll` and `VST3Host_jll`, so using this package needs no C or C++ toolchain
and writes nothing into the package directory. The JLLs are *in addition to* `csrc/`,
not a replacement: the sources stay in the repository because a generated standalone
program links them directly, with no Julia present. `clap_lib_path()` /
`vst3_lib_path()` give the prebuilt libraries' absolute paths and `clap_src_path()` /
`vst3_src_path()` the sources they were built from; each JLL's version tracks the
release of this package whose `csrc/` it was built from.

The VST3 SDK is **not** vendored (it is a source tree of a different order of
magnitude than the CLAP and LV2 headers). `vst3sdk_jll` builds it once (pinned to
`v3.8.1_build_84`, MIT) and `VST3Host_jll` compiles `vst3_host.cpp` against it as a
build dependency, so the host library is self-contained and exports only its
`extern "C"` surface.

The only thing that needs a compiler is building the *test* plugins
(`clap_test_bundle()`; `vst3_test_bundle(sdk)` needs a C++ compiler and the SDK from
`vst3sdk_jll`, a test dependency only), and those go into a per-package scratch
space, so a read-only installation hosts plugins fine and fails only at test time,
with a message that says so. The C/C++ sources are also compiled and probed directly
in CI (`.github/workflows/CProbe.yml`), with no Julia involved.

To work on `csrc/clap_host.c` itself, build it locally and point the JLL at your
build through a preference, then restart Julia:

```julia
using Preferences, CLAPHost_jll
set_preferences!(CLAPHost_jll, "libclap_host_path" => "/path/to/libclap_host.so")
```

## Why the API looks like C rather than like Julia

The host is deliberately a thin layer over named `ccall`s into a shared library at a fixed
path, rather than an idiomatic Julia API. Its first consumer is a synchronous modelling
compiler which requires exactly that: a *named* symbol in a library at a compile-time
constant path, taking and returning scalar doubles. A process-local function pointer, a
Julia callback, or a C++ type cannot cross that boundary at all.

The upside for everyone else is that the same host serves a generated standalone C program
with no Julia present, and the cost to an ordinary Julia caller is close to zero.

## The processing contract

Every plugin format shares it, which is why one host shape fits all three:

- a **contiguous** block of samples in, a block out;
- block size **fixed at setup**;
- plugin **state persisting** across blocks;
- **scalar parameters** that may change per block.

Contiguity is the caller's responsibility: the host knows the block size and the sample
rate but cannot see your clock, so it cannot check that you are feeding it every sample
exactly once. A plugin fed a non-contiguous stream returns a perfectly valid-looking result
that simply is not continuous audio.

**Headless only.** No plugin GUI is ever loaded, and none will be.

## Testing without third-party binaries

`test/plugins/ap_test_plugins.c` is a CLAP bundle written for this repository, and
`test/plugins/ap_test_vst3.cpp` the same three plugins as a VST3 bundle: a gain, a
one-pole filter, and a 16-sample lookahead. Hosting is only proved by hosting something,
and depending on a third-party plugin would make the suite rest on a binary whose
arithmetic cannot be checked and may not even be fetchable. Because these are ours, every
expectation is arithmetic rather than a recording:

- `ap.gain` — output equals input times the gain, sample-exactly;
- `ap.onepole` — two consecutive blocks equal one continuous run over the concatenated
  input, which is what proves state survives block boundaries;
- `ap.lookahead` — reported latency is real and is surfaced rather than silently absorbed.

## LV2 discovery is missing, and why

The LV2 *audio* path is genuinely simpler than CLAP's — one ISC header, ports connected
once by index, then `run(n_samples)`. Discovery is the problem: LV2 metadata lives in
Turtle/RDF manifests, which in practice means `lilv`, which needs `serd`, `sord` and
`sratom`. Julia's General registry currently has only `Serd_jll`.

So the audio path here takes the port map as explicit arguments instead. A half-correct
hand-rolled Turtle parser that silently mis-maps a port would be worse than no discovery at
all. The fix is Yggdrasil recipes for the missing JLLs, which would benefit every Julia
audio project rather than only this one.

The VST3 suite additionally hosts the SDK's own `again` example (the sample-accurate
variant, prebuilt by `vst3sdk_jll`) and asserts its output is sample-exact.

## VST3: what crosses the ABI

A plugin class is named by its 32-hex-character class id (`vst3_scan` lists them).
Parameter **values are normalised (0..1)** as VST3's processor consumes them through
`inputParameterChanges`; `vst3_params()` reports the controller's plain range and
default, and `vst3_param_plain` / `vst3_param_normalized` convert. Latency is
`IAudioProcessor::getLatencySamples()`, re-read when the plugin asks for a restart with
`kLatencyChanged`. The host asks for mono or stereo on the plugin's main buses and
deactivates the others (a sidechain input, event buses); no editor is ever created.

## Known limits

- **Third-party binary code runs in-process.** A plugin that segfaults takes the Julia
  process down with it. Out-of-process hosting is the robust answer and is a much larger
  project; in-process is fine for offline work, and that is what this is for.
- **Realtime discipline is not provided.** CLAP and VST3 alike ask a host to keep an
  audio thread that never blocks and never allocates (VST3 additionally distinguishes
  the main thread from the processing thread; this host calls everything from whichever
  thread calls it). A garbage-collected process driving a solver that may retry a step
  cannot promise that. Harmless offline; not harmless on a live capture with a deadline.
- **Reported latency is surfaced, not compensated.** Callers that care must align the
  stream themselves.

## Licence

MIT. Vendored headers under `csrc/vendor/` carry their own permissive licences (CLAP: MIT;
LV2: ISC) — see `LICENSE`. The VST3 SDK (MIT since 3.8) is used unmodified through
`vst3sdk_jll`; its copyright and licence text ship with that JLL.
