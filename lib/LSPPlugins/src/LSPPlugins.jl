"""
    LSPPlugins

The [Linux Studio Plugins](https://lsp-plug.in/) suite — 198 plugins in one CLAP module —
registered with [AudioPlugins](https://github.com/SciML/AudioPlugins.jl) so that every
one is openable by id:

```julia
using AudioPlugins, LSPPlugins

plugins(LSPPlugins_jll)          # all 198, with names
clap_open!("in.lsp-plug.compressor_stereo"; sample_rate = 48000, block_size = 256, channels = 2)
```

!!! warning "MIT package, LGPL-3.0-or-later binaries in your process"
    This package is MIT, and covers only the lines of Julia below. The binaries it
    installs and loads — `LSPPlugins_jll` — are **LGPL-3.0-or-later**, and they run
    inside your Julia process. An MIT wrapper is exactly what the LGPL is for: nothing
    is linked statically, the module is loaded at runtime, and a user who wants a
    modified LSP can replace the artifact. The terms governing the binaries are still
    theirs — distributing a work that loads these plugins means honouring the LGPL for
    the LSP part of it, in particular the recipient's right to relink against their own
    build. `LICENSE.md` and the README carry the per-component evidence.

    AudioPlugins itself is MIT and depends on none of this — installing AudioPlugins
    does not install LSP, and nothing here is reachable from the MIT core.

Ids are LSP's own, `in.lsp-plug.<plugin>`, with the channel layout in the suffix:
`_mono`, `_stereo`, `_lr` (independent left/right) and `_ms` (mid/side).
[`AudioPlugins.plugins`](@ref) is how to find out what is there;
[`AudioPlugins.clap_open!`](@ref) takes the id.

Third-party binary code runs in your process: a plugin that crashes takes Julia with it.
See AudioPlugins' "Known limits".
"""
module LSPPlugins

using AudioPlugins: register_bundle!
using LSPPlugins_jll: LSPPlugins_jll

# The registry is runtime state, so this is `__init__` rather than top level: a path
# baked in at precompile time would not survive a relocated depot.
function __init__()
    register_bundle!(LSPPlugins_jll.lsp_plugins_clap; source = LSPPlugins_jll)
    return nothing
end

end # module
