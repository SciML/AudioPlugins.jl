"""
    X42PluginsGPL3

The GPL-3.0-or-later part of Robin Gareus'
[x42-plugins](https://github.com/x42/x42-plugins) — darc (compressor), dpl (digital
peak limiter), fat1 (autotune) and zconvo (zero-latency convolver), four LV2 bundles
and 13 plugins — as a collection for
[AudioPlugins](https://github.com/SciML/AudioPlugins.jl). LV2 collections are not
registered with [`AudioPlugins.register_bundle!`](@ref) (that API is CLAP-only);
pass [`lv2_path`](@ref) to [`AudioPlugins.lv2_scan`](@ref) /
[`AudioPlugins.lv2_open!`](@ref) instead:

```julia
using AudioPlugins, X42PluginsGPL3

path = lv2_path()
lv2_scan(path)   # 13 plugins
lv2_open!(path; uri = "http://gareus.org/oss/lv2/dpl#stereo",
          sample_rate = 48000, block_size = 256, channels = 2)
```

!!! warning "MIT package, GPL-3.0-or-later binaries in your process"
    This package is MIT, and covers only the lines of Julia below. The binaries it
    installs and loads — `X42Darc_jll`, `X42Dpl_jll`, `X42Fat1_jll` and
    `X42Zconvo_jll` — are **GPL-3.0-or-later**, and they run inside your Julia
    process, so the effective terms of the running combination are the artifacts'
    rather than this package's: if you distribute a work that loads these plugins,
    GPL-3.0-or-later governs that work. `LICENSE.md` and the README carry the
    component-by-component evidence.

    AudioPlugins itself is MIT and depends on none of this — installing AudioPlugins
    does not install these plugins, and nothing here is reachable from the MIT core.

fat1 and zconvo link `libfftw3f` (`FFTW_jll`); zconvo also links `libsndfile` and
`libsamplerate`. The six zconvo plugins require `worker:schedule` and
`options:options`, which this host does not provide: they are in the artifacts and
`lv2_scan` lists them, but [`AudioPlugins.lv2_open!`](@ref) refuses them.

Third-party binary code runs in your process: a plugin that crashes takes Julia with it.
See AudioPlugins' "Known limits".
"""
module X42PluginsGPL3

using AudioPlugins: lv2_default_path
using X42Darc_jll: X42Darc_jll
using X42Dpl_jll: X42Dpl_jll
using X42Fat1_jll: X42Fat1_jll
using X42Zconvo_jll: X42Zconvo_jll

export lv2_path

"""
    lv2_path() -> String

LV2 search path that covers the four `.lv2` bundles from `X42Darc_jll`,
`X42Dpl_jll`, `X42Fat1_jll` and `X42Zconvo_jll` (each under its own artifact
`share/lv2`), plus the LV2 specification bundles from `lv2_jll`. Pass the result
to [`AudioPlugins.lv2_scan`](@ref) or [`AudioPlugins.lv2_open!`](@ref).
"""
function lv2_path()
    # Each *_lv2 FileProduct is …/share/lv2/<bundle>/manifest.ttl in its own artifact.
    return lv2_default_path(
        dirname(dirname(X42Darc_jll.darc_lv2)),
        dirname(dirname(X42Dpl_jll.dpl_lv2)),
        dirname(dirname(X42Fat1_jll.fat1_lv2)),
        dirname(dirname(X42Zconvo_jll.zconvo_lv2)),
    )
end

end # module
