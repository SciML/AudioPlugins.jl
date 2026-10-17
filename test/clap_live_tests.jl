using Test, AudioPlugins

@testset "experimental CLAP live sessions" begin
    @test !live_available(; library = joinpath(mktempdir(), "missing.so"))
    if Sys.islinux()
        mktempdir() do dir
            root = dirname(@__DIR__)
            cc = AudioPlugins._c_compiler()
            lib = joinpath(dir, "libclap_live.so")
            plugin = joinpath(dir, "ap_live.clap")
            arch = AudioPlugins._c_arch_flags()
            run(`$cc $arch -std=c11 -O2 -fPIC -shared -Wall -Wextra -Werror -o $lib $root/csrc/clap_live.c -pthread -ldl -lm`)
            run(`$cc $arch -std=c11 -O2 -fPIC -shared -Wall -Wextra -Werror -o $plugin $root/test/plugins/ap_test_live.c`)
            @test live_available(; library = lib)
            @test sizeof(LiveEvent) == 24
            @test sizeof(LiveStats) == 44
            @test_throws ErrorException open_live(plugin; plugin_id = "missing", library = lib)
            @test_throws ErrorException open_live(plugin; plugin_id = "ap.initfail", library = lib)
            @test_throws ErrorException open_live(plugin; plugin_id = "ap.live", library = lib, channels = 1)
            saved = Ref{ClapLiveSession}()
            open_live(plugin; plugin_id = "ap.live", library = lib, block_size = 32, queue_blocks = 4) do s
                saved[] = s
                @test isopen(s)
                input, output = ones(Float32, 64), zeros(Float32, 64)
                @test try_read!(s, output) === nothing
                @test_throws DimensionMismatch try_write!(s, Float32[1])
                @test_throws ArgumentError try_write!(s, input; sequence = typemax(UInt64))
                @test_throws ErrorException try_write!(s, input; events = [LiveEvent(; value = NaN)])
                events = [LiveEvent(; value = 0.5), LiveEvent(; frame = 16, value = 2)]
                for i in 0:3
                    @test try_write!(s, input; events, sequence = i)
                end
                @test !try_write!(s, input)
                start!(s)
                @test_throws ErrorException close(s)
                # The earliest output remains in the ring even if Julia is late.
                metadata = nothing
                for _ in 1:1000
                    metadata = try_read!(s, output)
                    metadata !== nothing && break
                    sleep(0.002)
                end
                @test metadata !== nothing
                @test metadata.tick == 0 && metadata.input_sequence == 0
                @test output == vcat(fill(0.5f0, 32), fill(2.0f0, 32))
                before = live_stats(s)
                GC.gc()
                after = before
                for _ in 1:1000
                    sleep(0.002)
                    after = live_stats(s)
                    after.blocks - before.blocks >= 16 && break
                end
                @test after.blocks - before.blocks >= 16
                @test after.underruns > 0 && after.overruns > 0
                @test poll!(s)
                stop!(s)
                @test live_stats(s).state == 0
                @test try_read!(s, output) === nothing
                start!(s)
                stop!(s)
            end
            @test !isopen(saved[])
            close(saved[])
            @test_throws ErrorException live_stats(saved[])
            @test_throws ErrorException open_live(plugin; plugin_id = "ap.live", library = lib) do s
                saved[] = s
                start!(s)
                error("test unwind")
            end
            @test !isopen(saved[])
        end
    else
        @test live_available()
    end
end
