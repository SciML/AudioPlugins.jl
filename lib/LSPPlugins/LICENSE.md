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

Installing this package pulls in `LSPPlugins_jll`, whose binaries are licensed
**LGPL-3.0-or-later**, and `using LSPPlugins` loads them into your Julia process. An MIT
wrapper is what the LGPL is for — nothing is linked statically, the module is loaded at
runtime, and the artifact can be replaced — but the terms that govern the binaries are
still theirs, not this file's: if you distribute a work that loads these plugins, honour
the LGPL for the LSP part of it, in particular the recipient's right to relink against
their own build.

Per-component licences of what the artifact contains, read from the
`lsp-plugins-src-1.2.35.tar.gz` release tarball:

| Component | Licence | Evidence |
|---|---|---|
| `lsp-plugins` and all 65 modules under `modules/` | LGPL-3.0-or-later | every module carries both `COPYING` (GPLv3) and `COPYING.LESSER` (LGPLv3); headers say "GNU Lesser General Public License … either version 3 of the License, or any later version"; `README.md` states LGPLv3 |
| `modules/lsp-3rd-party/include/steinberg/` — 128 files | GPL-3.0 | LSP's own clean-room VST3 interface headers. **Not compiled**: the recipe enables only the CLAP wrapper |
| `modules/lsp-3rd-party/include/clap/` | MIT | the CLAP headers |
| libsndfile, linked for the sampler and impulse-response plugins | LGPL-2.1-or-later | `libsndfile_jll` |

The artifact carries `COPYING.LESSER` and `COPYING` itself; the Yggdrasil recipe installs
them with `install_license`.
