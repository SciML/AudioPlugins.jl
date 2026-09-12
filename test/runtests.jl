using SciMLTesting

# Explicit group bodies rather than folder discovery: `test/export/` and
# `test/plugins/` are fixture directories whose `*.jl` files are plugin step
# functions compiled into plugins, not tests. Folder discovery resolves a group
# name case-insensitively, so a group named `Export` would glob those fixtures.
run_tests(;
    core = joinpath(@__DIR__, "clap_host_tests.jl"),
    groups = Dict(
        "Bundles" => joinpath(@__DIR__, "bundle_tests.jl"),
        "Export" => joinpath(@__DIR__, "export_tests.jl"),
        "LV2" => joinpath(@__DIR__, "lv2tests.jl"),
        "State" => joinpath(@__DIR__, "state_tests.jl"),
    ),
    qa = (; env = joinpath(@__DIR__, "qa"), body = joinpath(@__DIR__, "qa", "qa.jl")),
)
