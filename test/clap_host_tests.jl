# The CLAP host, tested by hosting plugins that ship with this package.
#
# Hosting is only proved by hosting something, and depending on a third-party
# plugin would make the suite depend on a binary whose arithmetic we cannot
# check and may not be able to fetch. `test/plugins/ap_test_plugins.c` is
# ours: a gain, a one-pole and a 16-sample lookahead, in one bundle, so every
# expectation below is arithmetic rather than a recording -- the same approach
# the synthetic sources take in AudioComponents and VisionComponents.

using Test
using Libdl
using AudioPlugins
const AP = AudioPlugins

const BUNDLE = clap_test_bundle()

# CLAPHost_jll ships a prebuilt build of `csrc/clap_host.c`, so a change to that
# source reaches Julia only once the JLL has been rebuilt and the compat bound
# raised. The chained-parameter tests are gated on the host actually exporting
# the entry point rather than on a version number: the C-level coverage that
# runs on every PR is `test/probe.c`, which compiles the source in this
# repository, and these switch themselves on when the JLL catches up.
const HOST_HAS_CHAIN = clap_host_available() && let h = Libdl.dlopen(clap_lib_path())
    ok = Libdl.dlsym_e(h, :clap_set_param) != C_NULL
    Libdl.dlclose(h)
    ok
end

# The eight-parameter fixture, built once. It is not part of `clap_test_bundle`
# because that bundle's contents are documented and exercised by the README:
# widening it would rewrite examples that have nothing to do with parameters.
const MANY_DIR = Ref{String}("")
function many_bundle()
    isempty(MANY_DIR[]) && (MANY_DIR[] = mktempdir())
    out = joinpath(MANY_DIR[], "ap_manyparams.clap")
    if !isfile(out)
        cc = AP._c_compiler()
        src = joinpath(@__DIR__, "plugins", "ap_test_manyparams.c")
        run(`$cc $(AP._c_arch_flags()) -O2 -fPIC -shared -Wall -Wextra -o $out $src`)
    end
    return out
end

sha_free(x) = x  # (no fixtures to checksum: the plugin is built from source here)

if !clap_host_available()
    @testset "AudioPlugins / CLAP: no prebuilt host on this platform" begin
        # A platform CLAPHost_jll has no build for. Hosting is covered by
        # the C probes in test/export, which compile csrc/clap_host.c.
        @test isfile(clap_src_path())
        @test_throws ErrorException clap_lib_path()
        @test_throws ErrorException clap_scan(BUNDLE)
        @info "CLAPHost_jll has no build for $(Base.BinaryPlatforms.host_triplet()): " *
            "in-process hosting tests do not run here"
    end
else
    @testset "AudioPlugins / CLAP" begin

        @testset "the host comes prebuilt, the test plugins do not" begin
            # The host library is the JLL's, not something compiled into the
            # package directory: it exists, it loads, and it exports the ABI.
            @test isfile(clap_lib_path())
            @test !startswith(clap_lib_path(), pkgdir(AudioPlugins))
            h = Libdl.dlopen(clap_lib_path())
            @test Libdl.dlsym(h, :clap_process) != C_NULL
            Libdl.dlclose(h)
            # The sources still ship, for a standalone C program to link.
            @test isfile(clap_src_path())
            @test endswith(clap_src_path(), joinpath("csrc", "clap_host.c"))
            # The test bundle is built into a scratch space, not into the package.
            @test !startswith(BUNDLE, pkgdir(AudioPlugins))
            @test (@test_deprecated build_clap_host!()) == clap_lib_path()
        end

        @testset "the bundle builds and enumerates" begin
            @test isfile(BUNDLE)
            plugs = clap_scan(BUNDLE)
            @test length(plugs) == 3
            @test [p.id for p in plugs] == ["ap.gain", "ap.onepole", "ap.lookahead"]
            @test plugs[1].name == "AudioPlugins Test Gain"
        end

        @testset "failure paths are loud" begin
            @test_throws ErrorException clap_scan("/nonexistent.clap")
            # A real shared object that simply is not a CLAP plugin.
            libm = "/lib/aarch64-linux-gnu/libm.so.6"
            if isfile(libm)
                @test_throws ErrorException clap_scan(libm)
                @test occursin("clap_entry", clap_last_error())
            end
            @test_throws ErrorException clap_open!(
                BUNDLE; plugin_id = "no.such.id",
                block_size = 64
            )
            @test occursin("no.such.id", clap_last_error())
            @test_throws ErrorException clap_open!(
                BUNDLE; plugin_id = "ap.gain",
                block_size = 99999
            )
            @test_throws ErrorException clap_open!(
                BUNDLE; plugin_id = "ap.gain",
                block_size = 64, channels = 7
            )
            @test_throws ErrorException clap_open!(
                BUNDLE; plugin_id = "ap.gain",
                sample_rate = -1, block_size = 64
            )
            # State after a failed open is closed, not half-open.
            @test !clap_is_open()
        end

        @testset "open reports the configuration actually in force" begin
            clap_open!(BUNDLE; plugin_id = "ap.gain", sample_rate = 48000, block_size = 64)
            @test clap_is_open()
            @test clap_block_size() == 64
            @test clap_sample_rate() == 48000
            @test clap_plugin_name() == "AudioPlugins Test Gain"
        end

        @testset "parameter discovery" begin
            ps = clap_params()
            @test length(ps) == 1
            @test ps[1].id == 0.0
            @test ps[1].name == "Gain"
            @test (ps[1].min, ps[1].max, ps[1].default) == (0.0, 4.0, 1.0)
        end

        @testset "gain arithmetic: out == in * g, sample-exactly" begin
            clap_reset_counters!()
            x = [sin(2pi * 5 * i / 64) * 0.5 for i in 0:63]
            for g in (0.5, 1.0, 2.0, 0.0)
                tok = clap_fill!(x)
                out = AP.clp_process(tok, 0, g, -1, 0, -1, 0, -1, 0)
                @test !isnan(out)
                y = clap_out(out)
                @test length(y) == 64
                # float32 storage inside the plugin, so compare at float precision
                @test all(abs.(Float32.(x .* g) .- Float32.(y)) .< 1.0e-7)
            end
            @test clap_n_process() == 4     # exactly one process() per call
        end

        @testset "a stale token is refused, not answered" begin
            t1 = clap_fill!(ones(64))
            o1 = AP.clp_process(t1, 0, 1.0, -1, 0, -1, 0, -1, 0)
            t2 = clap_fill!(zeros(64))
            o2 = AP.clp_process(t2, 0, 1.0, -1, 0, -1, 0, -1, 0)
            @test isnan(AP.clp_out_rms(o1))            # superseded output token
            @test !isnan(AP.clp_out_rms(o2))           # the current one still reads
            @test isempty(clap_out(o1))                # and the Julia reader agrees
            @test isnan(AP.clp_process(t1, 0, 1.0, -1, 0, -1, 0, -1, 0))
            @test AP.clp_out_valid(o1) == 0.0
            @test AP.clp_out_valid(o2) == 1.0
        end

        @testset "parameter changes land on the intended block" begin
            t = clap_fill!(fill(1.0, 64))
            o = AP.clp_process(t, 0, 2.0, -1, 0, -1, 0, -1, 0)
            @test AP.clp_out_peak(o) ≈ 2.0
            @test AP.clp_out_rms(o) ≈ 2.0
            t = clap_fill!(fill(1.0, 64))
            o = AP.clp_process(t, 0, 0.25, -1, 0, -1, 0, -1, 0)
            @test AP.clp_out_peak(o) ≈ 0.25
            # And the plugin's own view agrees with what was sent.
            @test AP.clap_param_value(0) ≈ 0.25
            @test isnan(AP.clap_param_value(77))
        end

        @testset "latency is reported (and documented as uncompensated)" begin
            clap_open!(BUNDLE; plugin_id = "ap.gain", block_size = 64)
            @test clap_latency() == 0.0
            clap_open!(BUNDLE; plugin_id = "ap.lookahead", block_size = 64)
            @test clap_latency() == 16.0
            @test clap_param_count() == 0        # lookahead exposes no parameters
            @test !clap_compensating()           # off by default
            # The delay is real: a unit impulse comes out 16 samples later.
            imp = zeros(64); imp[1] = 1.0
            t = clap_fill!(imp)
            o = AP.clp_process(t, -1, 0, -1, 0, -1, 0, -1, 0)
            y = clap_out(o)
            @test y[17] ≈ 1.0                   # 1-based: sample 16 -> index 17
            @test all(abs.(y[[1:16; 18:64]]) .< 1.0e-9)
            # And without the mode, a read still yields the whole raw block.
            @test length(y) == 64
        end

        @testset "opt-in latency compensation aligns the stream" begin
            # A signal with no block-period symmetry, so a misalignment of even
            # one sample shows up in the comparison.
            clap_open!(
                BUNDLE; plugin_id = "ap.lookahead", block_size = 64,
                compensate_latency = true
            )
            @test clap_compensating()
            @test clap_latency() == 16.0
            x = [sin(2pi * 13 * i / 256) + 0.3 * cos(2pi * i / 97) for i in 0:255]
            aligned = Float64[]
            for k in 0:3
                o = AP.clp_process(
                    clap_fill!(x[(k * 64 + 1):((k + 1) * 64)]),
                    -1, 0, -1, 0, -1, 0, -1, 0
                )
                y = clap_out(o)
                # The lag: 16 samples do not fit in one block, so the first
                # read has no whole block to give back yet.
                k == 0 && @test isempty(y)
                k > 0 && @test length(y) == 64
                append!(aligned, y)
            end
            tail = clap_flush!()
            @test length(tail) == 64            # the last block arrives on flush
            @test clap_flush!() == tail          # reads are idempotent
            append!(aligned, tail)
            @test length(aligned) == length(x)
            @test maximum(abs.(aligned .- x)) < 1.0e-6

            # An impulse at input sample j lands at aligned sample j, not j+16.
            clap_open!(
                BUNDLE; plugin_id = "ap.lookahead", block_size = 64,
                compensate_latency = true
            )
            imp = zeros(128); imp[1] = 1.0
            got = Float64[]
            for k in 0:1
                o = AP.clp_process(
                    clap_fill!(imp[(k * 64 + 1):((k + 1) * 64)]),
                    -1, 0, -1, 0, -1, 0, -1, 0
                )
                append!(got, clap_out(o))
            end
            append!(got, clap_flush!())
            @test got[1] ≈ 1.0
            @test all(abs.(got[2:128]) .< 1.0e-9)

            # A zero-latency plugin under the mode is just the raw stream.
            clap_open!(
                BUNDLE; plugin_id = "ap.gain", block_size = 64,
                compensate_latency = true
            )
            o = AP.clp_process(clap_fill!(fill(0.5, 64)), 0, 2.0, -1, 0, -1, 0, -1, 0)
            @test clap_out(o) ≈ fill(1.0, 64)
            @test isempty(clap_flush!())

            # Latency larger than the block: the lag is ceil(N/B) blocks.
            clap_open!(
                BUNDLE; plugin_id = "ap.lookahead", block_size = 8,
                compensate_latency = true
            )
            x8 = collect(0.0:31.0)
            got8 = Float64[]
            for k in 0:3
                o = AP.clp_process(
                    clap_fill!(x8[(k * 8 + 1):((k + 1) * 8)]),
                    -1, 0, -1, 0, -1, 0, -1, 0
                )
                y = clap_out(o)
                k < 2 && @test isempty(y)        # ceil(16/8) = 2 blocks of lag
                k >= 2 && @test length(y) == 8
                append!(got8, y)
            end
            tail8 = clap_flush!()
            @test length(tail8) == 16
            append!(got8, tail8)
            @test got8 == Float64.(Float32.(x8))   # a delay copies exactly
        end

        @testset "compensation is per channel, and enforces read-once" begin
            clap_open!(
                BUNDLE; plugin_id = "ap.lookahead", block_size = 64,
                channels = 2, compensate_latency = true
            )
            x = collect(0.0:63.0)
            inter = vec(permutedims([x 2 .* x]))   # interleaved: ch0 = x, ch1 = 2x
            o1 = AP.clp_process(
                clap_fill!(inter; channels = 2), -1, 0, -1, 0, -1, 0, -1, 0
            )
            @test isempty(clap_out(o1; channel = 0))
            @test isempty(clap_out(o1; channel = 1))
            o2 = AP.clp_process(
                clap_fill!(inter; channels = 2), -1, 0, -1, 0, -1, 0, -1, 0
            )
            @test clap_out(o2; channel = 0) ≈ x atol = 1.0e-7
            @test clap_out(o2; channel = 1) ≈ 2 .* x atol = 1.0e-7
            @test clap_flush!(; channel = 0) ≈ x atol = 1.0e-7
            @test clap_flush!(; channel = 1) ≈ 2 .* x atol = 1.0e-7

            # A block processed but never read is a hole in the aligned
            # stream: loud, not silently misaligned.
            clap_open!(
                BUNDLE; plugin_id = "ap.lookahead", block_size = 64,
                compensate_latency = true
            )
            AP.clp_process(clap_fill!(ones(64)), -1, 0, -1, 0, -1, 0, -1, 0)
            o2 = AP.clp_process(clap_fill!(ones(64)), -1, 0, -1, 0, -1, 0, -1, 0)
            @test_throws ErrorException clap_out(o2)
            # Re-reading a stale token errors rather than returning [].
            clap_open!(
                BUNDLE; plugin_id = "ap.lookahead", block_size = 64,
                compensate_latency = true
            )
            o1 = AP.clp_process(clap_fill!(ones(64)), -1, 0, -1, 0, -1, 0, -1, 0)
            clap_out(o1)
            o2 = AP.clp_process(clap_fill!(ones(64)), -1, 0, -1, 0, -1, 0, -1, 0)
            clap_out(o2)
            @test_throws ErrorException clap_out(o1)
            @test_throws ErrorException clap_out(NaN)
        end

        @testset "clap_flush! without the mode is an error" begin
            clap_open!(BUNDLE; plugin_id = "ap.gain", block_size = 64)
            @test !clap_compensating()
            @test_throws ErrorException clap_flush!()
        end

        @testset "state persists across blocks (the invariant that matters)" begin
            # Two consecutive 32-frame blocks through the one-pole must equal one
            # 64-frame run over the concatenated input. If the plugin's state reset
            # per block, or the host re-activated between blocks, the second half
            # would restart from zero.
            a = 0.25
            clap_open!(BUNDLE; plugin_id = "ap.onepole", block_size = 32)
            split = Float64[]
            for _ in 1:2
                t = clap_fill!(ones(32))
                o = AP.clp_process(t, 0, a, -1, 0, -1, 0, -1, 0)
                append!(split, clap_out(o))
            end
            clap_open!(BUNDLE; plugin_id = "ap.onepole", block_size = 64)
            t = clap_fill!(ones(64))
            o = AP.clp_process(t, 0, a, -1, 0, -1, 0, -1, 0)
            whole = clap_out(o)

            @test length(split) == 64 && length(whole) == 64
            @test maximum(abs.(split .- whole)) < 1.0e-6
            # Closed form: a one-pole step response is 1 - (1-a)^n.
            @test split[32] ≈ 1 - (1 - a)^32 atol = 1.0e-5
            @test split[64] ≈ 1 - (1 - a)^64 atol = 1.0e-5
            # And the state really is carried: block 2 starts above where block 1 began.
            @test split[33] > split[1]
        end

        @testset "the plugin is not reset by reopening at a new block size" begin
            # Reopening is a fresh instance, so it MUST start from zero -- the
            # counterpart of the test above, and what makes that one meaningful.
            clap_open!(BUNDLE; plugin_id = "ap.onepole", block_size = 32)
            t = clap_fill!(ones(32))
            o = AP.clp_process(t, 0, 0.25, -1, 0, -1, 0, -1, 0)
            first_of_fresh = clap_out(o)[1]
            @test first_of_fresh ≈ 0.25 atol = 1.0e-6    # y = 0 + 0.25*(1-0)
        end

        if !HOST_HAS_CHAIN
            @info "CLAPHost_jll $(clap_lib_path()) predates clap_set_param: the chained " *
                "parameter and clp_expect tests are covered by test/probe.c until it is rebuilt"
        else
            @testset "a parameter chain drives more parameters than there are slots" begin
                # ap.weights has eight parameters and clap_process has four slots,
                # so this is the case the chain exists for. The weights are powers
                # of two, so the assertion pins down which value reached which id
                # rather than only that eight numbers arrived.
                many = many_bundle()

                clap_open!(many; sample_rate = 48000, block_size = 8, channels = 1)
                @test clap_param_count() == 8
                @test clap_plugin_index() == 0

                vals = [k / 16 for k in 0:7]
                g = sum((2.0^k) * vals[k + 1] for k in 0:7)

                tok = clap_fill!(ones(8))
                for k in 0:7
                    tok = AP.clp_set(tok, k, vals[k + 1])
                end
                y = clap_out(AP.clp_process(tok, -1, 0, -1, 0, -1, 0, -1, 0))
                @test all(≈(g; rtol = 1.0e-6), y)

                # Held across a block: the second block queues no event, and the
                # output is the same -- change detection must not mean "forgotten".
                tok = clap_fill!(ones(8))
                for k in 0:7
                    tok = AP.clp_set(tok, k, vals[k + 1])
                end
                y2 = clap_out(AP.clp_process(tok, -1, 0, -1, 0, -1, 0, -1, 0))
                @test y2 == y

                # One parameter moved, seven held.
                tok = clap_fill!(ones(8))
                for k in 0:7
                    tok = AP.clp_set(tok, k, k == 7 ? 1.0 : vals[k + 1])
                end
                y3 = clap_out(AP.clp_process(tok, -1, 0, -1, 0, -1, 0, -1, 0))
                @test all(≈(g + 128 * (1 - vals[8]); rtol = 1.0e-6), y3)
            end

            @testset "the chain and the four slots compose" begin
                many = many_bundle()
                clap_open!(many; sample_rate = 48000, block_size = 8, channels = 1)
                # ids 0 and 1 through the slots, id 7 through the chain.
                tok = AP.clp_set(clap_fill!(ones(8)), 7, 0.5)
                y = clap_out(AP.clp_process(tok, 0, 1.0, 1, 1.0, -1, 0, -1, 0))
                @test all(≈(1 + 2 + 64; rtol = 1.0e-6), y)
            end

            @testset "a chain refuses rather than guessing" begin
                many = many_bundle()
                clap_open!(many; sample_rate = 48000, block_size = 8, channels = 1)
                tok = clap_fill!(ones(8))

                @test isnan(AP.clp_set(tok, 8, 0.5))          # no such parameter
                @test isnan(AP.clp_set(tok, -1, 0.5))         # nor a negative one
                @test isnan(AP.clp_set(tok, 0, NaN))          # nor a value of NaN
                @test isnan(AP.clp_set(tok + 1, 0, 0.5))      # nor a stale block
                @test isnan(AP.clp_set(NaN, 0, 0.5))
                # A refusal poisons the chain rather than being skipped over.
                @test isnan(AP.clp_process(AP.clp_set(NaN, 0, 0.5), -1, 0, -1, 0, -1, 0, -1, 0))

                # An abandoned chain does not leak into the next block: id 0 was
                # queued and never processed, so the block that follows sees the
                # plugin's own defaults and multiplies by zero.
                AP.clp_set(tok, 0, 1.0)
                tok2 = clap_fill!(ones(8))
                y = clap_out(AP.clp_process(tok2, -1, 0, -1, 0, -1, 0, -1, 0))
                @test all(iszero, y)
            end

            @testset "clp_expect refuses the wrong plugin" begin
                # The host holds one plugin at a time and the driver is what opens
                # it, so a model built for one plugin has to be able to say so.
                clap_open!(BUNDLE; plugin_id = "ap.onepole", block_size = 8, channels = 1)
                @test clap_plugin_index() == 1
                tok = clap_fill!(ones(8))
                @test AP.clp_expect(tok, 1) == tok
                @test isnan(AP.clp_expect(tok, 0))
                @test isnan(AP.clp_expect(tok, 2))
                @test isnan(AP.clp_expect(NaN, 1))
                # And the refusal reaches the output rather than stopping at the guard.
                @test isnan(AP.clp_process(AP.clp_expect(tok, 0), 0, 0.25, -1, 0, -1, 0, -1, 0))
                clap_close!()
                @test clap_plugin_index() == -1
                @test isnan(AP.clp_expect(1.0, 0))
            end
        end

        @testset "closing is clean and idempotent" begin
            clap_close!()
            @test !clap_is_open()
            clap_close!()
            @test !clap_is_open()
            @test isnan(AP.clp_process(1.0, 0, 1.0, -1, 0, -1, 0, -1, 0))
        end

    end
end # clap_host_available()
