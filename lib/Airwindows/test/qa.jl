using SciMLTesting, Airwindows

# The shared docs environment excludes this sublibrary, so rendering is unchecked.
run_qa(Airwindows; api_docs_kwargs = (; rendered = false))
