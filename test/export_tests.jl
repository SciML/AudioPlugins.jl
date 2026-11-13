# Authoring, proved with no code generator anywhere: the plugins built here
# come from the hand-written step functions under test/export/, and are then
# hosted by the same CLAP host the rest of the suite uses. If the seam is
# real, nothing on this path needs to know what generated the C.

using Test
using AudioPlugins
using JuliaC
const AP = AudioPlugins

include("pkgconfig_paths.jl")

const FIX = joinpath(@__DIR__, "export")

include(joinpath(FIX, "reference.jl"))

# The generated header asserts Julia's own `sizeof`, and that is not the same on
# every target: i686 aligns Float64 to 4, x86_64 to 8, so JlGainPars is 12 bytes
# on one and 16 on the other. Mirror the struct instead of hardcoding either.
struct JlGainParsLayout
    gain::Float64
    bypass::Bool
end

# On Windows a Julia step ships its runtime (the .clap is a shim over
# Name.clap.runtime\bin, see the extension), so the fixtures are bundled
# there and built plain, against the running Julia, elsewhere.
function bundled_on_windows(spec)
    Sys.iswindows() || return spec
    s = spec.step::JuliaStep
    return PluginSpec(;
        id = spec.id, name = spec.name, vendor = spec.vendor, version = spec.version,
        description = spec.description, url = spec.url, features = spec.features,
        channels = spec.channels, base = spec.base, inputs = spec.inputs,
        output = spec.output, sub_clock = spec.sub_clock,
        sample_rate_field = spec.sample_rate_field, params = spec.params,
        constants = spec.constants,
        step = JuliaStep(; file = s.file, project = s.project, trim = s.trim, bundle = true)
    )
end

# A PATH with nothing of Julia's on it, for hosting a bundled plugin the way
# a DAW would: on CI the building Julia is on PATH, and its bin directory
# would satisfy the runtime's imports and hide a bundle that does not.
julia_free_path() = join(
    filter(p -> !occursin("julia", lowercase(p)), split(ENV["PATH"], Sys.iswindows() ? ';' : ':')),
    Sys.iswindows() ? ';' : ':'
)

function process(x, params...)
    t = clap_fill!(x)
    slots = fill(-1.0, 8)
    for (k, (id, v)) in enumerate(params)
        slots[2k - 1], slots[2k] = id, v
    end
    o = AP.clp_process(t, slots...)
    isnan(o) && error("process failed: $(clap_last_error())")
    return clap_out(o)
end

@testset "AudioPlugins / export" begin

    gain_spec = read_plugin_spec(joinpath(FIX, "fx_gain.toml"))
    eq_spec = read_plugin_spec(joinpath(FIX, "fx_eq.toml"))

    @testset "the descriptor reads back" begin
        @test gain_spec.id == "org.sciml.audioplugins.fixture.gain"
        @test gain_spec.base == "fx_gain" && gain_spec.pars == "FxGainPars"
        @test [i.role for i in gain_spec.inputs] == [:audio, :clock]
        @test gain_spec.output == "y" && gain_spec.channels == 2
        @test gain_spec.features == ["audio-effect", "utility"]
        @test [p.id for p in gain_spec.params] == [0, 7]
        @test gain_spec.params[2].ctype == "bool" && gain_spec.params[2].stepped
        @test gain_spec.step.pkgconfig === nothing
        @test isabspath(gain_spec.step.source) && isfile(gain_spec.step.source)
        @test eq_spec.sample_rate_field == "fs"
        @test eq_spec.constants == ["enabled" => true]
        @test endswith(eq_spec.step.pkgconfig, "fx_eq.pc")
        @test plugin_format("clap") === CLAP()
    end

    @testset "the descriptor is validated" begin
        p(; kw...) = PluginParam(; id = 0, name = "P", field = "p", min = 0, max = 1, default = 0, kw...)
        @test_throws ArgumentError PluginParam(; id = 0, name = "P", field = "1p", min = 0, max = 1, default = 0)
        @test_throws ArgumentError PluginParam(; id = 0, name = "P", field = "p", min = 0, max = 1, default = 2)
        @test_throws ArgumentError PluginParam(; id = 0, name = "P", field = "p", min = 1, max = 0, default = 0)
        @test_throws ArgumentError PluginParam(; id = 0, name = "P", field = "p", min = 0, max = Inf, default = 0)
        @test_throws ArgumentError p(ctype = "float")
        @test_throws ArgumentError StepInput("u", :midi)
        spec(; kw...) = PluginSpec(;
            id = "a", name = "b", base = "c", pars = "P",
            source = gain_spec.step.source, header = gain_spec.step.header, kw...
        )
        @test spec() isa PluginSpec
        @test_throws ArgumentError spec(inputs = [StepInput("c", :clock)])
        @test_throws ArgumentError spec(inputs = [StepInput("u", :audio), StepInput("v", :audio)])
        @test_throws ArgumentError spec(params = [p(), p()])
        @test_throws ArgumentError spec(sample_rate_field = "p", params = [p()])
        @test_throws ArgumentError spec(sample_rate_field = "k", constants = ["k" => 1.0])
        @test_throws ArgumentError spec(source = "/nonexistent.c")
        @test_throws ArgumentError spec(base = "not an ident")
        @test_throws ArgumentError spec(constants = ["k" => "string"])
        @test_throws ArgumentError spec(channels = 0)
        @test_throws ArgumentError plugin_format("aax")
        @test_throws ArgumentError export_plugin(gain_spec, "out.vst3")
    end

    @testset "pkg-config files are read without pkg-config" begin
        mktempdir() do d
            pc = joinpath(d, "x.pc")
            write(
                pc, """
                prefix=/opt/x
                libdir=\${prefix}/lib   # a comment
                Name: x
                Cflags: -I\${prefix}/include -DX=1
                Libs: -L\${libdir} -lx
                Libs.private: -lm
                """
            )
            f = AP.pkgconfig_flags(pc)
            @test f.cflags == ["-I/opt/x/include", "-DX=1"]
            @test f.libs == ["-L/opt/x/lib", "-lx", "-lm"]
        end
    end

    @testset "relocatable .pc expands \${pcfiledir}" begin
        mktempdir() do d
            pc = joinpath(d, "g.pc")
            write(
                pc, """
                prefix=\${pcfiledir}

                Name: g
                Description: relocatable step
                Version: 0
                Cflags: -I\${prefix}
                Libs: -lm
                """
            )
            f = AP.pkgconfig_flags(pc)
            @test f.cflags == ["-I$(abspath(d))"]
            @test f.libs == ["-lm"]
        end
    end

    @testset "Windows-style backslash paths survive flag splitting" begin
        # A `\` before a non-metacharacter is literal, so Windows-style
        # prefixes keep their separators through flag splitting.
        mktempdir() do d
            pc = joinpath(d, "win.pc")
            write(
                pc, """
                prefix=C:\\Users\\me\\proj
                Name: win
                Cflags: -I\${prefix}\\include
                Libs: -L\${prefix}\\lib -lm
                """
            )
            f = AP.pkgconfig_flags(pc)
            @test f.cflags == ["-IC:\\Users\\me\\proj\\include"]
            @test f.libs == ["-LC:\\Users\\me\\proj\\lib", "-lm"]
        end
    end

    @testset "backslash escapes whitespace and quotes in .pc flags" begin
        mktempdir() do d
            pc = joinpath(d, "esc.pc")
            write(
                pc, """
                Name: esc
                Cflags: -DX=a\\ b -DFOO=\\"bar\\" -I/opt/my\\ dir/include -D"a\\\\b" -D"foo\\"bar"
                Libs: -lm
                """
            )
            f = AP.pkgconfig_flags(pc)
            @test f.cflags == [
                "-DX=a b",
                "-DFOO=\"bar\"",
                "-I/opt/my dir/include",
                "-Da\\b",
                "-Dfoo\"bar",
            ]
            @test f.libs == ["-lm"]
        end
    end

    @testset "double-quoted backslash keeps space and single quote" begin
        mktempdir() do d
            pc = joinpath(d, "dq.pc")
            write(
                pc, """
                Name: dq
                Cflags: -D"a\\ b" -D"a\\'b"
                Libs: -lm
                """
            )
            f = AP.pkgconfig_flags(pc)
            @test f.cflags == ["-Da\\ b", "-Da\\'b"]
            @test f.libs == ["-lm"]
        end
    end

    @testset "empty quoted .pc flag argument is kept" begin
        mktempdir() do d
            pc = joinpath(d, "emptyq.pc")
            write(
                pc, """
                Name: emptyq
                Cflags: -I "" /opt
                Libs: -lm
                """
            )
            f = AP.pkgconfig_flags(pc)
            @test f.cflags == ["-I", "", "/opt"]
        end
    end

    @testset "unclosed quote in .pc flags is refused" begin
        mktempdir() do d
            pc = joinpath(d, "unclosed.pc")
            write(
                pc, """
                Name: unclosed
                Cflags: -I"/opt/my dir
                Libs: -lm
                """
            )
            err = @test_throws ArgumentError AP.pkgconfig_flags(pc)
            msg = sprint(showerror, err.value)
            @test occursin(pc, msg)
            @test occursin("quote", msg)
        end
    end

    @testset "detached -I/-L path form is kept" begin
        mktempdir() do d
            pc = joinpath(d, "detached.pc")
            write(
                pc, """
                inc=/opt/inc
                Name: detached
                Cflags: -I \${inc} -DX=1
                Libs: -L /opt/lib -lx
                """
            )
            f = AP.pkgconfig_flags(pc)
            @test f.cflags == ["-I", "/opt/inc", "-DX=1"]
            @test f.libs == ["-L", "/opt/lib", "-lx"]
        end
    end

    @testset "bare -I/-L without a path is refused" begin
        mktempdir() do d
            pc = joinpath(d, "bad.pc")
            write(
                pc, """
                empty=
                Name: bad
                Cflags: -I\${empty}
                Libs: -lm
                """
            )
            err = @test_throws ArgumentError AP.pkgconfig_flags(pc)
            msg = sprint(showerror, err.value)
            @test occursin(pc, msg)
            @test occursin("-I", msg)
            @test occursin("no path argument", msg)
        end
    end

    @testset "undefined .pc variable is refused" begin
        mktempdir() do d
            pc = joinpath(d, "typo.pc")
            write(
                pc, """
                Name: typo
                Cflags: -I\${missing}/include
                Libs: -lm
                """
            )
            err = @test_throws ArgumentError AP.pkgconfig_flags(pc)
            msg = sprint(showerror, err.value)
            @test occursin("missing", msg)
            @test occursin(pc, msg)
        end
    end

    @testset "explicitly empty .pc variable expands to empty" begin
        mktempdir() do d
            pc = joinpath(d, "emptyvar.pc")
            write(
                pc, """
                empty=
                Name: emptyvar
                Cflags: -I\${empty}/include
                Libs: -lm
                """
            )
            f = AP.pkgconfig_flags(pc)
            @test f.cflags == ["-I/include"]
            @test f.libs == ["-lm"]
        end
    end

    @testset "the rendered wrapper" begin
        mktempdir() do d
            w = AP.emit_wrapper(CLAP(), gain_spec, d)
            src = read(only(w.sources), String)
            @test !occursin(r"@[A-Z_]+@", src)
            @test occursin("#include \"fx_gain.h\"", src)
            @test occursin("fx_gain_step(x, true, &s->pars, &s->mem[c])", src)
            @test occursin("case 0: s->pars.gain = v; break;", src)
            @test occursin("case 1: s->pars.bypass = (v != 0.0); break;", src)
            @test occursin("CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED", src)
            @test occursin("\"audio-effect\", \"utility\",", src)
            e = read(only(AP.emit_wrapper(CLAP(), eq_spec, d).sources), String)
            @test occursin("fx_eq_step(x, &s->pars, &s->mem[c])", e)
            @test occursin("s->pars.fs = sr;", e)
            @test occursin("s->pars.enabled = true;", e)
        end
    end

    dir = mktempdir()
    gain = export_plugin(gain_spec, joinpath(dir, "fx_gain.clap"))
    eq = export_plugin(eq_spec, joinpath(dir, "fx_eq.clap"))

    f0, q, gdb = 1000.0, 0.7071067811865476, 6.0
    eq_params = ((0, f0), (1, q), (2, gdb))
    # The host stores samples as float32, so the reference can only be met to
    # one float32 ulp of the output (about 6e-8 relative); 1e-6 absolute is
    # well inside that for a signal of unit magnitude and would catch a wrong
    # coefficient or a lost state term.
    eq_tol = 1.0e-6

    # A C process over csrc/clap_host.c hosts what this package built. It
    # needs no CLAPHost_jll, so it runs where the JLL has no build, and no
    # Julia, so it can load a juliac plugin, which brings a libjulia this
    # process already has.
    probe = joinpath(dir, "probe_step")
    let cc = AP._c_compiler(), host = clap_src_path(), src = joinpath(FIX, "probe_step.c")
        dl = Sys.islinux() ? ["-ldl"] : String[]
        run(`$cc $(AP._c_arch_flags()) -O2 -Wall -Wextra -o $probe $src $host $dl -lm`)
    end
    function probe_run(bundle, x, block; sr = 48000, params = ())
        args = [bundle, string(sr), string(block), string(length(x) ÷ block)]
        for (id, v) in params
            push!(args, string(id), repr(Float64(v)))
        end
        input = join((repr(Float64(v)) for v in x), "\n") * "\n"
        out = read(pipeline(`$probe $args`; stdin = IOBuffer(input)), String)
        lines = split(out, r"\r?\n"; keepempty = false)      # Windows stdout is CRLF
        meta = String[l for l in lines if startswith(l, "#")]
        y = [parse(Float64, l) for l in lines if !startswith(l, "#")]
        return (; meta, y)
    end

    function probe_suite(kind, spec, gain_bundle, eq_bundle)
        @testset "$kind, from a C host: the bundle enumerates as described" begin
            @test ispath(gain_bundle) && ispath(eq_bundle)
            r = probe_run(gain_bundle, signal(64), 64)
            @test "# plugins 1" in r.meta
            @test "# plugin $(spec.id)|$(spec.name)" in r.meta
            @test "# name $(spec.name)" in r.meta
            @test "# params 2" in r.meta
            @test "# param 0|Gain|0|4|1" in r.meta
            @test "# param 7|Bypass|0|1|0" in r.meta
            @test length(r.y) == 64
        end

        @testset "$kind, from a C host: gain at 0.5, y == x * 0.5 sample-exactly" begin
            x = signal(64)
            @test probe_run(gain_bundle, x, 64; params = ((0, 0.5),)).y == x .* 0.5
            @test probe_run(gain_bundle, x, 64).y == x                          # default gain 1
            @test probe_run(gain_bundle, x, 64; params = ((0, 0.5), (7, 1.0))).y == x  # bypassed
            @test probe_run(gain_bundle, x, 64; params = ((0, 9.0),)).y == x .* 4      # clamped
        end

        @testset "$kind, from a C host: EQ matches RBJ, 2 x 128 == 1 x 256, three rates" begin
            x = signal(256)
            whole = probe_run(eq_bundle, x, 256; params = eq_params).y
            @test maximum(abs.(whole .- rbj_peaking(x, 48000, f0, q, gdb))) < eq_tol
            @test maximum(abs.(whole .- x)) > 1.0e-2
            split = probe_run(eq_bundle, x, 128; params = eq_params).y   # two blocks, one instance
            @test split == whole
            restarted = vcat(
                probe_run(eq_bundle, x[1:128], 128; params = eq_params).y,
                probe_run(eq_bundle, x[129:256], 128; params = eq_params).y
            )
            @test restarted != whole
            outs = [
                probe_run(eq_bundle, x, 256; sr = fs, params = eq_params).y
                    for fs in (44100, 48000, 96000)
            ]
            for (fs, y) in zip((44100, 48000, 96000), outs)
                @test maximum(abs.(y .- rbj_peaking(x, fs, f0, q, gdb))) < eq_tol
            end
            @test outs[1] != outs[2] && outs[2] != outs[3]
        end
    end

    probe_suite("C step", gain_spec, gain, eq)

    decim_spec = read_plugin_spec(joinpath(FIX, "fx_decim.toml"))
    decim = export_plugin(decim_spec, joinpath(dir, "fx_decim.clap"))

    # In-process hosting, through CLAPHost_jll: only where the JLL has a build.
    if clap_host_available()
        @testset "the bundle enumerates as described" begin
            @test ispath(gain) && ispath(eq)
            @test !startswith(gain, pkgdir(AudioPlugins))
            plugs = clap_scan(gain)
            @test length(plugs) == 1
            @test plugs[1].id == gain_spec.id && plugs[1].name == "Fixture Gain"
            clap_open!(
                gain; plugin_id = gain_spec.id, sample_rate = 48000, block_size = 64,
                channels = 2
            )
            @test clap_plugin_name() == "Fixture Gain"
            @test clap_latency() == 0.0
            ps = clap_params()
            @test [p.id for p in ps] == [0.0, 7.0]
            @test ps[1].name == "Gain" && (ps[1].min, ps[1].max, ps[1].default) == (0.0, 4.0, 1.0)
            @test ps[2].name == "Bypass" && (ps[2].min, ps[2].max, ps[2].default) == (0.0, 1.0, 0.0)
        end

        @testset "gain at 0.5: y == x * 0.5, sample-exactly, on both channels" begin
            x = signal(64)
            t = clap_fill!(x)
            o = AP.clp_process(t, 0, 0.5, -1, 0, -1, 0, -1, 0)
            @test clap_out(o; channel = 0) == x .* 0.5
            @test clap_out(o; channel = 1) == x .* 0.5
            @test maximum(abs.(clap_out(o) .- x .* 0.5)) == 0.0
            @test process(x, (0, 2.0)) == x .* 2
        end

        @testset "parameters are clamped, and a stepped bool rounds" begin
            x = signal(64)
            @test process(x, (0, 0.5), (7, 1.0)) == x            # bypassed
            @test AP.clap_param_value(7) == 1.0
            @test process(x, (0, 0.5), (7, 0.4)) == x .* 0.5     # 0.4 rounds to 0
            @test AP.clap_param_value(7) == 0.0
            @test process(x, (0, 9.0), (7, 0.0)) == x .* 4       # clamped to max
            @test AP.clap_param_value(0) == 4.0
        end

        @testset "peaking EQ matches the RBJ recursion" begin
            clap_open!(eq; plugin_id = eq_spec.id, sample_rate = 48000, block_size = 256)
            ps = clap_params()
            @test [p.id for p in ps] == [0.0, 1.0, 2.0]
            @test [p.default for p in ps] == [f0, q, gdb]
            x = signal(256)
            y = process(x, eq_params...)
            ref = rbj_peaking(x, 48000, f0, q, gdb)
            @test maximum(abs.(y .- ref)) < eq_tol
            @test maximum(abs.(y .- x)) > 1.0e-2      # the filter is doing something
        end

        @testset "2 x 128 frames == 1 x 256 continuous, bitwise" begin
            x = signal(256)
            clap_open!(eq; plugin_id = eq_spec.id, sample_rate = 48000, block_size = 128)
            split = vcat(process(x[1:128], eq_params...), process(x[129:256], eq_params...))
            clap_open!(eq; plugin_id = eq_spec.id, sample_rate = 48000, block_size = 256)
            whole = process(x, eq_params...)
            @test split == whole
            @test maximum(abs.(whole .- rbj_peaking(x, 48000, f0, q, gdb))) < eq_tol
            # Reopening is a fresh instance, so a run restarted at the boundary
            # must differ: that is what makes the equality above non-vacuous.
            clap_open!(eq; plugin_id = eq_spec.id, sample_rate = 48000, block_size = 128)
            a = process(x[1:128], eq_params...)
            clap_open!(eq; plugin_id = eq_spec.id, sample_rate = 48000, block_size = 128)
            b = process(x[129:256], eq_params...)
            @test vcat(a, b) != whole
        end

        @testset "activate(sr) at 44.1 / 48 / 96 kHz gives three correct filters" begin
            x = signal(256)
            outs = Vector{Float64}[]
            for fs in (44100, 48000, 96000)
                clap_open!(eq; plugin_id = eq_spec.id, sample_rate = fs, block_size = 256)
                y = process(x, eq_params...)
                @test maximum(abs.(y .- rbj_peaking(x, fs, f0, q, gdb))) < eq_tol
                push!(outs, y)
            end
            @test outs[1] != outs[2] && outs[2] != outs[3]
        end

        @testset "a mono descriptor and a Julia-side spec, no TOML" begin
            mono = PluginSpec(;
                id = "org.sciml.audioplugins.fixture.mono", name = "Mono Gain",
                base = "fx_gain", pars = "FxGainPars", channels = 1,
                inputs = [("u", :audio), ("clock1", :clock)],
                source = gain_spec.step.source, header = gain_spec.step.header,
                params = [
                    PluginParam(;
                        id = 3, name = "Gain", field = "gain",
                        min = 0, max = 2, default = 0.25
                    ),
                ]
            )
            b = export_plugin(mono, joinpath(dir, "mono.clap"))
            clap_open!(b; block_size = 32)
            @test clap_plugin_name() == "Mono Gain"
            @test only(clap_params()).id == 3.0
            x = signal(32)
            @test process(x) == x .* 0.25               # the default applies untouched
            @test process(x, (3, 2.0)) == x .* 2
        end

        @testset "a sub-clock output is held between ticks, sample-exactly" begin
            @test decim_spec.sub_clock
            w = read(only(AP.emit_wrapper(CLAP(), decim_spec, mktempdir()).sources), String)
            @test occursin("if (o.has_y) s->held[c] = o.y;", w)
            # The phase lives in the instance, so each run starts from a fresh one.
            fresh() = clap_open!(decim; plugin_id = decim_spec.id, sample_rate = 48000, block_size = 64)
            x = signal(64)
            fresh()
            @test process(x) == decimate_hold(x, 2, 1.0)
            fresh()
            @test process(x, (0, 0.5), (1, 3.0)) == decimate_hold(x, 3, 0.5)
            fresh()
            @test process(x, (0, 0.5), (1, 1.0)) == x .* 0.5      # divisor 1: every sample present
            fresh()
            @test !any(isnan, process(x, (0, 1.0), (1, 8.0)))   # NaN never leaks through the hold
        end

        @testset "the sub-clock phase carries across blocks" begin
            x = signal(96)
            clap_open!(decim; plugin_id = decim_spec.id, sample_rate = 48000, block_size = 32)
            split = vcat((process(x[(32b + 1):(32b + 32)], (0, 1.0), (1, 5.0)) for b in 0:2)...)
            clap_open!(decim; plugin_id = decim_spec.id, sample_rate = 48000, block_size = 96)
            whole = process(x, (0, 1.0), (1, 5.0))
            @test split == whole == decimate_hold(x, 5, 1.0)
            # 32 is not a multiple of 5, so a plugin that restarted its phase at
            # each block would tick at the block boundary and differ.
            clap_open!(decim; plugin_id = decim_spec.id, sample_rate = 48000, block_size = 32)
            restarted = Float64[]
            for b in 0:2
                clap_open!(decim; plugin_id = decim_spec.id, sample_rate = 48000, block_size = 32)
                append!(restarted, process(x[(32b + 1):(32b + 32)], (0, 1.0), (1, 5.0)))
            end
            @test restarted != whole
        end

        clap_close!()
    else
        @testset "no prebuilt host here: in-process hosting cannot run" begin
            @test_throws ErrorException clap_open!(gain; block_size = 64)
        end
    end

    @testset "link flags come from the .pc file" begin
        # Without fx_eq.pc's `Libs: -lm` the bundle has undefined sin/cos/pow,
        # and the link refuses it rather than leaving it for the host's dlopen.
        # macOS links libm through libSystem regardless, so only ELF can check.
        if Sys.islinux()
            nopc = PluginSpec(;
                id = eq_spec.id, name = eq_spec.name, base = eq_spec.base,
                pars = eq_spec.pars, source = eq_spec.step.source,
                header = eq_spec.step.header, inputs = eq_spec.inputs,
                sample_rate_field = "fs", params = eq_spec.params,
                constants = eq_spec.constants
            )
            @test_throws ProcessFailedException redirect_stderr(devnull) do
                export_plugin(nopc, joinpath(dir, "fx_eq_nopc.clap"))
            end
        end
    end

    @testset "the step source is validated" begin
        @test_throws ArgumentError CStep(; source = "/nonexistent.c", header = gain_spec.step.header)
        @test_throws ArgumentError JuliaStep(; file = "/nonexistent.jl")
        @test_throws ArgumentError JuliaStep(; file = joinpath(FIX, "jl_gain.jl"), trim = "maybe")
        @test_throws ArgumentError JuliaStep(; file = joinpath(FIX, "jl_gain.jl"), privatize = true)
        @test_throws ArgumentError JuliaStep(;
            file = joinpath(FIX, "jl_gain.jl"), bundle = true,
            privatize = "nine_long"
        )
        @test JuliaStep(; file = joinpath(FIX, "jl_gain.jl"), bundle = true, privatize = "ap").privatize == "ap"
        mktempdir() do d
            toml = read(joinpath(FIX, "jl_gain.toml"), String)
            # Forward slashes: a backslash in a TOML basic string is an escape.
            file = replace(joinpath(FIX, "jl_gain.jl"), '\\' => '/')
            toml = replace(
                toml, "julia = \"jl_gain.jl\"" =>
                    "julia = \"$file\"\nbundle = true\nprivatize = true"
            )
            write(joinpath(d, "p.toml"), toml)
            @test read_plugin_spec(joinpath(d, "p.toml")).step.privatize === true
        end
        @test_throws ArgumentError PluginSpec(;
            id = "a", name = "b", base = "c", pars = "P",
            step = gain_spec.step, source = gain_spec.step.source
        )
        @test_throws ArgumentError PluginSpec(;
            id = "a", name = "b", base = "c",
            step = gain_spec.step
        )      # a C step needs pars
        @test_throws ArgumentError PluginSpec(; id = "a", name = "b", base = "c")
    end

    # ----------------------------------------------------------------------
    # Julia steps: juliac-trimmed plugins, hosted from the C probe
    # ----------------------------------------------------------------------

    jl_gain_spec = read_plugin_spec(joinpath(FIX, "jl_gain.toml"))
    jl_eq_spec = read_plugin_spec(joinpath(FIX, "jl_eq.toml"))

    @testset "a Julia step's header is generated from its @ccallable signature" begin
        @test jl_gain_spec.step isa JuliaStep
        @test jl_gain_spec.pars == ""
        h = AP.julia_step_header(jl_gain_spec)
        @test h.pars == "JlGainPars"
        @test occursin("struct JlGainPars {\n    double gain;\n    bool bypass;\n};", h.header)
        @test occursin("_Static_assert(sizeof(JlGainPars) == $(sizeof(JlGainParsLayout)),", h.header)
        @test occursin("_Static_assert(offsetof(JlGainPars, bypass) == 8,", h.header)
        @test occursin("typedef JlGainMem jl_gain_mem;", h.header)
        @test occursin("typedef JlGainOut jl_gain_out;", h.header)
        @test occursin(
            "JlGainOut jl_gain_step(double u, bool clock1, JlGainPars *pars, JlGainMem *self);",
            h.header
        )
        @test occursin("void jl_gain_reset(JlGainMem *self);", h.header)
        e = AP.julia_step_header(jl_eq_spec)
        @test e.pars == "JlEqPars"
        @test occursin("double x1;\n    double x2;\n    double y1;\n    double y2;", e.header)

        # And it is checked against the descriptor.
        respec(spec; kw...) = PluginSpec(;
            id = spec.id, name = spec.name, base = spec.base,
            step = spec.step, inputs = spec.inputs,
            params = spec.params, kw...
        )
        @test_throws ArgumentError AP.julia_step_header(respec(jl_gain_spec; output = "z"))
        @test_throws ArgumentError AP.julia_step_header(respec(jl_gain_spec; pars = "Other"))
        @test_throws ArgumentError AP.julia_step_header(respec(jl_gain_spec; inputs = [StepInput("u", :audio)]))
        @test_throws ArgumentError AP.julia_step_header(respec(jl_gain_spec; sample_rate_field = "fs"))
        @test_throws ArgumentError AP.julia_step_header(respec(jl_gain_spec; constants = ["nope" => 1]))
        @test_throws ArgumentError AP.julia_step_header(
            respec(
                jl_gain_spec;
                params = [
                    PluginParam(;
                        id = 0, name = "G", field = "gain", min = 0, max = 1, default = 0,
                        ctype = "bool"
                    ),
                ]
            )
        )
        @test_throws ArgumentError AP.julia_step_header(respec(jl_gain_spec; base = "fx_gain"))
        @test_throws ArgumentError AP.julia_step_header(gain_spec)
    end

    juliac_ready = VERSION >= v"1.12" && isdefined(JuliaC, :ImageRecipe)

    if !juliac_ready
        @testset "a Julia step needs Julia >= 1.12" begin
            @test_throws ErrorException export_plugin(jl_gain_spec, joinpath(dir, "jl_gain.clap"))
        end
    else
        jl_gain = export_plugin(bundled_on_windows(jl_gain_spec), joinpath(dir, "jl_gain.clap"))
        jl_eq = export_plugin(bundled_on_windows(jl_eq_spec), joinpath(dir, "jl_eq.clap"))
        probe_suite("Julia step", jl_gain_spec, jl_gain, jl_eq)

        if Sys.islinux() || Sys.iswindows()
            @testset "bundle = true ships the runtime next to the plugin" begin
                spec = PluginSpec(;
                    id = jl_eq_spec.id, name = jl_eq_spec.name, base = jl_eq_spec.base,
                    step = JuliaStep(; file = jl_eq_spec.step.file, bundle = true),
                    inputs = jl_eq_spec.inputs, params = jl_eq_spec.params,
                    sample_rate_field = "fs", constants = jl_eq_spec.constants
                )
                out = joinpath(dir, "shipped", "jl_eq.clap")
                @test export_plugin(spec, out) == out
                layout = AP.runtime_layout(CLAP(), out)
                # JuliaC's layout: lib/julia on Linux, everything flat in bin on Windows.
                runtime_shipped() = Sys.iswindows() ? isfile(joinpath(layout.dir, "bin", "libjulia.dll")) :
                    isdir(joinpath(layout.dir, "lib", "julia"))
                @test runtime_shipped()
                @test any(startswith("libjulia"), readdir(joinpath(layout.dir, Sys.iswindows() ? "bin" : "lib")))
                if Sys.islinux()
                    @test !isfile(joinpath(layout.dir, "lib", "libjl_eq.so"))   # moved into the .clap
                    # The rpath, not the build machine's Julia, is what finds the runtime.
                    runpath = read(`readelf -d $out`, String)
                    @test occursin("\$ORIGIN/jl_eq.clap.runtime/lib", runpath)
                    @test !occursin(Sys.BINDIR, runpath)
                else
                    # The .clap is the shim: tens of KB, no Julia in it, no path
                    # baked in. The plugin is the DLL beside the runtime.
                    @test filesize(out) < 512 * 1024
                    @test !occursin(Sys.BINDIR, String(read(out)))
                    @test isfile(joinpath(layout.dir, "bin", "libjl_eq.dll"))
                end
                x = signal(256)
                # Hosted with nothing of Julia's on PATH, as a DAW would.
                y = withenv("PATH" => julia_free_path()) do
                    probe_run(out, x, 256; params = eq_params).y
                end
                @test maximum(abs.(y .- rbj_peaking(x, 48000, f0, q, gdb))) < eq_tol
                # Exporting again over the same path replaces the runtime.
                @test export_plugin(spec, out) == out
                @test runtime_shipped()
            end
        end

        @testset "a juliac sub-clock output is held between ticks" begin
            jl_decim_spec = read_plugin_spec(joinpath(FIX, "jl_decim.toml"))
            h = AP.julia_step_header(jl_decim_spec)
            @test occursin("double y;\n    bool has_y;", h.header)
            @test_throws ArgumentError AP.julia_step_header(
                PluginSpec(;
                    id = jl_gain_spec.id, name = jl_gain_spec.name, base = jl_gain_spec.base,
                    step = jl_gain_spec.step, inputs = jl_gain_spec.inputs, sub_clock = true
                )
            )
            jl_decim = export_plugin(bundled_on_windows(jl_decim_spec), joinpath(dir, "jl_decim.clap"))
            x = signal(96)
            @test probe_run(jl_decim, x, 96; params = ((0, 0.5), (1, 3.0))).y == decimate_hold(x, 3, 0.5)
            @test probe_run(jl_decim, x, 32; params = ((0, 1.0), (1, 5.0))).y == decimate_hold(x, 5, 1.0)
        end

        if !Sys.iswindows()
            @testset "privatize = true: two juliac plugins coexist in one process" begin
                probe_two = joinpath(dir, "probe_two")
                let cc = AP._c_compiler(), src = joinpath(FIX, "probe_two.c")
                    dl = Sys.islinux() ? ["-ldl"] : String[]
                    run(`$cc $(AP._c_arch_flags()) -O2 -Wall -Wextra -I$(AP.VENDOR_DIR) -o $probe_two $src $dl -lm`)
                end
                function shipped(spec, sub, privatize)
                    s = PluginSpec(;
                        id = spec.id, name = spec.name, base = spec.base,
                        inputs = spec.inputs, params = spec.params,
                        sample_rate_field = spec.sample_rate_field,
                        constants = spec.constants,
                        step = JuliaStep(; file = spec.step.file, bundle = true, privatize)
                    )
                    return export_plugin(s, joinpath(dir, sub, spec.base * ".clap"))
                end
                x = signal(128)
                input = join((repr(v) for v in x), "\n") * "\n"
                function both(a, b)
                    out = IOBuffer()
                    p = run(
                        pipeline(
                            ignorestatus(`$probe_two $a $b 48000 128 1 0 0.5`);
                            stdin = IOBuffer(input), stdout = out, stderr = devnull
                        )
                    )
                    lines = split(String(take!(out)), '\n'; keepempty = false)
                    ya = [parse(Float64, l[3:end]) for l in lines if startswith(l, "A ")]
                    yb = [parse(Float64, l[3:end]) for l in lines if startswith(l, "B ")]
                    return (; ok = success(p), ya, yb)
                end

                gain_p = shipped(jl_gain_spec, "priv", true)
                eq_p = shipped(jl_eq_spec, "priv", true)
                r = both(gain_p, eq_p)
                @test r.ok
                @test r.ya == x .* 0.5
                @test maximum(abs.(r.yb .- rbj_peaking(x, 48000, f0, q, gdb))) < eq_tol
                if Sys.islinux()
                    # Each plugin names its own salted libjulia, and neither the stock one.
                    needed(b) = [m[1] for m in eachmatch(r"\[(\S*libjulia\.so\S*)\]", read(`readelf -d $b`, String))]
                    ng, ne = needed(gain_p), needed(eq_p)
                    @test length(ng) == 1 && length(ne) == 1
                    @test ng != ne
                    @test !any(startswith("libjulia."), ng) && !any(startswith("libjulia."), ne)
                    libs = readdir(joinpath(AP.runtime_layout(CLAP(), gain_p).dir, "lib"))
                    @test any(f -> endswith(f, "_libjulia.so"), libs)
                    @test !any(==("libjulia.so"), libs)
                end
                # Without privatisation both plugins want the one libjulia the loader
                # has already loaded, and the second runtime aborts the process.
                gain_n = shipped(jl_gain_spec, "plain", false)
                eq_n = shipped(jl_eq_spec, "plain", false)
                @test !both(gain_n, eq_n).ok
                @test both(gain_p, gain_p).ok             # the same plugin twice is fine
            end
        else
            @testset "on Windows a Julia step must be bundled, and cannot be privatised yet" begin
                # Both are refused before anything is built.
                plain = joinpath(dir, "jl_gain_plain.clap")
                err = try
                    export_plugin(jl_gain_spec, plain); nothing
                catch e
                    e
                end
                @test err isa ErrorException && occursin("bundle = true", err.msg)
                @test !ispath(plain) && !ispath(plain * ".runtime")
                priv = PluginSpec(;
                    id = jl_gain_spec.id, name = jl_gain_spec.name,
                    base = jl_gain_spec.base, inputs = jl_gain_spec.inputs,
                    params = jl_gain_spec.params,
                    step = JuliaStep(;
                        file = jl_gain_spec.step.file, bundle = true,
                        privatize = true
                    )
                )
                out = joinpath(dir, "jl_gain_priv.clap")
                err = try
                    export_plugin(priv, out); nothing
                catch e
                    e
                end
                @test err isa ErrorException && occursin("privatize", err.msg)
                @test occursin("github.com/JuliaLang/JuliaC.jl", err.msg)
                @test !ispath(out) && !ispath(out * ".runtime")
            end
        end
    end
end
