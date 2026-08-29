# AudioPlugins.jl

Headless hosting of third-party audio plugins from Julia, behind a C ABI of scalar doubles.

```julia
using AudioPlugins
bundle = clap_test_bundle()            # builds the test plugins shipped here
clap_open!(bundle; plugin_id = "ap.gain", sample_rate = 48000, block_size = 64)

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

The only thing that needs a C compiler is building the *test* plugins
(`clap_test_bundle()`, `lv2_test_bundle()`), and those go into a per-package
scratch space, so a read-only installation hosts plugins fine and fails only at
test time, with a message that says so. The C sources are also compiled and
probed directly in CI (`.github/workflows/CProbe.yml`), with no Julia involved.

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
`test/plugins/ap_test_lv2.c` + `ap_test_lv2.ttl` the same three plugins as an LV2
bundle: a gain, a one-pole filter, and a 16-sample lookahead. Hosting is only proved by
hosting something, and depending on a third-party plugin would make the suite rest on a
binary whose arithmetic cannot be checked and may not even be fetchable. Because these
are ours, every expectation is arithmetic rather than a recording:

- `ap.gain` — output equals input times the gain, sample-exactly;
- `ap.onepole` — two consecutive blocks equal one continuous run over the concatenated
  input, which is what proves state survives block boundaries;
- `ap.lookahead` — reported latency is real and is surfaced rather than silently absorbed.

The LV2 suite additionally scans the machine's system LV2 directories when it has any
(`/usr/lib/lv2` and friends) and asserts that lilv enumerates what is installed.

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
- **Reported latency is surfaced, not compensated.** Callers that care must align the
  stream themselves.

## Licence

MIT. Vendored headers under `csrc/vendor/` carry their own permissive licences (CLAP: MIT;
LV2: ISC) — see `LICENSE`.
