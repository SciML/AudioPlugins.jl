using SciMLTesting

run_tests(;
    core = joinpath(@__DIR__, "core_tests.jl"),
    qa = joinpath(@__DIR__, "qa.jl"),
)
