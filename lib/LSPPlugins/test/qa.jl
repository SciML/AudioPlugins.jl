using SciMLTesting, LSPPlugins

# The shared docs environment excludes this sublibrary, so rendering is unchecked.
run_qa(LSPPlugins; api_docs_kwargs = (; rendered = false))
