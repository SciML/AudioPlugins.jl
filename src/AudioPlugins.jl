"""
    AudioPlugins

Headless hosting of third-party audio plugins from Julia, behind a C ABI of
scalar doubles.

Three formats: **CLAP** (MIT, header-only), **LV2** (ISC) and **VST3** (MIT
since SDK 3.8) — see the README for what is implemented in each. The host is deliberately a thin C layer
with a plain `ccall` surface rather than an idiomatic Julia API, because its
first consumer is a synchronous modelling compiler that requires node-side
operators to be *named* `ccall`s into a shared library at a compile-time
constant path. That constraint costs a Julia caller nothing and is what lets the
same host serve a generated C program with no Julia present.

The processing contract, which every plugin format shares:

  * a contiguous block of samples in, a block out;
  * block size fixed at setup;
  * plugin state persisting across blocks;
  * scalar parameters that may change per block.

Headless only: no plugin GUI is ever loaded.

The hosts are shipped prebuilt by `CLAPHost_jll` and `VST3Host_jll`; the
sources under `csrc/` stay in the package for a generated program to link
directly.
"""
module AudioPlugins

include("clap_io.jl")
include("vst3_io.jl")

end # module
