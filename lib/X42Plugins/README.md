# X42Plugins.jl

Robin Gareus' [x42-plugins](https://github.com/x42/x42-plugins) — fourteen LV2
bundles, 54 plugins — as a plugin collection for
[AudioPlugins.jl](https://github.com/SciML/AudioPlugins.jl). **This package is MIT;
the binaries it installs and loads are `GPL-2.0-or-later`.** Those binaries run
inside your Julia process, so the effective terms of the running combination are
the artifact's, not this package's. The MIT licence covers only the few lines of
Julia that expose the bundle directory.

LV2 collections do **not** go through `register_bundle!` (that API is
CLAP-only). Point `lv2_default_path` / `lv2_scan` at `lv2_dir`:

```julia
using AudioPlugins, X42Plugins

path = lv2_default_path(lv2_dir())
lv2_scan(path)   # 54 plugins
lv2_open!(path; uri = "http://gareus.org/oss/lv2/nodelay",
          sample_rate = 48000, block_size = 256, channels = 1)
```

## What is in the artifact

Fourteen submodules from x42-plugins pin `3fb6abe`, built headless
(`BUILDOPENGL=no BUILDJACKAPP=no`): balance, controlfilter, matrixmixer,
mididebug, midifilter (33 plugins), midigen, midimap, nodelay (3),
onsettrigger (2), phaserotate (2, needs `libfftw3f`), stepseq (default 8×8
grid as `stepseq_s8n8.lv2`), stereoroute, testsignal, xfade — **54** plugins
in total.

`midimap` requires `worker:schedule`, which AudioPlugins' LV2 host does not
offer; it is in the artifact and listed by `lv2_scan`, but `lv2_open!` refuses
it.

## Licences, component by component

Read from each submodule at the pins of meta-repo `3fb6abe`:

| Component | Licence | Evidence |
|---|---|---|
| this package (`src/`, `test/`) | MIT | `LICENSE.md` |
| the fourteen shipped submodules | GPL-2.0-or-later | each `COPYING` is the GPLv2 text; headers say "either version 2 … or (at your option) any later version"; no version-3 source files |
| `FFTW_jll` (runtime dep of `phaserotate`) | GPL-2.0-or-later | FFTW |

## Not packaged here

- **GPL-3.0-or-later source:** dpl (`peaklim`), fat1 (`resampler*`), meters /
  sisco (`zita-resampler`), zconvo (`zeta-convolver`). meters and sisco also
  cannot build headless (cairo / OpenGL).
- **darc:** every source header is GPL-2.0-or-later; its `COPYING` is the GPLv3
  text, so it is excluded on that basis.
- **GPL-2.0-or-later but not headless:** fil4 and tuna (cairo in DSP), spectra
  (OpenGL mandatory), mixtri (libltc).

dpl (limiter), fat1 (autotune), darc (compressor) and zconvo (convolver) are
packaged separately, under GPL-3.0-or-later, as `X42PluginsGPL3`
(`lib/X42PluginsGPL3`).

## Opt-in, and that is the point

AudioPlugins itself is MIT and depends on none of this. Installing AudioPlugins
does not install x42-plugins. Installing *this* package is what puts GPL
binaries on your machine.

## What the JLL contains

Fourteen LV2 bundles under `share/lv2/`, built headless. The recipe is
`X/X42Plugins` in Yggdrasil.
