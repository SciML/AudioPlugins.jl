using Documenter, AudioPlugins
using Airwindows, DragonflyReverb, LSPPlugins, X42Plugins, X42PluginsGPL3, ZamPlugins

cp("./docs/Manifest.toml", "./docs/src/assets/Manifest.toml", force = true)
cp("./docs/Project.toml", "./docs/src/assets/Project.toml", force = true)

makedocs(
    modules = [
        AudioPlugins,
        Airwindows,
        DragonflyReverb,
        LSPPlugins,
        X42Plugins,
        X42PluginsGPL3,
        ZamPlugins,
    ],
    sitename = "AudioPlugins.jl",
    clean = true,
    doctest = true,
    linkcheck = true,
    checkdocs = :public,
    format = Documenter.HTML(
        assets = ["assets/favicon.ico"],
        canonical = "https://docs.sciml.ai/AudioPlugins/stable/"
    ),
    pages = [
        "index.md",
        "Hosting a plugin" => "hosting.md",
        "Authoring a plugin" => "authoring.md",
        "Plugin collections" => "collections.md",
        "Collection API" => "collections_api.md",
        "Known limits" => "limits.md",
        "API" => "api.md",
    ]
)

deploydocs(repo = "github.com/SciML/AudioPlugins.jl"; push_preview = true)
