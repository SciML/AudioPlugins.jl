using SciMLTesting, X42PluginsGPL3

# AudioPlugins is used for lv2_default_path inside lv2_path; the four JLLs are
# the packaged bundles. Nothing to ignore for stale_deps.
run_qa(X42PluginsGPL3)
