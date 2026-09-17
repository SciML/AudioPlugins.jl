MIT License

Copyright (c) 2026 JuliaHub, Inc. and contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

---

## What this licence covers, and what it does not

The MIT licence above covers **this package only**: the Julia source under `src/`, which
is the handful of lines that register sixteen bundles with AudioPlugins. It covers no
binary.

Installing this package pulls in `ZamPlugins_jll`, whose binaries are licensed
**GPL-2.0-or-later**, and `using ZamPlugins` loads them into your Julia process. The
effective terms of the running combination are the artifact's, not this file's: if you
distribute a work that loads these plugins, GPL-2.0-or-later governs that work.

Per-component licences of what the artifact contains, read from the zam-plugins source
tree at tag `4.5`:

| Component | Licence | Evidence |
|---|---|---|
| all 81 plugin sources built into the artifact | GPL-2.0-or-later | headers say "either version 2 of the License, or (at your option) any later version"; `COPYING` is the GPLv2 text |
| the 37 generated artwork files beside them | no notice of their own | they inherit `COPYING`, so GPL-2.0-or-later |
| `dpf/` — DISTRHO Plugin Framework | ISC, with an MIT CLAP target | `dpf/LICENSING.md`, `NOTICE.DPF`. DPF's LGPL-2.1-or-later files are the LADSPA and DSSI headers plus `DistrhoPluginJACK.cpp`, none of which a CLAP-only build compiles |

Three of upstream's nineteen default plugins are **not** in the artifact, and one of those
exclusions is what keeps the GPL-2.0-or-later label above true:

  - `ZamVerb` and `ZamHeadX2` link the bundled zita-convolver 4.0.0, which is
    **GPL-3.0-or-later** (`lib/zita-convolver-4.0.0/zita-convolver.h`: "either version 3
    of the License, or (at your option) any later version"). Those two binaries would be
    more restrictive than everything else here, so they are excluded rather than silently
    mixed in.
  - `ZamNoise` needs FFTW. That is licence-compatible — FFTW is GPL-2.0-or-later — but it
    is the only plugin that would pull a numerical dependency into the artifact.

`ZamChild670`, `ZamPiano`, `ZamSFZ` and `ZamSynth` are not in upstream's default `PLUGINS`
list and are not built either. `ZamSFZ` would additionally need Rubberband, which is
GPL-2.0-or-later **or commercial**.

The artifact carries `COPYING` and `NOTICE.DPF` itself; the Yggdrasil recipe installs them
with `install_license`.
