using Test, AudioPlugins, Libdl
include("build_live.jl")
function await_output(s, out)
    for _ in 1:2000
        metadata = try_read!(s, out)
        metadata === nothing || return metadata
        sleep(0.002)
    end
    error("native processing failed to produce output: $(live_stats(s))")
end
@testset "registered JLL live ABI" begin
    for format in (:clap, :lv2, :vst3)
        @test live_available(; format)
    end
end
@testset "live formats and devices" begin
    mktempdir() do dir
        lib, sdk = build_live(joinpath(dir, "build"))
        cc, arch = AudioPlugins._c_compiler(), AudioPlugins._c_arch_flags()
        plugin = joinpath(dir, "live.clap")
        source = joinpath(@__DIR__, "plugins", "ap_test_live.c")
        run(`$cc $arch -std=c11 -O2 -fPIC -shared -o $plugin $source`)
        root = dirname(@__DIR__)
        platform = Sys.iswindows() ? ["-lavrt", "-lole32", "-luuid", "-luser32", "-lwinmm"] :
            Sys.isapple() ? ["-framework", "CoreFoundation", "-framework", "CoreAudio", "-framework", "AudioToolbox"] : ["-ldl", "-lm"]
        probe = joinpath(dir, "probe_live" * (Sys.iswindows() ? ".exe" : ""))
        run(`$cc $arch -std=c11 -O1 -DAP_LIVE_TEST -DAP_LIVE_NO_ALLOC_WRAP
             $root/test/probe_clap_live.c $root/csrc/clap_live.c -o $probe -pthread $platform`)
        run(`$probe $plugin`)
        transport = joinpath(dir, "probe_device" * (Sys.iswindows() ? ".exe" : ""))
        run(`$cc $arch -std=c11 -O1 $root/test/probe_live_device.c -o $transport -pthread $platform`)
        run(`$transport`)
        hardware = joinpath(dir, "probe_hardware" * (Sys.iswindows() ? ".exe" : ""))
        run(`$cc $arch -std=c11 -O1 -DAP_LIVE_TEST -DAP_LIVE_WITH_DEVICE
             $root/test/probe_live_hardware.c $root/csrc/clap_live.c $root/csrc/live_device.c
             -o $hardware -pthread $platform`)
        run(`$hardware $plugin`)
        lv2path = lv2_default_path(lv2_test_bundle())
        vstpath = vst3_test_bundle(sdk)
        @test LiveSession === ClapLiveSession
        @test audio_devices(; library = lib, backend = :null) == [(index = 0, name = "NULL Playback Device")]
        @test !live_available(; library = lib, format = :missing)
        @test_throws ErrorException audio_devices(; library = lib, backend = :missing)
        fixtures = [
            (:clap, plugin, "ap.live", "ap.delay", 2, 0.5),
            (:lv2, lv2path, "urn:audioplugins:test:gain", "urn:audioplugins:test:lookahead", 1, 0.5),
            (:vst3, vstpath, AudioPlugins.VST3_TEST_GAIN, AudioPlugins.VST3_TEST_LOOKAHEAD, 1, 0.125),
        ]
        for (format, path, gain, delay, channels, value) in fixtures
            @test live_available(; library = lib, format)
            for driver in (:timer, :device)
                @testset "$format / $driver" begin
                    options = (; format, library = lib, channels, block_size = 32, driver, backend = :null)
                    open_live(path; options..., plugin_id = gain) do s
                        input, output = ones(Float32, 32channels), zeros(Float32, 32channels)
                        @test_throws ErrorException live_latency(s)
                        @test try_write!(s, input; events = [LiveEvent(; value)])
                        if format == :lv2
                            @test_throws ErrorException try_write!(s, input; events = [LiveEvent(; value, frame = 1)])
                        end
                        start!(s)
                        meta = await_output(s, output)
                        @test meta.tick == meta.input_sequence == 0
                        @test output == fill(0.5f0, 32channels)
                        @test live_latency(s) == 0
                        if driver == :device
                            @test device_stats(s).buffering_frames >= 64
                            @test device_stats(s).buffering_frames % 32 == 0
                            @test device_stats(s).lost == 0
                        end
                        before = live_stats(s).blocks
                        GC.gc()
                        for _ in 1:2000
                            live_stats(s).blocks - before >= 16 && break
                            sleep(0.002)
                        end
                        @test live_stats(s).blocks - before >= 16
                        @test live_stats(s).underruns > 0
                        @test live_stats(s).overruns > 0
                        @test poll!(s)
                        for _ in 1:3
                            stop!(s)
                            @test live_stats(s).state == 0
                            start!(s)
                        end
                    end
                    open_live(path; options..., plugin_id = delay) do s
                        input = Float32.(repeat(1:32; inner = channels))
                        output = similar(input)
                        @test try_write!(s, input)
                        start!(s)
                        await_output(s, output)
                        @test output == vcat(zeros(Float32, 16channels), input[1:(16channels)])
                        @test live_latency(s) == 16
                    end
                    # Separate live instances coexist with distinct parameters.
                    open_live(path; options..., plugin_id = gain) do first
                        open_live(path; options..., plugin_id = gain) do second
                            for (s, scale) in ((first, 1), (second, 2))
                                @test try_write!(s, ones(Float32, 32channels); events = [LiveEvent(; value = value * scale)])
                                start!(s)
                            end
                            output = zeros(Float32, 32channels)
                            await_output(first, output); @test all(==(0.5f0), output)
                            await_output(second, output); @test all(==(1.0f0), output)
                        end
                    end
                    if driver == :device
                        open_live(path; options..., plugin_id = gain, capture = true) do s
                            start!(s)
                            output = zeros(Float32, 32channels)
                            meta = await_output(s, output)
                            @test meta.input_sequence == meta.tick
                            @test all(iszero, output)
                            @test live_stats(s).underruns == 0
                            @test device_stats(s).capture == 1
                        end
                    end
                end
            end
        end
        # Live and offline instances of the same plugin keep independent state.
        for (format, path, id, _, channels, value) in fixtures
            if format == :clap
                path, id = clap_test_bundle(), "ap.gain"
            end
            open_live(path; format, plugin_id = id, channels, block_size = 32, library = lib) do s
                @test try_write!(s, ones(Float32, 32channels); events = [LiveEvent(; value)])
                start!(s)
                close_offline = format == :clap ? clap_close! : format == :lv2 ? lv2_close! : vst3_close!
                try
                    if format == :clap
                        clap_open!(path; plugin_id = id, channels, block_size = 32)
                    elseif format == :lv2
                        lv2_open!(path; uri = id, channels, block_size = 32)
                    else
                        vst3_open!(path; class_id = id, channels, block_size = 32)
                    end
                    fill_input = getproperty(AudioPlugins, Symbol(format, "_fill!"))
                    process = getproperty(AudioPlugins, format == :clap ? :clp_process : Symbol(format, "_process"))
                    read_output = getproperty(AudioPlugins, Symbol(format, "_out"))
                    token = fill_input(ones(Float32, 32channels); channels)
                    result = process(token, 0, value * 4, -1, 0, -1, 0, -1, 0)
                    @test read_output(result) == fill(2.0, 32)
                    output = zeros(Float32, 32channels)
                    await_output(s, output)
                    @test all(==(0.5f0), output)
                finally
                    close_offline()
                end
            end
        end
        # MIDI and parameter offsets through actual LV2/VST3 adapters.
        midi_path = lv2_default_path(lv2_midi_test_bundle())
        vst_live = build_vst_live_fixture(dir, sdk)
        for (format, path, id, channels) in (
                (:lv2, midi_path, "urn:audioplugins:test:notegain", 1),
                (:vst3, vst_live, "41504C49564500000000000000000001", 2),
            )
            open_live(path; format, plugin_id = id, channels, block_size = 32, library = lib) do s
                input, output = ones(Float32, 32channels), zeros(Float32, 32channels)
                events = [LiveEvent((0x90, 0x3c, 0x7f); frame = 8), LiveEvent((0x80, 0x3c, 0x00); frame = 24)]
                if format == :vst3
                    insert!(events, 2, LiveEvent(; frame = 16, value = 0.5))
                end
                @test_throws ErrorException try_write!(s, input; events = [LiveEvent((0x90, 0xff, 0x7f))])
                @test_throws ErrorException try_write!(s, input; events = [LiveEvent((0xf0, 0x00, 0x00))])
                @test try_write!(s, input; events)
                start!(s)
                await_output(s, output)
                expected = vcat(
                    zeros(Float32, 8channels), ones(Float32, 8channels),
                    fill(format == :vst3 ? 0.5f0 : 1.0f0, 8channels), zeros(Float32, 8channels)
                )
                @test output == expected
                @test live_stats(s).output_events_dropped == 2
            end
        end
        for id in ("ap.startfail", "ap.activatefail")
            open_live(plugin; plugin_id = id, library = lib, driver = :device, backend = :null) do s
                @test_throws ErrorException start!(s)
                @test live_stats(s).state == 0
            end
        end
        for id in ("ap.restart", "ap.processfail")
            open_live(plugin; plugin_id = id, library = lib, driver = :device, backend = :null) do s
                start!(s)
                for _ in 1:2000
                    live_stats(s).state == 2 && break
                    sleep(0.002)
                end
                @test !poll!(s)
                @test live_stats(s).state == 0
            end
        end
        @test_throws ErrorException open_live(plugin; plugin_id = "ap.live", library = lib, driver = :device, backend = :missing)
    end
end
