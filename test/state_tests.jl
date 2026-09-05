# clap.state, checked the way a DAW uses it: test/export/probe_state.c is
# a minimal host that sets a parameter, saves, loads into a fresh instance
# and offers garbage. It talks to the plugin through the CLAP ABI directly
# (the package's own host has no state calls), so it also serves a
# juliac plugin, which cannot be loaded into this Julia process.

using Test
using AudioPlugins
using JuliaC
const AP = AudioPlugins

const FIX_STATE = joinpath(@__DIR__, "export")

@testset "AudioPlugins / clap.state" begin
    dir = mktempdir()
    probe = joinpath(dir, "probe_state")
    let cc = AP._c_compiler(), src = joinpath(FIX_STATE, "probe_state.c")
        dl = Sys.islinux() ? ["-ldl"] : String[]
        run(`$cc -O2 -Wall -Wextra -I$(AP.VENDOR_DIR) -o $probe $src $dl`)
    end
    function round_trip(bundle, id, value)
        out = read(`$probe $bundle $id $value`, String)
        pairs = (split(l, ' '; limit = 2) for l in split(out, r"\r?\n"; keepempty = false))
        return Dict(String(k) => String(v) for (k, v) in pairs)
    end

    @testset "the package's own test plugins have no state, and the probe says so" begin
        p = run(pipeline(ignorestatus(`$probe $(clap_test_bundle()) 0 0.5`); stderr = devnull))
        @test p.exitcode == 3
    end

    gain_spec = read_plugin_spec(joinpath(FIX_STATE, "fx_gain.toml"))
    gain = export_plugin(gain_spec, joinpath(dir, "fx_gain.clap"))
    bundles = ["C" => gain]

    if VERSION >= v"1.12" && isdefined(JuliaC, :ImageRecipe) && !Sys.iswindows()
        jl_spec = read_plugin_spec(joinpath(FIX_STATE, "jl_gain.toml"))
        push!(bundles, "Julia" => export_plugin(jl_spec, joinpath(dir, "jl_gain.clap")))
    end

    for (kind, bundle) in bundles
        @testset "a $kind step's parameters survive save and load" begin
            r = round_trip(bundle, 0, 0.75)
            @test r["set"] == "0.75"
            # "APST", version, count, then two (id, value) records.
            @test r["saved"] == string(4 + 4 + 4 + 2 * (4 + 8))
            @test r["fresh"] == "1"            # the default, before loading
            @test r["loaded"] == "0.75"
            @test r["garbage"] == "rejected"
            @test r["after_garbage"] == "0.75" # a rejected load changes nothing
            b = round_trip(bundle, 7, 1.0)     # the stepped bool parameter
            @test b["set"] == "1" && b["fresh"] == "0" && b["loaded"] == "1"
        end
    end
end
