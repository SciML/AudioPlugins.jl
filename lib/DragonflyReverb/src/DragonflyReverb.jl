"""
    DragonflyReverb

Michael Willis' [Dragonfly Reverb](https://michaelwillis.github.io/dragonfly-reverb/) —
hall, room, plate and early reflections — registered with
[AudioPlugins](https://github.com/SciML/AudioPlugins.jl) so that each is openable by id:

```julia
using AudioPlugins, DragonflyReverb

plugins(DragonflyReverb_jll)     # the four reverbs
clap_open!("michaelwillis.dragonfly.hall"; sample_rate = 48000, block_size = 256, channels = 2)
```

!!! warning "MIT package, GPL-3.0-or-later binaries in your process"
    This package is MIT, and covers only the lines of Julia below. The binaries it
    installs and loads — `DragonflyReverb_jll` — are **GPL-3.0-or-later**, and they run
    inside your Julia process, so the effective terms of the running combination are the
    artifact's rather than this package's: if you distribute a work that loads these
    reverbs, GPL-3.0-or-later governs that work.

    Component by component: Dragonfly's own sources are GPL-3.0-or-later, the bundled
    Freeverb3 DSP is GPL-2.0-or-later, kiss_fft is BSD-3-Clause and DPF is ISC; the first
    two combine as GPL-3.0-or-later. `LICENSE.md` and the README carry the evidence.

    AudioPlugins itself is MIT and depends on none of this — installing AudioPlugins does
    not install Dragonfly Reverb, and nothing here is reachable from the MIT core.

Four separate CLAP modules, not one, because each reverb is its own DPF build; all four
are registered, so [`AudioPlugins.plugins`](@ref) lists them together and
[`AudioPlugins.clap_open!`](@ref) resolves any of their ids.

Third-party binary code runs in your process: an effect that crashes takes Julia with it.
See AudioPlugins' "Known limits".
"""
module DragonflyReverb

using AudioPlugins: register_bundle!
using DragonflyReverb_jll: DragonflyReverb_jll

# The registry is runtime state, so this is `__init__` rather than top level: a path
# baked in at precompile time would not survive a relocated depot.
function __init__()
    for clap in (
            DragonflyReverb_jll.dragonfly_hall_clap,
            DragonflyReverb_jll.dragonfly_room_clap,
            DragonflyReverb_jll.dragonfly_plate_clap,
            DragonflyReverb_jll.dragonfly_early_clap,
        )
        register_bundle!(clap; source = DragonflyReverb_jll)
    end
    return nothing
end

end # module
