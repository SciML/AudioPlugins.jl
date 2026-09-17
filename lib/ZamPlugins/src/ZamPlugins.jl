"""
    ZamPlugins

Damien Zammit's [zam-plugins](https://github.com/zamaudio/zam-plugins) — sixteen of them,
as CLAP modules — registered with
[AudioPlugins](https://github.com/SciML/AudioPlugins.jl) so that each is openable by id:

```julia
using AudioPlugins, ZamPlugins

plugins(ZamPlugins_jll)          # all sixteen, with names
clap_open!("com.zamaudio.ZamComp"; sample_rate = 48000, block_size = 256, channels = 2)
```

!!! warning "MIT package, GPL-2.0-or-later binaries in your process"
    This package is MIT, and covers only the lines of Julia below. The binaries it
    installs and loads — `ZamPlugins_jll` — are **GPL-2.0-or-later**, and they run inside
    your Julia process, so the effective terms of the running combination are the
    artifact's rather than this package's: if you distribute a work that loads these
    plugins, GPL-2.0-or-later governs that work.

    Component by component: all 81 plugin sources say "either version 2 of the License,
    or (at your option) any later version", and DPF's CLAP target is MIT. `LICENSE.md`
    and the README carry the evidence, and the reason `ZamVerb` and `ZamHeadX2` are
    absent: they link GPL-3.0-or-later zita-convolver, which would make the collection's
    single label untrue.

    AudioPlugins itself is MIT and depends on none of this — installing AudioPlugins does
    not install zam-plugins, and nothing here is reachable from the MIT core.

Sixteen of the nineteen upstream builds by default. `ZamVerb` and `ZamHeadX2` are left out
because they link zita-convolver 4.0.0, which is GPL-3.0-**or-later** and would make those
two binaries more restrictive than the rest of the collection; `ZamNoise` is left out
because it is the only one that needs FFTW. Each is its own CLAP module, so all sixteen
are registered separately and listed together.

Several of these take a sidechain as a second audio input, so the `channels` to open one
with is the number of *host* channels, not the number of signal channels: `ZamComp` is
mono-with-sidechain and wants `channels = 2`. `ZamCompX2` and `ZamGateX2` want three audio
inputs — left, right and sidechain — which is past this host's two-channel limit
(`CLAP_HOST_MAX_CHAN`); they are in the artifact and usable by a host that is not so
limited, but `clap_open!` here cannot feed them.

Third-party binary code runs in your process: a plugin that crashes takes Julia with it.
See AudioPlugins' "Known limits".
"""
module ZamPlugins

using AudioPlugins: register_bundle!
using ZamPlugins_jll: ZamPlugins_jll

# The registry is runtime state, so this is `__init__` rather than top level: a path
# baked in at precompile time would not survive a relocated depot.
function __init__()
    for clap in (
            ZamPlugins_jll.zam_autosat_clap,
            ZamPlugins_jll.zam_comp_clap,
            ZamPlugins_jll.zam_comp_x2_clap,
            ZamPlugins_jll.zam_delay_clap,
            ZamPlugins_jll.zam_dynamic_eq_clap,
            ZamPlugins_jll.zam_echo_clap,
            ZamPlugins_jll.zam_eq2_clap,
            ZamPlugins_jll.zam_gate_clap,
            ZamPlugins_jll.zam_gate_x2_clap,
            ZamPlugins_jll.zam_geq31_clap,
            ZamPlugins_jll.zam_grains_clap,
            ZamPlugins_jll.zam_maxim_x2_clap,
            ZamPlugins_jll.zam_phono_clap,
            ZamPlugins_jll.zam_tube_clap,
            ZamPlugins_jll.zam_multi_comp_clap,
            ZamPlugins_jll.zam_multi_comp_x2_clap,
        )
        register_bundle!(clap; source = ZamPlugins_jll)
    end
    return nothing
end

end # module
