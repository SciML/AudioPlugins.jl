# What this sublibrary is for is registration: that `using Airwindows` alone puts 504
# effects behind a string id. The arithmetic of the effects themselves is not ours to
# assert -- they are third-party DSP -- so what is checked here is the seam, plus
# enough of one known effect to prove the audio path is really connected.

using Test
using AudioPlugins
using Airwindows
using Airwindows_jll

@testset "Airwindows" begin
    @testset "loading the package registers the bundle" begin
        bs = bundles()
        i = findfirst(b -> b.source === Airwindows_jll, bs)
        @test i !== nothing
        @test bs[i].path == Airwindows_jll.airwindows_clap
        # The registry count comes from the module's own factory, so this also says the
        # artifact was not truncated on the way through the host's descriptor cache.
        @test bs[i].n == 504
    end

    @testset "every effect is there, under org.airwindows." begin
        ps = plugins(Airwindows_jll)
        @test length(ps) == 504
        @test all(p -> startswith(p.id, "org.airwindows."), ps)
        @test length(unique(p.id for p in ps)) == 504
        @test any(p -> p.id == "org.airwindows.Galactic", ps)
    end

    @testset "an effect opens by id and alters the signal" begin
        clap_open!("org.airwindows.Galactic"; sample_rate = 48000, block_size = 64, channels = 2)
        @test clap_is_open()
        @test clap_plugin_name() == "Galactic"
        @test clap_param_count() == 5

        x = Float64[sin(2pi * 440 * i / 48000) for i in 0:127]   # 64 frames, 2 channels
        tok = AudioPlugins.clp_process(
            clap_fill!(x; channels = 2), -1, 0, -1, 0, -1, 0, -1, 0
        )
        y = clap_out(tok)
        @test length(y) == 64
        @test all(isfinite, y)
        # A reverb that returned its input unchanged would not be a reverb.
        @test y != x[1:64]
        clap_close!()
    end

    @testset "a zero-parameter effect is still openable" begin
        # 47 of the 504 expose no parameters at all; a host that assumed otherwise
        # would fail on exactly those.
        clap_open!("org.airwindows.LeftoMono"; sample_rate = 48000, block_size = 64, channels = 2)
        @test clap_param_count() == 0
        @test clap_is_open()
        clap_close!()
    end
end
