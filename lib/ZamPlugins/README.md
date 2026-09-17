# ZamPlugins.jl

Damien Zammit's [zam-plugins](https://github.com/zamaudio/zam-plugins) — sixteen of them —
as a plugin collection for
[AudioPlugins.jl](https://github.com/SciML/AudioPlugins.jl). **This package is MIT; the
binaries it installs and loads are `GPL-2.0-or-later`.** Those binaries run inside your
Julia process, so the effective terms of the running combination are the artifact's, not
this package's: if you distribute a work that loads these plugins, GPL-2.0-or-later
governs that work. The MIT licence covers only the few lines of Julia that register the
bundles.

```julia
using AudioPlugins, ZamPlugins

plugins(ZamPlugins_jll)   # all sixteen, with names
clap_open!("com.zamaudio.ZamComp"; sample_rate = 48000, block_size = 256, channels = 2)
```

Compressors — `ZamComp`, `ZamCompX2`, `ZaMultiComp`, `ZaMultiCompX2`, `ZamDynamicEQ` — the
`ZaMaximX2` limiter, `ZamGate`/`ZamGateX2`, `ZamEQ2` and the `ZamGEQ31` graphic EQ,
`ZamDelay` and `ZamEcho`, `ZamTube` and `ZamPhono` emulation, `ZamAutoSat`, `ZamGrains`.

Several take a sidechain as a second audio input, so the `channels` you open one with is
the number of *host* channels rather than signal channels — `ZamComp` is
mono-with-sidechain and wants `channels = 2`. `ZamCompX2` and `ZamGateX2` want three audio
inputs (left, right, sidechain), past AudioPlugins' two-channel `CLAP_HOST_MAX_CHAN`
limit; they are in the artifact and usable by a host that is not so limited, but
`clap_open!` here cannot feed them. The other fourteen are opened and driven by this
package's test suite.

## Licences, component by component

Read from the zam-plugins source tree at tag `4.5`:

| Component | Licence | Evidence |
|---|---|---|
| this package (`src/`, `test/`) | MIT | `LICENSE.md` |
| all 81 plugin sources built into the artifact | GPL-2.0-or-later | headers say "either version 2 of the License, or (at your option) any later version"; `COPYING` is the GPLv2 text |
| the 37 generated artwork files beside them | no notice of their own | they inherit `COPYING`, so GPL-2.0-or-later |
| `dpf/` — DISTRHO Plugin Framework | ISC, with an MIT CLAP target | `dpf/LICENSING.md`, `NOTICE.DPF`. DPF's LGPL-2.1-or-later files are the LADSPA and DSSI headers plus `DistrhoPluginJACK.cpp`, none compiled by a CLAP-only build |

## Three plugins are deliberately not here

Upstream's default set is nineteen. Three are excluded, and the first exclusion is what
keeps the `GPL-2.0-or-later` label above true rather than approximate:

- **`ZamVerb` and `ZamHeadX2`** link the bundled zita-convolver 4.0.0, which is
  GPL-3.0-**or-later** (`lib/zita-convolver-4.0.0/zita-convolver.h`: "either version 3 of
  the License, or (at your option) any later version"). Those two binaries would be
  GPL-3.0-or-later while everything else here is GPL-2.0-or-later, and shipping them in a
  collection labelled GPL-2.0-or-later would misstate what a user has. The test suite
  asserts their absence, so this stays a property of the collection rather than an
  accident of some future build.
- **`ZamNoise`** needs FFTW. That is licence-compatible — FFTW is GPL-2.0-or-later — but
  it is the only plugin that would pull a numerical dependency into the artifact, so it is
  a separate decision rather than a silent one.

`ZamChild670`, `ZamPiano`, `ZamSFZ` and `ZamSynth` are not in upstream's default `PLUGINS`
list and are not built here either. (`ZamSFZ` would additionally need Rubberband, which is
GPL-2.0-or-later *or commercial* — a dual-licensing arrangement worth a deliberate look
before anyone adds it.)

## Opt-in, and that is the point

AudioPlugins itself is MIT and depends on none of this. Installing AudioPlugins does not
install zam-plugins, nothing in AudioPlugins' `Project.toml` mentions it, and the MIT core
never reaches this package — which is the entire reason a plugin collection is a separate
package rather than an extension of the host. Installing *this* package is what puts GPL
binaries on your machine, and that is a decision you make by name.

## What the JLL contains

Sixteen CLAP modules, one per plugin, built headless: `HAVE_OPENGL=false` takes DPF down
to `UI_TYPE=none`, so the artifacts have no GUI and link no X11 or OpenGL. The recipe is
`Z/ZamPlugins` in Yggdrasil.
