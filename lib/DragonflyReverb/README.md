# DragonflyReverb.jl

Michael Willis' [Dragonfly Reverb](https://michaelwillis.github.io/dragonfly-reverb/) —
hall, room, plate and early reflections — as a plugin collection for
[AudioPlugins.jl](https://github.com/SciML/AudioPlugins.jl). **This package is MIT; the
binaries it installs and loads are `GPL-3.0-or-later`.** Those binaries run inside your
Julia process, so the effective terms of the running combination are the artifact's, not
this package's: if you distribute a work that loads these reverbs, GPL-3.0-or-later
governs that work. The MIT licence covers only the few lines of Julia that register the
bundle.

```julia
using AudioPlugins, DragonflyReverb

plugins(DragonflyReverb_jll)   # the four reverbs
clap_open!("michaelwillis.dragonfly.hall"; sample_rate = 48000, block_size = 256, channels = 2)
```

## Licences, component by component

Read from the dragonfly-reverb source tree at tag `3.2.10`:

| Component | Licence | Evidence |
|---|---|---|
| this package (`src/`, `test/`) | MIT | `LICENSE.md` |
| Dragonfly's own plugin and DSP sources | GPL-3.0-or-later | `LICENSE` is the GPLv3 text; source headers say "version 3 of the License, or any later version" |
| `common/freeverb/` — Freeverb3, Teru Kamogashira | GPL-2.0-or-later | `common/freeverb/COPYING` is the GPLv2 text; headers say "version 2 … or (at your option) any later version" |
| `common/kiss_fft/` | BSD-3-Clause | `common/kiss_fft/COPYING.txt` |
| `dpf/` — DISTRHO Plugin Framework | ISC | `dpf/LICENSE`. The CLAP target is MIT; DPF's LGPL-2.1-or-later files are the LADSPA and DSSI headers plus `DistrhoPluginJACK.cpp`, none of which a CLAP-only build compiles |

GPL-2.0-or-later combines with GPL-3.0-or-later as GPL-3.0-or-later, which is what
`DragonflyReverb_jll` as a whole is. The artifact carries those licence files itself.

## Opt-in, and that is the point

AudioPlugins itself is MIT and depends on none of this. Installing AudioPlugins does not
install Dragonfly Reverb, nothing in AudioPlugins' `Project.toml` mentions it, and the MIT
core never reaches this package — which is the entire reason a plugin collection is a
separate package rather than an extension of the host. Installing *this* package is what
puts a GPL binary on your machine, and that is a decision you make by name.

## What the JLL contains

Four CLAP modules, one per reverb, built headless: `HAVE_OPENGL=false` takes DPF down to
`UI_TYPE=none`, so the artifact has no GUI and links no X11 or OpenGL. The recipe is
`D/DragonflyReverb` in Yggdrasil.
