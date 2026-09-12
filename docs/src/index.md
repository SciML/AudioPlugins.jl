# AudioPlugins.jl

Headless hosting of third-party audio plugins from Julia, behind a C ABI of scalar
doubles — and, in the other direction, authoring plugins from a per-sample step
function written in C or in Julia.

The package does two things that are inverses of each other:

  - **Hosting.** Load a plugin bundle, activate it at a fixed block size, push blocks of
    samples through it and read the result. See [Hosting a plugin](@ref).
  - **Authoring.** Wrap a step function of the shape a fixed-step code generator emits
    into a plugin bundle a DAW can load. See [Authoring a plugin](@ref).

No plugin GUI is ever loaded, in either direction.

## Installation

```julia
using Pkg
Pkg.add("AudioPlugins")
```

Hosting needs no C toolchain: the host libraries are shipped prebuilt as `CLAPHost_jll`
and `VST3Host_jll`.
A compiler is needed only to build the bundled test plugins ([`clap_test_bundle`](@ref))
and to author your own ([`export_plugin`](@ref)).

## Formats

| Format | Licence | State |
|---|---|---|
| **CLAP** | MIT, header-only | host implemented and tested — discovery, instantiation, parameters, block processing, latency. The one format [`export_plugin`](@ref) builds. |
| **LV2** | ISC | audio path implemented in C (`connect_port` / `run`); discovery is not — see [LV2 discovery](@ref) |
| **VST3** | MIT since SDK 3.8 | host implemented and tested — discovery, instantiation, parameters, block processing, latency. See [VST3](@ref) |

## The processing contract

Every plugin format shares it, which is why one host shape fits all of them:

  - a **contiguous** block of samples in, a block out;
  - block size **fixed at setup**;
  - plugin **state persisting** across blocks;
  - **scalar parameters** that may change per block.

Contiguity is the caller's responsibility. The host knows the block size and the sample
rate, but it cannot see your clock, so it cannot check that you are feeding it every
sample exactly once. A plugin fed a non-contiguous stream returns a perfectly
valid-looking result that simply is not continuous audio.

## Why the API looks like C rather than like Julia

The host is deliberately a thin layer over named `ccall`s into a shared library at a
fixed path, rather than an idiomatic Julia API. Its first consumer is a synchronous
modelling compiler which requires exactly that: a *named* symbol in a library at a
compile-time constant path, taking and returning scalar doubles. A process-local function
pointer, a Julia callback, or a C++ type cannot cross that boundary at all.

The upside for everyone else is that the same host serves a generated standalone C
program with no Julia present, and the cost to an ordinary Julia caller is close to zero:
the functions on this page take and return ordinary Julia values, and only their shape
gives the constraint away.

Two consequences worth knowing before reading further:

  - **The host holds one plugin at a time.** There is no handle to pass around;
    [`clap_open!`](@ref) opens *the* plugin and [`clap_is_open`](@ref) is the whole of
    its lifecycle state. Strings — a bundle path, a plugin id — appear only in the
    lifecycle and discovery functions, never on the processing path.
  - **Parameters are addressed by number, not by name.** A `clap_id` is a `UInt32` and
    every `UInt32` is exactly representable as a `Float64`, so a model can name its own
    parameters with nothing to keep in sync driver-side. [`clap_params`](@ref) is where
    you look the numbers up.

## Contributing

  - Please refer to the
    [SciML ColPrac: Contributor's Guide on Collaborative Practices for Community Packages](https://github.com/SciML/ColPrac/blob/master/README.md)
    for guidance on PRs, issues, and other matters relating to contributing to SciML.

  - See the [SciML Style Guide](https://github.com/SciML/SciMLStyle) for common coding practices and other style decisions.
  - There are a few community forums:
    
      + The #diffeq-bridged and #sciml-bridged channels in the
        [Julia Slack](https://julialang.org/slack/)
      + The #diffeq-bridged and #sciml-bridged channels in the
        [Julia Zulip](https://julialang.zulipchat.com/#narrow/stream/279055-sciml-bridged)
      + On the [Julia Discourse forums](https://discourse.julialang.org)
      + See also [SciML Community page](https://sciml.ai/community/)

## Reproducibility

```@raw html
<details><summary>The documentation of this SciML package was built using these direct dependencies,</summary>
```

```@example
using Pkg # hide
Pkg.status() # hide
```

```@raw html
</details>
```

```@raw html
<details><summary>and using this machine and Julia version.</summary>
```

```@example
using InteractiveUtils # hide
versioninfo() # hide
```

```@raw html
</details>
```

```@raw html
<details><summary>A more complete overview of all dependencies and their versions is also provided.</summary>
```

```@example
using Pkg # hide
Pkg.status(; mode = PKGMODE_MANIFEST) # hide
```

```@raw html
</details>
```

```@eval
using TOML
using Markdown
version = TOML.parse(read("../../Project.toml", String))["version"]
name = TOML.parse(read("../../Project.toml", String))["name"]
link_manifest = "https://github.com/SciML/" * name * ".jl/tree/gh-pages/v" * version *
                "/assets/Manifest.toml"
link_project = "https://github.com/SciML/" * name * ".jl/tree/gh-pages/v" * version *
               "/assets/Project.toml"
Markdown.parse("""You can also download the
[manifest]($link_manifest)
file and the
[project]($link_project)
file.
""")
```
