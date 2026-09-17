# LSPPlugins.jl

The [Linux Studio Plugins](https://lsp-plug.in/) suite — 198 plugins in one CLAP module —
as a plugin collection for [AudioPlugins.jl](https://github.com/SciML/AudioPlugins.jl).
**This package is MIT; the binaries it installs and loads are `LGPL-3.0-or-later`.** Those
binaries run inside your Julia process. An MIT wrapper is exactly what the LGPL is for —
nothing is linked statically and the artifact can be replaced — but the terms that govern
the binaries are still theirs: if you distribute a work that loads these plugins, honour
the LGPL for the LSP part of it, in particular the recipient's right to relink against
their own build.

```julia
using AudioPlugins, LSPPlugins

plugins(LSPPlugins_jll)   # all 198, with names
clap_open!("in.lsp-plug.compressor_stereo"; sample_rate = 48000, block_size = 256, channels = 2)
```

The most complete conventional suite in the collection: 8-, 16- and 32-band parametric
equalisers, graphic equalisers, the compressor / expander / gate / limiter / clipper
family in mono, stereo, left-right and mid-side variants and with sidechain and multiband
versions of each, crossovers, convolution reverb and impulse responses, delays, samplers,
and the analysis plugins.

## Licences, component by component

Read from the `lsp-plugins-src-1.2.35.tar.gz` release tarball:

| Component | Licence | Evidence |
|---|---|---|
| this package (`src/`, `test/`) | MIT | `LICENSE.md` |
| `lsp-plugins` and all 65 modules under `modules/` | LGPL-3.0-or-later | every module carries both `COPYING` (GPLv3) and `COPYING.LESSER` (LGPLv3); file headers say "GNU Lesser General Public License … either version 3 of the License, or any later version"; `README.md` states LGPLv3 |
| `modules/lsp-3rd-party/include/steinberg/` — 128 files | GPL-3.0 | LSP's own clean-room VST3 interface headers — **not compiled**, because this build enables only the CLAP wrapper |
| `modules/lsp-3rd-party/include/clap/` | MIT | the CLAP headers |
| libsndfile, linked for the sampler and impulse-response plugins | LGPL-2.1-or-later | `libsndfile_jll` |

The recipe installs `COPYING.LESSER` and `COPYING` into the artifact.

## Opt-in, and that is the point

AudioPlugins itself is MIT and depends on none of this. Installing AudioPlugins does not
install LSP, nothing in AudioPlugins' `Project.toml` mentions it, and the MIT core never
reaches this package — which is the entire reason a plugin collection is a separate
package rather than an extension of the host. Installing *this* package is what puts an
LGPL binary on your machine, and that is a decision you make by name.

## What the JLL contains

One `lsp-plugins.clap` module, built from source with `FEATURES=clap`. That drops LSP's
`ui` feature, which is what keeps X11, cairo, freetype, fontconfig and the GL stack out
of the artifact, and drops the LADSPA/LV2/VST2/VST3 wrappers and the standalone
JACK/PipeWire hosts. The recipe is `L/LSPPlugins` in Yggdrasil.
