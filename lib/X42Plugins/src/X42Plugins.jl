"""
    X42Plugins

Robin Gareus' [x42-plugins](https://github.com/x42/x42-plugins) — fourteen LV2
bundles, 54 plugins — as a collection for
[AudioPlugins](https://github.com/SciML/AudioPlugins.jl). LV2 collections are
not registered with [`register_bundle!`](@ref) (that API is CLAP-only); point
[`lv2_default_path`](@ref) / [`lv2_scan`](@ref) at [`lv2_dir`](@ref) instead:

```julia
using AudioPlugins, X42Plugins

path = lv2_default_path(lv2_dir())
lv2_scan(path)   # 54 plugins
lv2_open!(path; uri = "http://gareus.org/oss/lv2/nodelay",
          sample_rate = 48000, block_size = 256, channels = 1)
```

!!! warning "MIT package, GPL-2.0-or-later binaries in your process"
    This package is MIT, and covers only the lines of Julia below. The binaries it
    installs and loads — `X42Plugins_jll` — are **GPL-2.0-or-later**, and they run
    inside your Julia process, so the effective terms of the running combination are
    the artifact's rather than this package's: if you distribute a work that loads
    these plugins, GPL-2.0-or-later governs that work.

    Component by component: every source in the fourteen shipped submodules says
    "either version 2 … or (at your option) any later version", and none of those
    trees contain GPL-3.0-or-later files. Not in this artifact: dpl, fat1, meters,
    sisco and zconvo (GPL-3.0-or-later source); darc (sources are v2-or-later, but
    its `COPYING` is GPLv3); meters and sisco also cannot build headless.
    `LICENSE.md` and the README carry the evidence.

    AudioPlugins itself is MIT and depends on none of this — installing AudioPlugins
    does not install x42-plugins, and nothing here is reachable from the MIT core.

`phaserotate` links `libfftw3f` (from `FFTW_jll`, a dependency of the artifact).
`midimap` needs `worker:schedule`, which this host does not provide — it is in
the artifact and `lv2_scan` lists it, but [`lv2_open!`](@ref) refuses it.

Third-party binary code runs in your process: a plugin that crashes takes Julia with it.
See AudioPlugins' "Known limits".
"""
module X42Plugins

using X42Plugins_jll: X42Plugins_jll

export lv2_dir

"""
    lv2_dir() -> String

Absolute path of the directory that contains the fourteen `.lv2` bundles from
`X42Plugins_jll` (i.e. `…/share/lv2`). Pass it to
[`AudioPlugins.lv2_default_path`](@ref) so lilv also sees the LV2 specification
bundles from `lv2_jll`.
"""
function lv2_dir()
    # balance_lv2 is …/share/lv2/balance.lv2/manifest.ttl
    return dirname(dirname(X42Plugins_jll.balance_lv2))
end

end # module
