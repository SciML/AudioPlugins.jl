using SciMLTesting, ZamPlugins

# The shared docs environment excludes this sublibrary, so rendering is unchecked.
run_qa(ZamPlugins; api_docs_kwargs = (; rendered = false))
