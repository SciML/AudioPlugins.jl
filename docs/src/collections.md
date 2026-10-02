# Plugin collections

A *collection* is many plugins in one module: CLAP's factory indexes several plugins per
`.clap`, so a suite of five hundred effects is one file and one artifact. A collection
reaches Julia as a JLL, and a small sublibrary package wraps that JLL and calls
[`register_bundle!`](@ref) with the path it exposes. From then on every plugin in it is
openable by its id alone.

The mechanism is described in
[Plugin collections: the bundle registry](@ref) — this page is about *which* collections
there are, what they are licensed under, and how to add one.

## Nothing ships by default

No collection is a dependency of this package, and none is downloaded. On a fresh
install [`plugins`](@ref) returns an empty vector and [`clap_open!`](@ref) behaves as it
always did: name a bundle path, get that bundle. A collection is opt-in — you install a
package by name and `using` it — and that is a licensing decision rather than a packaging
preference, for the reason given under
[What loading a collection in your process means](@ref).

## What exists today

| Collection | Upstream | Upstream licence | Status |
|---|---|---|---|
| Airwindows — 504 effects | [baconpaul/airwin2rack](https://github.com/baconpaul/airwin2rack), `airwin-registry` target | MIT | Shipped: `Airwindows_jll` 1.0.0 is registered and `lib/Airwindows` is in this repository |
| Pitch shift / time stretch | [signalsmith-stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) | MIT | Proposed — a DSP library, so it needs an adapter before it is a plugin at all |
| Convolution | [HiFi-LoFi/FFTConvolver](https://github.com/HiFi-LoFi/FFTConvolver) | MIT | Proposed — likewise a library |
| LSP Plugins — 198 plugins | [lsp-plugins](https://github.com/lsp-plugins/lsp-plugins) | LGPL-3.0-or-later | Shipped: `LSPPlugins_jll` 1.2.35 is registered and `lib/LSPPlugins` is in this repository. **Linux glibc only** (`x86_64` and `i686`), so it does not resolve on macOS or Windows |
| Dragonfly Reverb | [dragonfly-reverb](https://github.com/michaelwillis/dragonfly-reverb) | GPL-3.0-or-later | Shipped: `DragonflyReverb_jll` 3.2.10 is registered and `lib/DragonflyReverb` is in this repository |
| ZamPlugins | [zam-plugins](https://github.com/zamaudio/zam-plugins) | GPL-2.0-or-later for the sixteen plugins packaged; the upstream repository is not uniformly so (see below) | Shipped: `ZamPlugins_jll` 4.5.0 is registered and `lib/ZamPlugins` is in this repository |
| x42-plugins | [x42-plugins](https://github.com/x42/x42-plugins) | GPL-2.0-or-later for the fourteen submodules packaged; the meta-repo is not uniformly so (see below) | Shipped: `X42Plugins_jll` and `lib/X42Plugins` — 54 headless LV2 plugins. LV2 collections are not `register_bundle!`'d; use `lv2_default_path(X42Plugins.lv2_dir())` |
| x42-plugins, GPL-3.0-or-later part | [darc](https://github.com/x42/darc.lv2), [dpl](https://github.com/x42/dpl.lv2), [fat1](https://github.com/x42/fat1.lv2), [zconvo](https://github.com/x42/zconvo.lv2) | GPL-3.0-or-later (see below) | Shipped: `X42Darc_jll`, `X42Dpl_jll`, `X42Fat1_jll`, `X42Zconvo_jll` and `lib/X42PluginsGPL3` — 13 headless LV2 plugins, 7 of which this host opens. Use `X42PluginsGPL3.lv2_path()` |

"Proposed" means exactly that: no recipe, no JLL, no sublibrary, and no commitment that
one is coming. It is the list from
[issue #39](https://github.com/SciML/AudioPlugins.jl/issues/39), kept here so that the
tiers are visible in one place. Which plugin formats each of those upstreams actually
builds is not recorded here, because it has not been established — several are DPF-based
and build more than one. It matters when one is packaged, for the reason in
[The registry is CLAP-only](@ref).

The licence column is what each upstream repository declares for the repository as a
whole, read on 2026-09-17. It is a starting point, not an answer about any particular
build: what an artifact is licensed under depends on which targets a recipe compiles and
what it links, which is why the Airwindows row is MIT even though its upstream
repository also contains GPL3 material. **When a JLL exists, the JLL is the authority**
— read its licence, not this table.

Every identifier above is spelled `-or-later` where the upstream headers say "either
version N ... or (at your option) any later version". This is not pedantry:
`GPL-3.0-only` and `GPL-3.0-or-later` are materially different grants, and the bare
`GPL-3.0` form is deprecated in SPDX precisely because it does not say which is meant.

Three entries in issue #39's own table do not match what upstream declares, and are
corrected above rather than propagated. lsp-plugins is LGPL-3.0-or-later (its README
says "GNU Lesser Public License v3"), not GPLv3. x42-plugins declares no
repository-level licence at all, being a meta-repository of per-plugin submodules: at
pin `3fb6abe`, fourteen headless submodules are GPL-2.0-or-later throughout (what is
packaged here); five contain GPL-3.0-or-later source (dpl, fat1, meters, sisco,
zconvo); darc's sources are GPL-2.0-or-later but its `COPYING` is GPLv3, so it is
excluded on that basis; and four more are GPL-2.0-or-later but not headless (fil4,
tuna, spectra, mixtri). No single label is correct for the whole meta-repo — see the
X42Plugins section below. And zam-plugins is
GPL-2.0-or-later only for what is packaged here: `ZamVerb` and `ZamHeadX2` link a
bundled zita-convolver 4.0.0 which is GPL-3.0-or-later, so both are deliberately
excluded from the JLL to keep the single label true rather than approximate.

### X42Plugins

Fourteen LV2 bundles from [x42-plugins](https://github.com/x42/x42-plugins) at pin
`3fb6abe`, built headless: balance, controlfilter, matrixmixer, mididebug,
midifilter, midigen, midimap, nodelay, onsettrigger, phaserotate, stepseq,
stereoroute, testsignal, xfade — **54** plugins. Ids are Robin Gareus' own,
`http://gareus.org/oss/lv2/…`. LV2 collections do not use
[`register_bundle!`](@ref); expose the directory and scan it:

```julia
using AudioPlugins, X42Plugins

path = lv2_default_path(lv2_dir())
lv2_scan(path)             # 54
lv2_open!(path; uri = "http://gareus.org/oss/lv2/nodelay",
          sample_rate = 48000, block_size = 256, channels = 1)
```

`phaserotate` needs `libfftw3f` (pulled in by the JLL via `FFTW_jll`). `midimap`
needs `worker:schedule`, which this host does not provide — it is in the artifact
and enumerated by `lv2_scan`, but `lv2_open!` refuses it (53 of 54 open).

Not in `X42Plugins_jll`: meters and sisco (GPL-3.0-or-later source; also need
cairo/OpenGL); and the GPL-2.0-or-later plugins that do not build headless (fil4,
tuna, spectra, mixtri). darc, dpl, fat1 and zconvo ship under
[X42PluginsGPL3](@ref X42PluginsGPL3-section) below.

### [X42PluginsGPL3](@id X42PluginsGPL3-section)

The four x42-plugins submodules whose binaries are GPL-3.0-or-later, from the same pin
`3fb6abe`, built headless: darc (compressor), dpl (digital peak limiter), fat1
(autotune) and zconvo (zero-latency convolver) — **13** plugins. They ship as four
JLLs (`X42Darc_jll`, `X42Dpl_jll`, `X42Fat1_jll`, `X42Zconvo_jll`) and one sublibrary
so that `X42Plugins` stays GPL-2.0-or-later. Each JLL has its own `share/lv2`;
[`lv2_path`](@ref) joins them (plus the LV2 specification bundles) for the host.

```julia
using AudioPlugins, X42PluginsGPL3

path = lv2_path()
lv2_scan(path)             # 13
lv2_open!(path; uri = "http://gareus.org/oss/lv2/dpl#stereo",
          sample_rate = 48000, block_size = 256, channels = 2)
```

Why each is GPL-3.0-or-later: darc's `COPYING` is the GPLv3 text (its headers say
v2-or-later); dpl compiles Fons Adriaensen's v3-or-later `peaklim`; fat1 compiles
zita-resampler (v3-or-later) although its `COPYING` is GPLv2; zconvo compiles
`zeta-convolver`, a modified zita-convolver (v3-or-later), although its `COPYING` is
GPLv2. fat1 and zconvo link `libfftw3f` (`FFTW_jll`); zconvo also links `libsndfile`
(LGPL-2.1-or-later) and `libsamplerate` (BSD-2-Clause).

darc, dpl and fat1 (seven plugins) open and process audio. The six zconvo plugins
require `worker:schedule` and `options:options`, which this host does not provide:
`lv2_scan` lists them and `lv2_open!` refuses them.

### Airwindows

Chris Johnson's Airwindows effects — 504 of them in one CLAP module, with ids of the form
`org.airwindows.<effect name>`. What is in this repository today:

  - `csrc/airwindows/airwindows_clap.cpp`, the adapter from airwin2rack's `airwin-registry`
    table to CLAP. It links that target and nothing else: the GPL3 material in that
    repository is its JUCE and Rack front ends, which are not compiled and not linked, so
    the resulting module is MIT end to end.
  - A CI job that builds the adapter against a pinned upstream commit and hosts the result
    through `csrc/clap_host.c`, so the module is known to build and to open.
  - `lib/Airwindows`, the sublibrary — the handful of lines that register the bundle
    on load.

`Airwindows_jll` 1.0.0 is registered, so this runs:

```julia
using AudioPlugins, Airwindows

plugins(Airwindows_jll)             # all 504 of them
clap_open!("org.airwindows.Galactic"; sample_rate = 48000, block_size = 256, channels = 2)
```

## A collection you can run today

The package's own test bundle is a three-plugin CLAP module, and the registry does not
care that it is small. This is the whole flow — register, list, open by id — against
something that needs no third-party artifact:

```julia
using AudioPlugins
const AP = AudioPlugins

plugins()                               # empty — nothing is registered by default

register_bundle!(clap_test_bundle())    # 3
plugins()
# 3-element Vector:
#  (id = "ap.gain",      name = "AudioPlugins Test Gain",      bundle = "…", index = 0, source = nothing)
#  (id = "ap.onepole",   name = "AudioPlugins Test One Pole",  bundle = "…", index = 1, source = nothing)
#  (id = "ap.lookahead", name = "AudioPlugins Test Lookahead", bundle = "…", index = 2, source = nothing)

clap_open!("ap.gain"; sample_rate = 48000, block_size = 64, channels = 1)
clap_plugin_name()                      # "AudioPlugins Test Gain"

tok = clap_fill!(fill(1.0, 64))
y = clap_out(AP.clp_process(tok, 0.0, 0.5, -1, 0, -1, 0, -1, 0))
y[1]                                    # 0.5

clap_close!()
unregister_bundle!(clap_test_bundle())  # true
```

[`clap_test_bundle`](@ref) compiles the bundle, so this is one of the two places a C
compiler is needed; a real collection arrives prebuilt in its JLL and needs none.

## The registry is CLAP-only

[`register_bundle!`](@ref) scans a `.clap` module. A collection packaged as LV2 or VST3
is hosted just as well, but not through the registry: an LV2 search path is a list of
directories rather than one module, and a VST3 bundle is named by path and class id.
Host those with [`lv2_scan`](@ref) / [`lv2_open!`](@ref) and [`vst3_scan`](@ref) /
[`vst3_open!`](@ref), naming the path each time. See
[LV2 discovery goes through lilv](@ref) and [VST3](@ref).

So a collection that is only distributed as LV2 would have to be built as CLAP to reach
[`plugins`](@ref) at all — which is what the Airwindows adapter does, and what makes
packaging a collection more than writing a recipe.

## What loading a collection in your process means

AudioPlugins is MIT, and it ships no plugins. It knows how to `dlopen` a module and call
it, which is a different thing from distributing one. That separation is the reason a
collection is a separate JLL and a separate sublibrary rather than a weak dependency or
an extra in this package: the collections worth bundling are mostly copyleft, and an MIT
package that merely knows how to host them must not make that a transitive obligation of
everyone who installs it.

The consequence for you is that installing a copyleft collection is a decision you make
by name, knowingly, and its terms are then yours to read. A plugin runs **in your Julia
process** — this is `dlopen` and a direct call, not a subprocess — which is what makes
"what did I just load" a question worth asking rather than a formality. That is not legal
advice, and nothing here is: the licence in the JLL is what governs, and how it applies
to what you are building is between you and it.

The same in-process property is also the stability story, and it is unchanged by where a
plugin came from: a collection that crashes takes Julia down with it. See
[Plugins run in-process](@ref).

## Writing a sublibrary for a collection

The extension point is a package, not a hook. A collection sublibrary is:

```julia
module SomeCollection
using AudioPlugins: register_bundle!
using SomeCollection_jll: SomeCollection_jll
__init__() = register_bundle!(SomeCollection_jll.collection_clap; source = SomeCollection_jll)
end
```

Four things are worth getting right, and they are all visible in `lib/Airwindows`:

 1. **Register in `__init__`, not at top level.** The registry is runtime state, and a
    path baked in at precompile time does not survive a relocated depot.
 2. **Pass `source`.** It is the module [`plugins`](@ref) filters on, so
    `plugins(SomeCollection_jll)` can answer for one collection out of several. The
    registry never loads it; it is a label.
 3. **Declare the module a `FileProduct` in the recipe.** A `.clap` is a `.so`/`.dll` on
    Linux and Windows and a bundle directory on macOS, and `FileProduct` covers both.
 4. **Derive ids from something stable.** Airwindows ids come from the effect's name
    rather than its index in the upstream registry, so an insertion upstream cannot
    repoint an existing id at a different effect.

Such a wrapper can live under `lib/` in this repository as its own package, or be a
package of your own; a package that already depends on the JLL for its own reasons can
register from a package extension instead. The registry does not care which — what
matters is that installing AudioPlugins pulls in none of them.

What a sublibrary's tests should assert is the seam rather than the DSP. The arithmetic
of third-party effects is not yours to pin down, but that `using` registers the bundle,
that the ids are what they are claimed to be, and that one known effect opens and changes
the signal, all are. A count assertion is worth having too: it is what catches an
artifact that was truncated on the way through.
