# What this sublibrary is for is registration: that `using ZamPlugins` alone puts the
# sixteen plugins behind string ids. The arithmetic of the plugins themselves is not ours
# to assert -- they are third-party DSP -- so what is checked here is the seam, then every
# plugin this host can drive is actually driven.

using Test
using AudioPlugins
using ZamPlugins
using ZamPlugins_jll

# Channel count per plugin, which is not cosmetic: several of these take a sidechain as a
# second audio input, so "stereo" here means "two host channels", not "two signal
# channels". ZamCompX2 and ZamGateX2 are absent because they want three audio inputs
# (left, right, sidechain) and AudioPlugins' CLAP host is capped at two channels
# (CLAP_HOST_MAX_CHAN); they are in the artifact and usable by a host that isn't.
const DRIVABLE = [
    "com.zamaudio.ZamAutoSat" => 1,
    "com.zamaudio.ZamDelay" => 1,
    "com.zamaudio.ZamEcho" => 1,
    "com.zamaudio.ZamEQ2" => 1,
    "com.zamaudio.ZamGEQ31" => 1,
    "com.zamaudio.ZamGrains" => 1,
    "com.zamaudio.ZamPhono" => 1,
    "com.zamaudio.ZamTube" => 1,
    "com.zamaudio.ZaMultiComp" => 1,
    "com.zamaudio.ZamComp" => 2,
    "com.zamaudio.ZamDynamicEQ" => 2,
    "com.zamaudio.ZamGate" => 2,
    "com.zamaudio.ZaMaximX2" => 2,
    "com.zamaudio.ZaMultiCompX2" => 2,
]

const SIDECHAIN_STEREO = ["com.zamaudio.ZamCompX2", "com.zamaudio.ZamGateX2"]

@testset "ZamPlugins" begin
    @testset "loading the package registers all sixteen bundles" begin
        bs = filter(b -> b.source === ZamPlugins_jll, bundles())
        @test length(bs) == 16
        # Each module declares exactly one plugin, so a bundle reporting anything else
        # would mean the artifact and the registry disagree about what was loaded.
        @test all(b -> b.n == 1, bs)
    end

    @testset "every plugin is there, under com.zamaudio." begin
        ids = sort([p.id for p in plugins(ZamPlugins_jll)])
        @test ids == sort([first.(DRIVABLE); SIDECHAIN_STEREO])
        # The two convolution plugins are excluded on licence grounds -- they link
        # GPL-3.0-or-later zita-convolver -- so their absence is a property of the
        # collection, not an accident of the build.
        @test !any(in(("com.zamaudio.ZamVerb", "com.zamaudio.ZamHeadX2")), ids)
    end

    @testset "$id runs audio through it" for (id, ch) in DRIVABLE
        clap_open!(id; sample_rate = 48000, block_size = 64, channels = ch)
        @test clap_is_open()

        x = Float64[0.9 * sin(2pi * 440 * i / 48000) for i in 0:(64ch - 1)]
        tok = AudioPlugins.clp_process(
            clap_fill!(x; channels = ch), -1, 0, -1, 0, -1, 0, -1, 0
        )
        y = clap_out(tok)
        @test length(y) == 64
        @test all(isfinite, y)
        # A processor that returned its input unchanged would not be doing anything; all
        # fourteen of these alter the signal at their default settings.
        @test y != x[1:64]
        clap_close!()
    end

    @testset "a zero-parameter plugin is still openable" begin
        # ZamAutoSat exposes none; a host that assumed otherwise would fail on it alone.
        clap_open!("com.zamaudio.ZamAutoSat"; sample_rate = 48000, block_size = 64, channels = 1)
        @test clap_param_count() == 0
        @test clap_is_open()
        clap_close!()
    end
end
