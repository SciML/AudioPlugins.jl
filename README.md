# AudioPlugins.jl

[![Join the chat at https://julialang.zulipchat.com #sciml-bridged](https://img.shields.io/static/v1?label=Zulip&message=chat&color=9558b2&labelColor=389826)](https://julialang.zulipchat.com/#narrow/stream/279055-sciml-bridged)
[![Global Docs](https://img.shields.io/badge/docs-SciML-blue.svg)](https://docs.sciml.ai/AudioPlugins/stable/)
[![Stable](https://img.shields.io/badge/docs-stable-blue.svg)](https://docs.sciml.ai/AudioPlugins/stable/)
[![Dev](https://img.shields.io/badge/docs-dev-blue.svg)](https://docs.sciml.ai/AudioPlugins/dev/)

[![codecov](https://codecov.io/gh/SciML/AudioPlugins.jl/branch/main/graph/badge.svg)](https://codecov.io/gh/SciML/AudioPlugins.jl)
[![Tests](https://github.com/SciML/AudioPlugins.jl/actions/workflows/Tests.yml/badge.svg?branch=main)](https://github.com/SciML/AudioPlugins.jl/actions/workflows/Tests.yml)

[![ColPrac: Contributor's Guide on Collaborative Practices for Community Packages](https://img.shields.io/badge/ColPrac-Contributor%27s%20Guide-blueviolet)](https://github.com/SciML/ColPrac)
[![SciML Code Style](https://img.shields.io/static/v1?label=code%20style&message=SciML&color=9558b2&labelColor=389826)](https://github.com/SciML/SciMLStyle)

Headless hosting of third-party audio plugins from Julia, behind a C ABI of scalar doubles —
and, in the other direction, authoring plugins from a per-sample C step function.

```julia
using AudioPlugins
bundle = clap_test_bundle()            # builds the test plugins shipped here
clap_open!(bundle; plugin_id = "ap.gain", sample_rate = 48000, block_size = 64, channels = 1)
```

and the same shape for LV2, against whatever the machine has installed:

<!-- illustrative -->
```julia
path = lv2_default_path("/usr/lib/lv2")   # your bundles + the LV2 spec from lv2_jll
lv2_scan(path)                            # every plugin lilv finds, by URI
lv2_open!(path; uri = "http://lv2plug.in/plugins/eg-amp", block_size = 64)
lv2_params()                              # control ports: id, name, symbol, range
```

Two formats:

| Format | Licence | State |
|---|---|---|
| **CLAP** | MIT, header-only | host implemented and tested — discovery, instantiation, parameters, block processing, latency |
| **LV2** | ISC | host implemented and tested — discovery through lilv, instantiation, parameters, block processing, latency |
| VST3 | MIT since SDK 3.8 | not yet implemented |

## How the hosts are shipped

The C hosts (`csrc/clap_host.c`, `csrc/lv2_host.c`) are built by
[Yggdrasil](https://github.com/JuliaPackaging/Yggdrasil) and shipped prebuilt as
`CLAPHost_jll` and `LV2Host_jll`, so using this package needs no C toolchain and
writes nothing into the package directory. The JLLs are *in addition to* `csrc/`,
not a replacement: the sources stay in the repository because a generated
standalone C program links them directly, with no Julia present.
`clap_lib_path()` / `lv2_lib_path()` give the prebuilt libraries' absolute paths
and `clap_src_path()` / `lv2_src_path()` the sources they were built from; each
JLL's version tracks the release of this package whose `csrc/` it was built from.
`LV2Host_jll` depends on `Lilv_jll`, which brings `Serd_jll`, `Sord_jll`,
`Sratom_jll`, `Zix_jll` and `lv2_jll` — the JLLs that made discovery possible.

`CLAPHost_jll` 1.0.1 builds for Linux, macOS and Windows, so in-process hosting runs on
all three. Where a platform has no build, the package still loads and authors plugins,
`clap_host_available()` is `false`, and hosting is done from C over `csrc/clap_host.c`,
as the test probes do.

The only things that need a C compiler are building the *test* plugins
(`clap_test_bundle()`, `lv2_test_bundle()`), which go into a per-package scratch
space, and authoring your own with `export_plugin`. A read-only installation hosts
plugins fine and fails only at test or export time, with a message that says so. The C
sources are also compiled and probed directly in CI (`.github/workflows/CProbe.yml`),
with no Julia involved.

To work on `csrc/clap_host.c` itself, build it locally and point the JLL at your
build through a preference, then restart Julia:

<!-- illustrative -->
```julia
using Preferences, CLAPHost_jll
set_preferences!(CLAPHost_jll, "libclap_host_path" => "/path/to/libclap_host.so")
```

## Plugin collections

Opening a plugin by path works; a collection shipped as a JLL has a path only the JLL
knows. `register_bundle!` is the seam — a sublibrary under `lib/` wraps the JLL and
registers the bundle it ships, and every plugin in it becomes openable by id:

<!-- illustrative -->
```julia
using AudioPlugins, AudioPluginsSomeCollection   # the sublibrary registers on load

plugins()                                # every plugin from every registered bundle
clap_open!("org.example.galactic"; sample_rate = 48000, block_size = 256, channels = 2)
```

**Nothing is registered by default.** No collection ships with this package and none is
downloaded: with no sublibrary installed the registry is empty and `clap_open!` behaves
exactly as it always did. That is the point — the collections worth bundling are mostly
GPL, and an MIT package that merely knows how to host them must not make that a
transitive obligation of everyone who installs it. Installing one is opt-in, by name, and
what you load runs in-process (see "Known limits").

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
`test/plugins/ap_test_lv2.c` + `ap_test_lv2.ttl` the same three plugins as an LV2
bundle: a gain, a one-pole filter, and a 16-sample lookahead. Hosting is only proved by
hosting something, and depending on a third-party plugin would make the suite rest on a
binary whose arithmetic cannot be checked and may not even be fetchable. Because these
are ours, every expectation is arithmetic rather than a recording:

- `ap.gain` — output equals input times the gain, sample-exactly;
- `ap.onepole` — two consecutive blocks equal one continuous run over the concatenated
  input, which is what proves state survives block boundaries;
- `ap.lookahead` — reported latency is real and is surfaced rather than silently
  absorbed (and compensates sample-exactly under `compensate_latency = true`).

The LV2 suite additionally scans the machine's system LV2 directories when it has any
(`/usr/lib/lv2` and friends) and asserts that lilv enumerates what is installed.

## Authoring: from a step function to a plugin

`export_plugin` builds a plugin bundle around any step function of the shape

```c
typedef struct MyPars MyPars;
struct MyPars { double gain; /* ... */ };
typedef struct { /* state */ } my_fx_mem;
typedef struct { double y; } my_fx_out;

my_fx_out my_fx_step(double u, MyPars *pars, my_fx_mem *self);
void      my_fx_reset(my_fx_mem *self);
```

which is what a fixed-step code generator emits: a named parameter struct passed by typed
pointer, a per-instance state struct, a result struct, and `<base>_step` / `<base>_reset`.
The wrapper calls `<base>_step` once per sample per channel, with one `<base>_mem` per channel
that persists across blocks, and writes host parameter changes straight into the struct
fields the descriptor names.

The step comes in one of two forms. **C source with a header** (`CStep`), compiled by the
system compiler. Or **Julia `@ccallable` functions** (`JuliaStep`), compiled by
[juliac](https://github.com/JuliaLang/JuliaC.jl) into a trimmed shared library — the
wrapper and the Julia image become one bundle:

<!-- illustrative -->
```julia
struct GainPars; gain::Float64; bypass::Bool; end
struct GainMem; ticks::Int64; end
struct GainOut; y::Float64; end

Base.@ccallable function my_fx_step(u::Float64, pars::Ptr{GainPars}, self::Ptr{GainMem})::GainOut
    p = unsafe_load(pars)
    return GainOut(p.bypass ? u : p.gain * u)
end
Base.@ccallable function my_fx_reset(self::Ptr{GainMem})::Cvoid
    unsafe_store!(self, GainMem(0))
    return nothing
end
```

The structs are read off the `@ccallable` signature and declared to C by a generated header
(with `_Static_assert`s of every size and offset), so nothing about the layout is written
twice.

The descriptor is a TOML file, and it names no code generator:

```toml
[plugin]
id = "org.example.gain"
name = "Example Gain"

[abi]
base = "my_fx"
pars = "MyPars"               # the C header's parameter struct (C step only)
sample_rate_field = "fs"      # optional: the host's rate lands in pars->fs on activate

[build]
source = "my_fx.c"            # a C step ...
header = "my_fx.h"
pkgconfig = "my_fx.pc"        # optional: Cflags and Libs for compiling and linking source
# julia = "my_fx.jl"          # ... or a Julia step, with optional project, trim, bundle

[[param]]
id = 0
name = "Gain"
field = "gain"
min = 0.0
max = 4.0
default = 1.0
```

<!-- illustrative -->
```julia
using AudioPlugins
spec = read_plugin_spec("my_fx.toml")
export_plugin(spec, "MyGain.clap")             # a .clap on Linux/Windows, a bundle dir on macOS
clap_open!("MyGain.clap"; block_size = 64)      # and host it, right here (C steps)
```

The same package hosts what it builds, so `test/export_tests.jl` proves the seam with
hand-written step functions under `test/export/` in both C and Julia and no generator
anywhere: a gain that is sample-exact at 0.5, and an RBJ peaking EQ whose output matches the
reference recursion, is bitwise identical whether processed as 2 × 128 or 1 × 256 frames,
and is a different, correct filter at each of 44.1, 48 and 96 kHz because the sample rate
arrives as a parameter.

What the exporter needs and does not need:

- **A C compiler** (`cc`, `gcc` or `clang` on `PATH`, or `compiler = ...`), for authoring only.
  Hosting stays toolchain-free.
- **For Julia steps, `using JuliaC` and Julia ≥ 1.12.** JuliaC is a weak dependency; the
  `AudioPluginsJuliaCExt` extension does the build. On Windows a Julia step must be bundled
  (`bundle = true`); see below for why the `.clap` is a shim there.
- **Link flags from the `.pc` file**, not a hardcoded `-lm`: that is how libraries the
  generated C calls into reach the link line, and an undefined symbol fails the link rather
  than the first `dlopen`.
- **No licence machinery.** The output is a plain, royalty-free bundle with nothing embedded.
- **Formats register themselves.** `CLAP` ships here; a format whose SDK cannot be vendored
  publicly subtypes `PluginFormat` out of tree and calls `register_plugin_format!`.

### What a Julia step brings with it

A juliac-built plugin is pure native code for the step itself, but it links `libjulia` and
initialises a Julia runtime when the host loads it. Consequences:

- **Where the runtime is found.** By default the plugin's rpath points at the absolute path of
  the Julia that built it, which runs on that machine only. `JuliaStep(bundle = true)` copies
  the runtime (about 120 MB of libraries) next to the plugin — `Name.clap.runtime/` beside a
  Linux `.clap`, `Contents/Resources/julia/` inside a macOS bundle — with a relative rpath, and
  that is the relocatable form.
- **Windows has no rpath**, and the loader resolves a DLL's imports from the host executable's
  directory, the system directories and `PATH` — never from the DLL's own directory unless it
  was opened with `LOAD_WITH_ALTERED_SEARCH_PATH`, which a DAW does not do. So on Windows a
  Julia step is always bundled, the plugin DLL and the runtime go together into
  `Name.clap.runtime\bin`, and `Name.clap` is a small shim (`csrc/clap_forward_shim.c`, no
  Julia in it) whose `clap_entry` loads the real plugin from beside itself with that flag, puts
  the same directory at the front of the process `PATH` for the libraries Julia opens by name
  at start-up, and forwards `get_factory` and `deinit`. The test suite hosts such a bundle with
  nothing of Julia's on `PATH`.
- **One runtime per process, unless privatised.** Two juliac plugins in the same host would
  share, and fight over, one `libjulia`. `JuliaStep(bundle = true, privatize = true)` salts
  the bundled runtime's library names and symbol versions (JuliaC's `--privatize`), so each
  plugin loads its own; `test/export/probe_two.c` loads two such plugins into one process
  and runs audio through both. JuliaC salts on Linux and macOS only, so on Windows
  `privatize` is refused with a pointer to the upstream issue
  ([JuliaC.jl #186](https://github.com/JuliaLang/JuliaC.jl/issues/186)): two juliac plugins in
  one Windows host would still share, and fight over, one `libjulia.dll`. A juliac plugin
  still cannot be hosted from inside the Julia process that built it: the test suite hosts
  them from C probes in a separate process, which is also the public CI story.
- **Realtime.** JuliaC disables Julia's signal handlers and pins the runtime to one thread
  for a library, and an isbits step allocates nothing, but the garbage collector still exists
  in the audio callback. A C step has no such caveat.

The wrapper also implements `clap.state`, so a DAW session reloads with the parameter
values it was saved with: a small little-endian blob of `(id, value)` pairs, checked by
`test/export/probe_state.c`, a minimal host that saves, loads into a fresh instance, and
offers garbage.

An output on a clock slower than the sample clock is declared with `sub_clock = true`: the
output struct then also carries a `bool has_<output>` presence flag, and on samples where it
is false the wrapper holds the last present value per channel. The phase lives in the step's
own state, so it carries across blocks like everything else.

Not yet: privatised (coexisting) Julia steps on Windows, see above; Julia-step bundling is
tested on Linux and Windows, not on macOS.

## LV2: what discovery gives you, and what the host refuses

LV2 metadata lives in Turtle manifests next to the binary, and this host reads them
through [lilv](https://gitlab.com/lv2/lilv), the reference reader, rather than a
hand-rolled parser that could silently mis-map a port. A plugin is named by URI;
`lv2_params()` returns its control input ports (id = port index, name, symbol, range,
default) as read from the manifest; latency comes from the plugin's designated
`lv2:latency` port. Search paths go through `lv2_default_path(dirs...)`, which appends
the LV2 specification bundles from `lv2_jll` so lilv has the vocabulary to classify what
it finds.

The host offers four features (`urid:map`, `urid:unmap`, `bufsz:fixedBlockLength`,
`bufsz:boundedBlockLength`) and connects audio and control ports. A plugin that
*requires* anything else — an atom, CV or event port, or another host feature — is
refused at `lv2_open!` with a message that says which. An unconnected required port is
undefined behaviour in the LV2 specification, so refusing is the honest answer; plugins
with only optional extras open fine. Parameter changes are written to the control port
the plugin reads at `run()`, so a change lands on exactly the block it is passed with.

## Known limits

- **Third-party binary code runs in-process.** A plugin that segfaults takes the Julia
  process down with it. Out-of-process hosting is the robust answer and is a much larger
  project; in-process is fine for offline work, and that is what this is for.
- **Realtime discipline is not provided.** CLAP asks a host to keep an audio thread that
  never blocks and never allocates. A garbage-collected process driving a solver that may
  retry a step cannot promise that. Harmless offline; not harmless on a live capture with a
  deadline.
- **Reported latency is surfaced, not compensated, by default.** `clap_open!` accepts
  `compensate_latency = true`, an opt-in Julia-side mode that aligns the stream:
  `clap_out` then returns block k aligned with input block k and `clap_flush!` yields
  the tail. `lv2_open!` has no such option: `lv2_latency()` reports what the plugin
  writes to its `lv2:latency` port and aligning the stream is the caller's job. The C
  hosts (`csrc/`) stay uncompensated — a generated C program linking them directly never
  sees the mode.

## Licence

MIT. Vendored headers under `csrc/vendor/` carry their own permissive licences (CLAP: MIT;
LV2: ISC) — see `LICENSE`.
