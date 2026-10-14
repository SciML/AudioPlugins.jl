module AudioPluginsJuliaCExt

using AudioPlugins: AudioPlugins, PluginFormat, PluginSpec, JuliaStep, julia_step_header,
    emit_wrapper, place_library, runtime_layout, _with_pars, _resolve_compiler,
    _run, VENDOR_DIR
using JuliaC: JuliaC

const FORWARD_SHIM = normpath(joinpath(VENDOR_DIR, "..", "clap_forward_shim.c"))

# JuliaC's privatisation (salted library names, rewritten imports) is Unix
# only; on Windows two bundles in one process abort on the second load.
# The issue carries a standalone reproducer.
const WINDOWS_PRIVATIZE_URL = "https://github.com/JuliaLang/JuliaC.jl/issues/186"

function AudioPlugins._export_julia_step(
        format::PluginFormat, spec::PluginSpec,
        out::AbstractString; compiler, verbose::Bool
    )
    isdefined(JuliaC, :ImageRecipe) ||
        error(
        "export_plugin: JuliaC $(pkgversion(JuliaC)) only builds on Julia ≥ 1.12; " *
            "this is Julia $VERSION"
    )
    step = spec.step::JuliaStep
    if Sys.iswindows()
        step.bundle ||
            error(
            "export_plugin: on Windows a Julia step needs bundle = true: the loader has " *
                "no rpath, so the plugin is a shim that loads the runtime shipped beside it"
        )
        step.privatize == false ||
            error(
            "export_plugin: privatize is not available on Windows yet: JuliaC salts " *
                "the runtime on Linux and macOS only, see $WINDOWS_PRIVATIZE_URL"
        )
    end
    reflected = julia_step_header(spec)
    spec = _with_pars(spec, reflected.pars)
    cc = _resolve_compiler(compiler)
    layout = runtime_layout(format, out)
    mktempdir() do dir
        write(joinpath(dir, spec.base * ".h"), reflected.header)
        wrapper = emit_wrapper(format, spec, dir)
        cflags = ["-I" * d for d in [wrapper.include_dirs; dir]]
        append!(cflags, ["-fvisibility=hidden", "-Wall", "-Wextra", "-Werror"])
        image = JuliaC.ImageRecipe(;
            output_type = "--output-lib", file = step.file,
            project = step.project, trim_mode = step.trim,
            add_ccallables = true, c_sources = wrapper.sources, cflags,
            verbose, quiet = !verbose
        )
        # Windows has no rpath; JuliaC's @bundle stands for "the loader's default" there.
        rpath = !step.bundle ? JuliaC.RPATH_JULIA : Sys.iswindows() ? JuliaC.RPATH_BUNDLE : layout.rpath
        link = JuliaC.LinkRecipe(;
            image_recipe = image, outname = joinpath(dir, "lib" * spec.base),
            rpath
        )
        withenv("JULIA_CC" => _julia_cc(cc)) do
            JuliaC.compile_products(image)
            JuliaC.link_products(link)
            if step.bundle
                _clear_runtime(layout.dir)
                JuliaC.bundle_products(
                    JuliaC.BundleRecipe(;
                        link_recipe = link,
                        output_dir = layout.dir,
                        privatize = step.privatize
                    )
                )
            end
        end
        if Sys.iswindows()
            # bundle_products moved the plugin into <layout.dir>\bin; the .clap
            # is a shim that loads it from there (csrc/clap_forward_shim.c).
            inner = joinpath(layout.dir, "bin", basename(link.outname))
            isfile(inner) ||
                error(
                "export_plugin: expected the bundled plugin at $(repr(inner)), found " *
                    "$(repr(link.outname)); this JuliaC's Windows bundle layout is not the one " *
                    "the shim is built for"
            )
            _build_forward_shim(format, spec, cc, first(splitext(basename(inner))), dir, out, verbose)
        else
            place_library(format, spec, link.outname, out)
        end
    end
    return out
end

# JuliaC reads JULIA_CC with Base.shell_split, for which a backslash is an
# escape: a Windows path must go through with forward slashes, and quoted
# in case it has spaces.
_julia_cc(cc::AbstractString) = Sys.iswindows() ? "\"" * replace(cc, '\\' => '/') * "\"" : cc

function _build_forward_shim(
        format::PluginFormat, spec::PluginSpec, cc::AbstractString,
        inner_base::AbstractString, dir::AbstractString,
        out::AbstractString, verbose::Bool
    )
    occursin(r"^[A-Za-z_][A-Za-z0-9_]*$", inner_base) ||
        error("export_plugin: the inner library name $(repr(inner_base)) is not a C identifier")
    shim = joinpath(dir, "shim." * Base.BinaryPlatforms.platform_dlext())
    _run(`$cc -std=gnu99 -O2 -fvisibility=hidden -Wall -Wextra -Werror -isystem $VENDOR_DIR
          -DAP_INNER_BASE=$inner_base -shared -o $shim $FORWARD_SHIM -Wl,--no-undefined`, verbose)
    return place_library(format, spec, shim, out)
end

# A previous export's runtime for the same plugin is replaced; anything
# else at that path is left alone and reported. JuliaC lays the runtime
# out under lib/julia on Linux and macOS and flat under bin/ on Windows.
function _clear_runtime(dir::AbstractString)
    ispath(dir) || return
    isdir(joinpath(dir, "lib", "julia")) || isfile(joinpath(dir, "bin", "libjulia.dll")) ||
        error("export_plugin: $(repr(dir)) exists and is not a bundled Julia runtime; remove it first")
    rm(dir; recursive = true, force = true)
    return
end

end # module
