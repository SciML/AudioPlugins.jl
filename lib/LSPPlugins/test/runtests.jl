# What this sublibrary is for is registration: that `using LSPPlugins` alone puts the
# whole suite behind string ids. The arithmetic of the plugins themselves is not ours to
# assert -- they are third-party DSP -- so what is checked here is the seam, plus enough
# of one plugin to prove the audio path is really connected.

using Test
using AudioPlugins
using LSPPlugins
using LSPPlugins_jll

@testset "LSPPlugins" begin
    @testset "loading the package registers the bundle" begin
        bs = bundles()
        i = findfirst(b -> b.source === LSPPlugins_jll, bs)
        @test i !== nothing
        @test bs[i].path == LSPPlugins_jll.lsp_plugins_clap
        # The count comes from the module's own factory, so this also says the host saw
        # the whole factory rather than a prefix of it.
        @test bs[i].n == 198
    end

    @testset "every plugin is there, under in.lsp-plug." begin
        ps = plugins(LSPPlugins_jll)
        @test length(ps) == 198
        @test all(p -> startswith(p.id, "in.lsp-plug."), ps)
        @test length(unique(p.id for p in ps)) == 198
        # The suite's reason for being: a full parametric EQ and the dynamics family.
        @test any(p -> p.id == "in.lsp-plug.para_equalizer_x16_stereo", ps)
        @test any(p -> p.id == "in.lsp-plug.compressor_stereo", ps)
    end

    @testset "a plugin opens by id and alters the signal" begin
        clap_open!("in.lsp-plug.compressor_stereo"; sample_rate = 48000, block_size = 64, channels = 2)
        @test clap_is_open()
        @test clap_plugin_name() == "Compressor Stereo"
        @test clap_param_count() > 0

        x = Float64[0.9 * sin(2pi * 440 * i / 48000) for i in 0:127]   # 64 frames, 2 channels
        tok = AudioPlugins.clp_process(
            clap_fill!(x; channels = 2), -1, 0, -1, 0, -1, 0, -1, 0
        )
        y = clap_out(tok)
        @test length(y) == 64
        @test all(isfinite, y)
        # A compressor that returned its input unchanged would not be doing anything;
        # LSP's defaults include makeup gain, so the level moves.
        @test y != x[1:64]
        clap_close!()
    end

    @testset "the mid/side and left/right variants open too" begin
        # `_ms` and `_lr` are the layouts a host is most likely to get wrong, because
        # they are still two-channel but not plain stereo.
        for id in ("in.lsp-plug.compressor_ms", "in.lsp-plug.compressor_lr")
            clap_open!(id; sample_rate = 48000, block_size = 64, channels = 2)
            @test clap_is_open()
            clap_close!()
        end
    end
end
