# Airwindows.jl

Chris Johnson's [Airwindows](https://www.airwindows.com/) effects — 504 of them in one
CLAP module — as a plugin collection for
[AudioPlugins.jl](https://github.com/SciML/AudioPlugins.jl).

```julia
using AudioPlugins, Airwindows

plugins(Airwindows_jll)   # all 504, with names and categories
clap_open!("org.airwindows.Galactic"; sample_rate = 48000, block_size = 256, channels = 2)
```

A sublibrary, so that the effects are **opt-in**: AudioPlugins knows how to host a CLAP
module and ships none, and installing this package is what pulls in the `Airwindows_jll`
artifact. Nothing here is a dependency of AudioPlugins itself.

MIT end to end. The module is built from `csrc/airwindows/airwindows_clap.cpp` in
AudioPlugins.jl over [airwin2rack](https://github.com/baconpaul/airwin2rack)'s
`airwin-registry` target; the GPL3 material in that repository is its JUCE and Rack
front ends, which are not linked. The DSP is Chris Johnson's, MIT, and the artifact
carries his licence.
