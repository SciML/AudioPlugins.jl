using SciMLTesting, X42Plugins

# The shared docs environment excludes this sublibrary, so rendering is unchecked.
# The AudioPlugins dependency exists to bound the compatible AudioPlugins version.
run_qa(
    X42Plugins;
    aqua_kwargs = (; stale_deps = (; ignore = [:AudioPlugins])),
    api_docs_kwargs = (; rendered = false),
)
