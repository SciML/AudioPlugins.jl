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
is the handful of lines that register a bundle with AudioPlugins. It covers no binary.

Installing this package pulls in `DragonflyReverb_jll`, whose binaries are licensed
**GPL-3.0-or-later**, and `using DragonflyReverb` loads them into your Julia process. The
effective terms of the running combination are the artifact's, not this file's: if you
distribute a work that loads these reverbs, GPL-3.0-or-later governs that work.

Per-component licences of what the artifact contains, read from the dragonfly-reverb
source tree at tag `3.2.10`:

| Component | Licence | Evidence |
|---|---|---|
| Dragonfly's own plugin and DSP sources | GPL-3.0-or-later | `LICENSE` is the GPLv3 text; source headers say "version 3 of the License, or any later version" |
| `common/freeverb/` — Freeverb3, Teru Kamogashira | GPL-2.0-or-later | `common/freeverb/COPYING` is the GPLv2 text; headers say "version 2 … or (at your option) any later version" |
| `common/kiss_fft/` | BSD-3-Clause | `common/kiss_fft/COPYING.txt` |
| `dpf/` — DISTRHO Plugin Framework | ISC | `dpf/LICENSE`; the CLAP target DPF builds here is MIT, and its LGPL-2.1-or-later files are the LADSPA/DSSI headers, which a CLAP-only build never compiles |

GPL-2.0-or-later combines with GPL-3.0-or-later as GPL-3.0-or-later, which is what the
artifact as a whole is. The artifact carries those licence files itself; the Yggdrasil
recipe installs them with `install_license`.
