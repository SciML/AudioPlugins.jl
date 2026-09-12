"""
    Airwindows

Chris Johnson's Airwindows effects — 504 of them, in one CLAP module — registered
with [AudioPlugins](https://github.com/SciML/AudioPlugins.jl) so that every one is
openable by id:

```julia
using AudioPlugins, Airwindows

plugins(Airwindows_jll)          # all 504, with names and categories
clap_open!("org.airwindows.Galactic"; sample_rate = 48000, block_size = 256, channels = 2)
```

This is a **sublibrary**: it exists so that the effects are opt-in. AudioPlugins knows
how to host a CLAP module and ships none, and installing this package is what pulls in
the `Airwindows_jll` artifact. Nothing here is a dependency of AudioPlugins itself.

The module is built from `csrc/airwindows/airwindows_clap.cpp` in AudioPlugins.jl over
[airwin2rack](https://github.com/baconpaul/airwin2rack)'s MIT `airwin-registry` target.
MIT end to end — the GPL3 material in that repository is its JUCE and Rack front ends,
which are not linked.

Ids are `org.airwindows.<effect name>`, taken from the effect's name rather than its
position in the registry, so an upstream insertion cannot repoint an id at a different
effect. [`AudioPlugins.plugins`](@ref) is how to find out what is there;
[`AudioPlugins.clap_open!`](@ref) takes the id.

Third-party binary code runs in your process: an effect that crashes takes Julia with
it. See AudioPlugins' "Known limits".
"""
module Airwindows

using AudioPlugins: register_bundle!
using Airwindows_jll: Airwindows_jll

# The registry is runtime state, so this is `__init__` rather than top level: a path
# baked in at precompile time would not survive a relocated depot.
function __init__()
    register_bundle!(Airwindows_jll.airwindows_clap; source = Airwindows_jll)
    return nothing
end

end # module
