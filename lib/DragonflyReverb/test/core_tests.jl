# What this sublibrary is for is registration: that `using DragonflyReverb` alone puts
# the four reverbs behind string ids. The arithmetic of the reverbs themselves is not
# ours to assert -- they are third-party DSP -- so what is checked here is the seam,
# plus enough of one reverb to prove the audio path is really connected.

using Test
using AudioPlugins
using DragonflyReverb
using DragonflyReverb_jll

const IDS = [
    "michaelwillis.dragonfly.hall",
    "michaelwillis.dragonfly.room",
    "michaelwillis.dragonfly.plate",
    "michaelwillis.dragonfly.early",
]

@testset "DragonflyReverb" begin
    @testset "loading the package registers all four bundles" begin
        bs = filter(b -> b.source === DragonflyReverb_jll, bundles())
        @test length(bs) == 4
        # Each module declares exactly one plugin, so a bundle reporting anything else
        # would mean the artifact and the registry disagree about what was loaded.
        @test all(b -> b.n == 1, bs)
    end

    @testset "each reverb is there, under michaelwillis.dragonfly." begin
        ps = plugins(DragonflyReverb_jll)
        @test sort([p.id for p in ps]) == sort(IDS)
    end

    @testset "$id opens and alters the signal" for id in IDS
        clap_open!(id; sample_rate = 48000, block_size = 64, channels = 2)
        @test clap_is_open()
        @test clap_param_count() > 0

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
end
