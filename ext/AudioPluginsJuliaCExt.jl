module AudioPluginsJuliaCExt

using AudioPlugins, JuliaC
using AudioPlugins: PluginFormat, PluginSpec, JuliaStep, julia_step_header, emit_wrapper,
                    place_library, runtime_layout, _with_pars, _resolve_compiler

function AudioPlugins._export_julia_step(format::PluginFormat, spec::PluginSpec,
                                         out::AbstractString; compiler, verbose::Bool)
    isdefined(JuliaC, :ImageRecipe) ||
        error("export_plugin: JuliaC $(pkgversion(JuliaC)) only builds on Julia ≥ 1.12; " *
              "this is Julia $VERSION")
    Sys.iswindows() &&
        error("export_plugin: a JuliaStep is not supported on Windows yet: the loader has no " *
              "rpath, so the plugin could not find the Julia runtime it links against")
    step = spec.step::JuliaStep
    reflected = julia_step_header(spec)
    spec = _with_pars(spec, reflected.pars)
    cc = _resolve_compiler(compiler)
    layout = runtime_layout(format, out)
    mktempdir() do dir
        write(joinpath(dir, spec.base * ".h"), reflected.header)
        wrapper = emit_wrapper(format, spec, dir)
        cflags = ["-I" * d for d in [wrapper.include_dirs; dir]]
        push!(cflags, "-fvisibility=hidden -Wall -Wextra -Werror")
        image = JuliaC.ImageRecipe(; output_type = "--output-lib", file = step.file,
                                   project = step.project, trim_mode = step.trim,
                                   add_ccallables = true, c_sources = wrapper.sources, cflags,
                                   verbose, quiet = !verbose)
        link = JuliaC.LinkRecipe(; image_recipe = image, outname = joinpath(dir, "lib" * spec.base),
                                 rpath = step.bundle ? layout.rpath : JuliaC.RPATH_JULIA)
        withenv("JULIA_CC" => cc) do
            JuliaC.compile_products(image)
            JuliaC.link_products(link)
            if step.bundle
                _clear_runtime(layout.dir)
                JuliaC.bundle_products(JuliaC.BundleRecipe(; link_recipe = link,
                                                            output_dir = layout.dir))
            end
        end
        place_library(format, spec, link.outname, out)
    end
    return out
end

# A previous export's runtime for the same plugin is replaced; anything
# else at that path is left alone and reported.
function _clear_runtime(dir::AbstractString)
    ispath(dir) || return
    isdir(joinpath(dir, "lib", "julia")) ||
        error("export_plugin: $(repr(dir)) exists and is not a bundled Julia runtime; remove it first")
    rm(dir; recursive = true, force = true)
    return
end

end # module
