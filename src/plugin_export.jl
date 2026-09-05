# Authoring plugins: wrap a per-sample step function in a plugin format
# and build the bundle. The inverse of clap_io.jl, which hosts one.
#
# The interface is deliberately "a step function plus a descriptor" and
# names no code generator: anything that can emit
#
#     <base>_out <base>_step(<inputs...>, <Pars> *pars, <base>_mem *self);
#     void       <base>_reset(<base>_mem *self);
#
# can be built into a plugin. The step comes either as C source with a
# header (`CStep`) or as Julia `@ccallable` functions compiled to a trimmed
# library by juliac (`JuliaStep`), and the test suite proves the seam with
# hand-written fixtures of both kinds and no code generator in the loop.

using TOML

export PluginFormat, CLAP, PluginParam, StepInput, StepSource, CStep, JuliaStep, PluginSpec,
       read_plugin_spec, export_plugin, register_plugin_format!, plugin_format

# ---------------------------------------------------------------------------
# Formats
# ---------------------------------------------------------------------------

"""
    PluginFormat

Abstract supertype of plugin formats [`export_plugin`](@ref) can build. A
format implements

  * `format_name(fmt) -> String`, the key under which it is registered;
  * `bundle_extension(fmt) -> String`, e.g. `".clap"`;
  * `emit_wrapper(fmt, spec, dir) -> (; sources, include_dirs)`, writing the
    format's wrapper C sources into `dir` — they are compiled with
    `-Wall -Wextra -Werror`;
  * `place_library(fmt, spec, library, out)`, moving one linked shared
    library into the bundle layout at `out`;
  * `runtime_layout(fmt, out) -> (; dir, rpath)`, where a bundled Julia
    runtime goes for the bundle at `out` and the rpath that finds it.

[`CLAP`](@ref) is the format that ships here. Formats whose SDKs cannot be
vendored in a public repository live out of tree, subtype this, and
[`register_plugin_format!`](@ref) themselves.
"""
abstract type PluginFormat end

"""
    CLAP()

The CLAP plugin format (MIT, header-only; the headers are vendored under
`csrc/vendor/clap`). A bundle is one shared object on Linux and Windows
(`.clap`), and a `Name.clap/Contents/MacOS/Name` directory on macOS.
"""
struct CLAP <: PluginFormat end

format_name(::CLAP) = "clap"
bundle_extension(::CLAP) = ".clap"

const PLUGIN_FORMATS = Dict{String, PluginFormat}()

"""
    register_plugin_format!(fmt::PluginFormat)

Make `fmt` available as `plugin_format(format_name(fmt))`. Registering a
name twice replaces the earlier format.
"""
function register_plugin_format!(fmt::PluginFormat)
    PLUGIN_FORMATS[format_name(fmt)] = fmt
    return fmt
end

"""
    plugin_format(name::AbstractString) -> PluginFormat

The registered format called `name` (`"clap"` ships with the package).
"""
function plugin_format(name::AbstractString)
    haskey(PLUGIN_FORMATS, name) && return PLUGIN_FORMATS[name]
    throw(ArgumentError("no plugin format registered as $(repr(name)); " *
                        "known: $(join(sort!(collect(keys(PLUGIN_FORMATS))), ", "))"))
end

# ---------------------------------------------------------------------------
# The descriptor
# ---------------------------------------------------------------------------

const PARAM_CTYPES = ("double", "bool", "int64_t")

"""
    PluginParam(; id, name, field, min, max, default,
                automatable = true, stepped = false, ctype = "double")

One plugin parameter: the `id` a host addresses it by, its display `name`
and range, and the `field` of the parameter struct it writes. `ctype` is
that field's C type (`"double"`, `"bool"` or `"int64_t"`); a value arriving
from the host is clamped to `[min, max]`, rounded when `stepped`, and
converted.
"""
struct PluginParam
    id::UInt32
    name::String
    field::String
    min::Float64
    max::Float64
    default::Float64
    automatable::Bool
    stepped::Bool
    ctype::String
    function PluginParam(; id, name, field, min, max, default,
                         automatable::Bool = true, stepped::Bool = false,
                         ctype::AbstractString = "double")
        _check_c_ident(field, "parameter field")
        ctype in PARAM_CTYPES ||
            throw(ArgumentError("parameter $(repr(name)): ctype must be one of " *
                                "$(join(PARAM_CTYPES, ", ")), got $(repr(ctype))"))
        all(isfinite, (min, max, default)) ||
            throw(ArgumentError("parameter $(repr(name)): min, max and default must be finite"))
        min <= max ||
            throw(ArgumentError("parameter $(repr(name)): min $min exceeds max $max"))
        min <= default <= max ||
            throw(ArgumentError("parameter $(repr(name)): default $default is outside [$min, $max]"))
        return new(UInt32(id), String(name), String(field), Float64(min), Float64(max),
                   Float64(default), automatable, stepped, String(ctype))
    end
end

"""
    StepInput(name, role)

One argument of the step function, in order. `role` is `:audio` for the
sample in (a `double`) or `:clock` for a "this clock ticked" flag (a `bool`,
always `true`: the wrapper calls the step once per sample).
"""
struct StepInput
    name::String
    role::Symbol
    function StepInput(name, role)
        role in (:audio, :clock) ||
            throw(ArgumentError("step input $(repr(name)): role must be :audio or :clock, got $(repr(role))"))
        return new(String(name), role)
    end
end

"""
    StepSource

Where a plugin's step function comes from: [`CStep`](@ref) for C source
with a header, [`JuliaStep`](@ref) for Julia `@ccallable`s compiled by
juliac.
"""
abstract type StepSource end

"""
    CStep(; source, header, pkgconfig = nothing, include_dirs = String[])

A step function written in C: `source` defines `<base>_step` and
`<base>_reset`, `header` declares them along with the parameter, state
and output structs. `pkgconfig` is the `.pc` file describing how to
compile and link `source`; its `Cflags` and `Libs` go on the command
lines, so libraries the C calls into reach the link line without being
hardcoded. `include_dirs` are extra include directories.
"""
struct CStep <: StepSource
    source::String
    header::String
    pkgconfig::Union{Nothing, String}
    include_dirs::Vector{String}
    function CStep(; source, header, pkgconfig = nothing, include_dirs = String[])
        for (label, path) in (("source", source), ("header", header), ("pkgconfig", pkgconfig))
            path === nothing && continue
            isfile(path) || throw(ArgumentError("$label $(repr(path)) is not a file"))
        end
        return new(abspath(source), abspath(header),
                   pkgconfig === nothing ? nothing : abspath(pkgconfig),
                   String[abspath(d) for d in include_dirs])
    end
end

"""
    JuliaStep(; file, project = "", trim = "safe", bundle = false)

A step function written in Julia and compiled to a trimmed library by
juliac. `file` defines `<base>_step` and `<base>_reset` as
`Base.@ccallable` functions over isbits structs:

```julia
struct GainPars; gain::Float64; bypass::Bool; end
struct GainMem; ticks::Int64; end
struct GainOut; y::Float64; end

Base.@ccallable function my_gain_step(u::Float64, pars::Ptr{GainPars}, self::Ptr{GainMem})::GainOut
    p = unsafe_load(pars)
    return GainOut(p.bypass ? u : p.gain * u)
end
Base.@ccallable function my_gain_reset(self::Ptr{GainMem})::Cvoid
    unsafe_store!(self, GainMem(0))
    return nothing
end
```

The parameter, state and output structs are read off the `@ccallable`
signature and declared to C by a generated header, so the descriptor
needs no `pars` name for a Julia step. `project` is the environment the
file is compiled in (the active one by default) and `trim` the juliac
trim mode.

Building needs `using JuliaC` and Julia ≥ 1.12, on Linux or macOS: Windows
has no rpath, so a plugin could not find its runtime, and a Julia step is
refused there. The plugin links against
`libjulia` at the building Julia's absolute path; with `bundle = true`
the runtime is copied next to the plugin and found by a relative rpath,
which is what makes the bundle relocatable. Either way the plugin brings
a Julia runtime with it, which has consequences the README spells out.
"""
struct JuliaStep <: StepSource
    file::String
    project::String
    trim::String
    bundle::Bool
    function JuliaStep(; file, project = "", trim = "safe", bundle::Bool = false)
        isfile(file) || throw(ArgumentError("file $(repr(file)) is not a file"))
        project == "" || isdir(project) || isfile(project) ||
            throw(ArgumentError("project $(repr(project)) does not exist"))
        trim in ("safe", "unsafe", "unsafe-warn", "no") ||
            throw(ArgumentError("trim must be safe, unsafe, unsafe-warn or no, got $(repr(trim))"))
        return new(abspath(file), project == "" ? "" : abspath(project), String(trim), bundle)
    end
end

"""
    PluginSpec(; id, name, base, step, kwargs...)

Everything [`export_plugin`](@ref) needs, and nothing about where the
step function came from. Required:

  * `id`, `name` — the plugin's identifier (reverse-DNS by convention) and display name;
  * `base` — the ABI base name: the step function is `<base>_step`, its
    state `<base>_mem`, its result `<base>_out`, and `<base>_reset` clears the state;
  * `step` — a [`CStep`](@ref) or [`JuliaStep`](@ref). As a shorthand for
    a C step, `source`, `header`, `pkgconfig` and `include_dirs` may be
    given directly;
  * `pars` — the name of the parameter struct type declared in the C
    header. Required for a `CStep`; derived from the `@ccallable` signature
    for a `JuliaStep`.

Optional:

  * `vendor`, `version`, `description`, `url` — descriptor strings;
  * `features` — CLAP feature strings, default `["audio-effect"]`;
  * `channels` — channels per port, default `2`; one `<base>_mem` per channel;
  * `inputs` — the step arguments before `pars`, as [`StepInput`](@ref)s;
    default one `:audio` input. Exactly one must be `:audio`;
  * `output` — the field of `<base>_out` carrying the sample, default `"y"`;
  * `sub_clock` — `true` when the output is on a clock slower than the
    sample clock: `<base>_out` then also carries a `bool has_<output>`
    presence flag, and on a sample where it is false the wrapper holds the
    last present value per channel (zero until the first). Default `false`;
  * `sample_rate_field` — a field of the parameter struct to write the
    host's sample rate into on activate, so one bundle serves every rate;
  * `params` — [`PluginParam`](@ref)s;
  * `constants` — `field => value` pairs written into the parameter struct
    once at instantiation, for fields that are not plugin parameters.

[`read_plugin_spec`](@ref) builds one from a TOML file.
"""
struct PluginSpec
    id::String
    name::String
    vendor::String
    version::String
    description::String
    url::String
    features::Vector{String}
    channels::Int
    base::String
    pars::String
    inputs::Vector{StepInput}
    output::String
    sub_clock::Bool
    sample_rate_field::Union{Nothing, String}
    params::Vector{PluginParam}
    constants::Vector{Pair{String, Any}}
    step::StepSource
end

function PluginSpec(; id, name, base, step = nothing, pars = "",
                    source = nothing, header = nothing, pkgconfig = nothing,
                    include_dirs = String[],
                    vendor = "", version = "0.0.0", description = "", url = "",
                    features = ["audio-effect"], channels::Integer = 2,
                    inputs = [StepInput("u", :audio)], output = "y", sub_clock::Bool = false,
                    sample_rate_field = nothing, params = PluginParam[],
                    constants = Pair{String, Any}[])
    isempty(id) && throw(ArgumentError("plugin id must not be empty"))
    isempty(name) && throw(ArgumentError("plugin name must not be empty"))
    1 <= channels <= 64 || throw(ArgumentError("channels must be in 1..64, got $channels"))
    _check_c_ident(base, "ABI base name")
    _check_c_ident(output, "output field")
    sample_rate_field === nothing || _check_c_ident(sample_rate_field, "sample_rate_field")
    if step === nothing
        (source === nothing || header === nothing) &&
            throw(ArgumentError("give a `step` (CStep or JuliaStep), or `source` and `header` for a C step"))
        step = CStep(; source, header, pkgconfig, include_dirs)
    elseif source !== nothing || header !== nothing
        throw(ArgumentError("give either `step` or `source`/`header`, not both"))
    end
    if step isa CStep
        isempty(pars) && throw(ArgumentError("a C step needs `pars`, the parameter struct's name"))
        _check_c_ident(pars, "parameter struct name")
    elseif !isempty(pars)
        _check_c_ident(pars, "parameter struct name")
    end
    inputs = StepInput[i isa StepInput ? i : StepInput(i...) for i in inputs]
    count(i -> i.role === :audio, inputs) == 1 ||
        throw(ArgumentError("the step function must take exactly one :audio input"))
    params = PluginParam[params...]
    ids = [p.id for p in params]
    allunique(ids) || throw(ArgumentError("parameter ids must be unique, got $ids"))
    consts = Pair{String, Any}[String(k) => v for (k, v) in constants]
    for (k, v) in consts
        _check_c_ident(k, "constant field")
        v isa Union{Bool, Integer, AbstractFloat} ||
            throw(ArgumentError("constant $k must be a Bool, Integer or Float, got $(typeof(v))"))
    end
    return PluginSpec(String(id), String(name), String(vendor), String(version),
                      String(description), String(url), String[features...], Int(channels),
                      String(base), String(pars), inputs, String(output), sub_clock,
                      sample_rate_field === nothing ? nothing : String(sample_rate_field),
                      params, consts, step)
end

"The same spec with the parameter struct named `pars`."
_with_pars(s::PluginSpec, pars::AbstractString) =
    PluginSpec(s.id, s.name, s.vendor, s.version, s.description, s.url, s.features, s.channels,
               s.base, String(pars), s.inputs, s.output, s.sub_clock, s.sample_rate_field,
               s.params, s.constants, s.step)

function _check_c_ident(s, what)
    occursin(r"^[A-Za-z_][A-Za-z0-9_]*$", s) ||
        throw(ArgumentError("$what must be a C identifier, got $(repr(s))"))
    return nothing
end

"""
    read_plugin_spec(path) -> PluginSpec

Read a descriptor from a TOML file. Relative paths in `[build]` resolve
against the file's directory. The layout, with every optional key shown:

```toml
schema = 1

[plugin]
id = "org.example.gain"
name = "Example Gain"
vendor = "Example"            # optional, as are version, description, url
version = "0.1.0"
features = ["audio-effect"]   # optional
channels = 2                  # optional

[abi]
base = "ex_gain"              # ex_gain_step, ex_gain_reset, ex_gain_mem, ex_gain_out
pars = "ExGainPars"           # the parameter struct declared in the C header (C step only)
inputs = [{ name = "u", role = "audio" }, { name = "clock", role = "clock" }]
output = "y"                  # field of ex_gain_out; optional, default "y"
sub_clock = false             # optional: true if ex_gain_out also carries `bool has_y`
sample_rate_field = "fs"      # optional

[build]                       # a C step ...
source = "ex_gain.c"
header = "ex_gain.h"
pkgconfig = "ex_gain.pc"      # optional
include_dirs = []             # optional

[build]                       # ... or a Julia step (one or the other)
julia = "ex_gain.jl"
project = "."                 # optional: environment to compile in
trim = "safe"                 # optional: juliac trim mode
bundle = false                # optional: copy the Julia runtime next to the plugin

[[param]]
id = 0
name = "Gain"
field = "gain"
min = 0.0
max = 4.0
default = 1.0
automatable = true            # optional
stepped = false               # optional
ctype = "double"              # optional: double, bool or int64_t

[constants]                   # optional: struct fields fixed at instantiation
enabled = true
```

The descriptor format is versioned by `schema`; only `1` exists.
"""
function read_plugin_spec(path::AbstractString)
    d = TOML.parsefile(path)
    dir = dirname(abspath(path))
    get(d, "schema", 1) == 1 ||
        throw(ArgumentError("$path: unsupported descriptor schema $(d["schema"])"))
    plugin = _section(d, "plugin", path)
    abi = _section(d, "abi", path)
    build = _section(d, "build", path)
    resolve(p) = p === nothing ? nothing : normpath(joinpath(dir, p))
    inputs = [StepInput(i["name"], Symbol(i["role"])) for i in get(abi, "inputs", Any[])]
    params = [PluginParam(; id = p["id"], name = p["name"], field = p["field"],
                          min = p["min"], max = p["max"], default = p["default"],
                          automatable = get(p, "automatable", true),
                          stepped = get(p, "stepped", false),
                          ctype = get(p, "ctype", "double"))
              for p in get(d, "param", Any[])]
    step = if haskey(build, "julia")
        haskey(build, "source") &&
            throw(ArgumentError("$path: [build] names both `julia` and `source`"))
        JuliaStep(; file = resolve(build["julia"]),
                  project = something(resolve(get(build, "project", nothing)), ""),
                  trim = get(build, "trim", "safe"), bundle = get(build, "bundle", false))
    else
        haskey(build, "source") && haskey(build, "header") ||
            throw(ArgumentError("$path: [build] needs `source` and `header`, or `julia`"))
        CStep(; source = resolve(build["source"]), header = resolve(build["header"]),
              pkgconfig = resolve(get(build, "pkgconfig", nothing)),
              include_dirs = [resolve(i) for i in get(build, "include_dirs", String[])])
    end
    return PluginSpec(;
        id = plugin["id"], name = plugin["name"],
        vendor = get(plugin, "vendor", ""), version = get(plugin, "version", "0.0.0"),
        description = get(plugin, "description", ""), url = get(plugin, "url", ""),
        features = get(plugin, "features", ["audio-effect"]),
        channels = get(plugin, "channels", 2),
        base = abi["base"], pars = get(abi, "pars", ""), step,
        inputs = isempty(inputs) ? [StepInput("u", :audio)] : inputs,
        output = get(abi, "output", "y"), sub_clock = get(abi, "sub_clock", false),
        sample_rate_field = get(abi, "sample_rate_field", nothing),
        params, constants = collect(get(d, "constants", Dict{String, Any}())))
end

function _section(d, key, path)
    haskey(d, key) || throw(ArgumentError("$path: missing [$key] table"))
    return d[key]
end

# ---------------------------------------------------------------------------
# pkg-config files, read without pkg-config
# ---------------------------------------------------------------------------

"""
    pkgconfig_flags(path) -> (; cflags::Vector{String}, libs::Vector{String})

The `Cflags` and `Libs` (plus `Libs.private`, since the object is linked
directly) of a `.pc` file, with `\${var}` references expanded from the
file's own definitions. Reading the file directly avoids requiring the
`pkg-config` binary, which a machine with a C compiler need not have.
"""
function pkgconfig_flags(path::AbstractString)
    vars = Dict{String, String}()
    fields = Dict{String, String}()
    for raw in eachline(path)
        line = strip(first(split(raw, '#'; limit = 2)))
        isempty(line) && continue
        m = match(r"^([A-Za-z0-9_.]+)\s*(=|:)\s*(.*)$", line)
        m === nothing && continue
        name, sep, value = m.captures
        if sep == "="
            vars[name] = _expand_pc(value, vars)
        else
            fields[name] = _expand_pc(value, vars)
        end
    end
    libs = vcat(Base.shell_split(get(fields, "Libs", "")),
                Base.shell_split(get(fields, "Libs.private", "")))
    return (; cflags = Base.shell_split(get(fields, "Cflags", "")), libs)
end

_expand_pc(s, vars) = replace(s, r"\$\{([A-Za-z0-9_.]+)\}" => m -> get(vars, m[3:(end - 1)], ""))

# ---------------------------------------------------------------------------
# A Julia step's C view: the header juliac does not write
# ---------------------------------------------------------------------------

const C_SCALARS = Dict{DataType, String}(
    Float64 => "double", Float32 => "float", Bool => "bool",
    Int8 => "int8_t", Int16 => "int16_t", Int32 => "int32_t", Int64 => "int64_t",
    UInt8 => "uint8_t", UInt16 => "uint16_t", UInt32 => "uint32_t", UInt64 => "uint64_t")

"""
    julia_step_header(spec::PluginSpec) -> (; pars::String, header::String)

Load `spec.step.file` into a private module (once per file), read the parameter, state
and output struct types off the `@ccallable` signatures of `<base>_step`
and `<base>_reset`, check them against the descriptor, and write the C
header declaring the same ABI. Julia lays out isbits structs the way C
does, and the header carries `_Static_assert`s of every size and field
offset so that a mismatch is a compile error rather than a wrong plugin.

Supported field types: the C scalars (`Float64`, `Float32`, `Bool`, the
sized integers), `Ptr`, `NTuple{N, T}` of those, and nested non-parametric
isbits structs.
"""
function julia_step_header(spec::PluginSpec)
    step = spec.step
    step isa JuliaStep || throw(ArgumentError("julia_step_header: the step is not a JuliaStep"))
    mod = _step_module(step.file)
    stepname, resetname = spec.base * "_step", spec.base * "_reset"
    rt, params = _ccallable_signature(mod, stepname, step.file)
    n = length(spec.inputs)
    length(params) == n + 2 ||
        throw(ArgumentError("$stepname must take the $n descriptor input(s), a Ptr to the parameter " *
                            "struct and a Ptr to the state struct; its @ccallable signature has " *
                            "$(length(params)) arguments"))
    for (inp, T) in zip(spec.inputs, params)
        want = inp.role === :audio ? Float64 : Bool
        T === want || throw(ArgumentError("$stepname: input $(inp.name) must be $want, got $T"))
    end
    P = _pointee(params[n + 1], "$stepname: the parameter argument")
    M = _pointee(params[n + 2], "$stepname: the state argument")
    O = rt
    for (T, what) in ((P, "parameter"), (M, "state"), (O, "output"))
        _plain_struct(T) ||
            throw(ArgumentError("$stepname: the $what struct must be a non-parametric isbits struct, got $T"))
    end
    hasfield(O, Symbol(spec.output)) ||
        throw(ArgumentError("$stepname: output struct $O has no field $(spec.output)"))
    if spec.sub_clock
        flag = Symbol("has_", spec.output)
        hasfield(O, flag) && fieldtype(O, flag) === Bool ||
            throw(ArgumentError("$stepname: a sub-clock output needs a Bool field $flag in $O"))
    end
    rrt, rparams = _ccallable_signature(mod, resetname, step.file)
    (rrt === Nothing || rrt === Cvoid) && rparams == [Ptr{M}] ||
        throw(ArgumentError("$resetname must be declared as ($resetname(self::Ptr{$M})::Cvoid), " *
                            "got return $rrt over $(Tuple(rparams))"))
    isempty(spec.pars) || spec.pars == String(nameof(P)) ||
        throw(ArgumentError("the descriptor names the parameter struct $(spec.pars) but $stepname takes Ptr{$P}"))
    for p in spec.params
        _check_struct_field(P, p.field, p.ctype, "parameter $(repr(p.name))")
    end
    for (k, _) in spec.constants
        hasfield(P, Symbol(k)) || throw(ArgumentError("constant $k: $P has no field $k"))
    end
    spec.sample_rate_field === nothing ||
        _check_struct_field(P, spec.sample_rate_field, "double", "sample_rate_field")
    return (; pars = String(nameof(P)), header = _c_header(spec, step.file, P, M, O))
end

# One module per (file, mtime): before Julia 1.12 a @ccallable name can be
# defined only once per session, so a file must not be included twice.
const STEP_MODULES = Dict{Tuple{String, Float64}, Module}()

function _step_module(file::AbstractString)
    return get!(STEP_MODULES, (file, mtime(file))) do
        mod = Module(:AudioPluginsReflect)
        try
            Base.include(mod, file)
        catch e
            if e isa LoadError && occursin("@ccallable was already defined", sprint(showerror, e.error))
                throw(ArgumentError("$file changed since it was last loaded, and on Julia $VERSION " *
                                    "a @ccallable name cannot be redefined: restart Julia to export it again"))
            end
            rethrow()
        end
        mod
    end
end

function _ccallable_signature(mod::Module, name::AbstractString, file::AbstractString)
    sym = Symbol(name)
    isdefined(mod, sym) || throw(ArgumentError("$file does not define $name"))
    f = getfield(mod, sym)
    ms = methods(f)
    length(ms) == 1 || throw(ArgumentError("$name must have exactly one method, found $(length(ms))"))
    m = only(ms)
    isdefined(m, :ccallable) || throw(ArgumentError("$name is not declared Base.@ccallable"))
    rt, sig = m.ccallable[1], m.ccallable[2]
    return rt, collect(sig.parameters[2:end])
end

function _pointee(T, what)
    T isa DataType && T <: Ptr && T !== Ptr ||
        throw(ArgumentError("$what must be a Ptr to a struct, got $T"))
    return T.parameters[1]
end

_plain_struct(T) = T isa DataType && isstructtype(T) && isbitstype(T) && isempty(T.parameters) &&
                   !(T <: Tuple)

function _check_struct_field(P, field, ctype, what)
    hasfield(P, Symbol(field)) || throw(ArgumentError("$what: $P has no field $field"))
    got = get(C_SCALARS, fieldtype(P, Symbol(field)), nothing)
    got == ctype ||
        throw(ArgumentError("$what: field $field of $P is $(fieldtype(P, Symbol(field))), " *
                            "which is not the descriptor's $ctype"))
    return nothing
end

function _c_header(spec::PluginSpec, file, P, M, O)
    guard = uppercase(spec.base) * "_H"
    io = IOBuffer()
    println(io, "/* $(spec.base).h -- generated by AudioPlugins from $(basename(file)):")
    println(io, " * the C declaration of its @ccallable step. Do not edit. */")
    println(io, "#ifndef $guard\n#define $guard\n")
    println(io, "#include <stdbool.h>\n#include <stddef.h>\n#include <stdint.h>\n")
    done = Set{DataType}()
    for T in (P, M, O)
        _emit_c_struct!(io, T, done)
    end
    M === O || println(io, "typedef $(nameof(M)) $(spec.base)_mem;")
    println(io, "typedef $(nameof(O)) $(spec.base)_out;")
    M === O && println(io, "typedef $(nameof(M)) $(spec.base)_mem;")
    args = join((i.role === :audio ? "double $(i.name)" : "bool $(i.name)" for i in spec.inputs), ", ")
    println(io, "\n$(nameof(O)) $(spec.base)_step($args, $(nameof(P)) *pars, $(nameof(M)) *self);")
    println(io, "void $(spec.base)_reset($(nameof(M)) *self);\n")
    println(io, "#endif")
    return String(take!(io))
end

function _emit_c_struct!(io, T::DataType, done::Set{DataType})
    T in done && return
    # Nested structs first, so every field type is declared before use.
    for i in 1:fieldcount(T)
        F = fieldtype(T, i)
        F <: Tuple && (F = _tuple_eltype(F, T, i))
        _plain_struct(F) && _emit_c_struct!(io, F, done)
    end
    name = String(nameof(T))
    println(io, "typedef struct $name $name;")
    println(io, "struct $name {")
    for i in 1:fieldcount(T)
        println(io, "    ", _c_field(fieldtype(T, i), fieldname(T, i), T, i), ";")
    end
    println(io, "};")
    println(io, "_Static_assert(sizeof($name) == $(sizeof(T)), \"$name: size differs from Julia\");")
    for i in 1:fieldcount(T)
        println(io, "_Static_assert(offsetof($name, $(fieldname(T, i))) == $(fieldoffset(T, i)), ",
                "\"$name.$(fieldname(T, i)): offset differs from Julia\");")
    end
    println(io)
    push!(done, T)
    return
end

function _tuple_eltype(F, T, i)
    fts = fieldtypes(F)
    (!isempty(fts) && all(===(fts[1]), fts)) ||
        throw(ArgumentError("field $(fieldname(T, i)) of $T: only homogeneous NTuple fields map to C arrays, got $F"))
    return fts[1]
end

function _c_field(F, name, T, i)
    if F <: Tuple
        E = _tuple_eltype(F, T, i)
        return "$(_c_type(E, T, i)) $name[$(fieldcount(F))]"
    end
    return "$(_c_type(F, T, i)) $name"
end

function _c_type(F, T, i)
    haskey(C_SCALARS, F) && return C_SCALARS[F]
    if F isa DataType && F <: Ptr
        F === Ptr && throw(ArgumentError("field $(fieldname(T, i)) of $T: an untyped Ptr cannot be declared to C"))
        E = F.parameters[1]
        return E === Cvoid ? "void *" : _c_type(E, T, i) * " *"
    end
    _plain_struct(F) && return String(nameof(F))
    throw(ArgumentError("field $(fieldname(T, i)) of $T has type $F, which has no C declaration " *
                        "(use Float64, Float32, Bool, sized integers, Ptr, NTuple or nested isbits structs)"))
end

# ---------------------------------------------------------------------------
# Rendering the wrapper
# ---------------------------------------------------------------------------

const CLAP_TEMPLATE = normpath(joinpath(@__DIR__, "..", "csrc", "clap_plugin_template.c"))
const VENDOR_DIR = normpath(joinpath(@__DIR__, "..", "csrc", "vendor"))

function _c_string(s::AbstractString)
    io = IOBuffer()
    print(io, '"')
    for c in s
        if c == '"' || c == '\\'
            print(io, '\\', c)
        elseif c == '\n'
            print(io, "\\n")
        elseif isascii(c) && !isprint(c)
            throw(ArgumentError("control character in string $(repr(s))"))
        else
            print(io, c)
        end
    end
    print(io, '"')
    return String(take!(io))
end

function _c_double(x::Real)
    isfinite(x) || throw(ArgumentError("cannot emit non-finite $x as a C double"))
    return repr(Float64(x))
end

_c_literal(v::Bool) = v ? "true" : "false"
_c_literal(v::Integer) = string(v)
_c_literal(v::AbstractFloat) = _c_double(v)

function _param_cast(p::PluginParam)
    p.ctype == "double" && return "v"
    p.ctype == "bool" && return "(v != 0.0)"
    return "(int64_t)v"
end

function _param_flags(p::PluginParam)
    flags = String[]
    p.automatable && push!(flags, "CLAP_PARAM_IS_AUTOMATABLE")
    p.stepped && push!(flags, "CLAP_PARAM_IS_STEPPED")
    return isempty(flags) ? "0u" : join(flags, " | ")
end

function _render_template(template::AbstractString, subs::Dict{String, String})
    out = template
    for (k, v) in subs
        out = replace(out, "@$k@" => v)
    end
    left = unique(eachmatch(r"@[A-Z_]+@", out))
    isempty(left) || error("unrendered tokens in template: $(join((m.match for m in left), ", "))")
    return out
end

function _clap_substitutions(spec::PluginSpec)
    isempty(spec.pars) && throw(ArgumentError("the parameter struct name is not known yet"))
    step_args = join(((i.role === :audio ? "x" : "true") for i in spec.inputs), ", ")
    port_type = spec.channels == 1 ? "CLAP_PORT_MONO" :
                spec.channels == 2 ? "CLAP_PORT_STEREO" : "NULL"
    param_info = join(("{ $(p.id)u, $(_c_string(p.name)), $(_c_double(p.min)), " *
                       "$(_c_double(p.max)), $(_c_double(p.default)), $(_param_flags(p)) },"
                       for p in spec.params), "\n    ")
    param_apply = join(("case $(i - 1): s->pars.$(p.field) = $(_param_cast(p)); break;"
                        for (i, p) in enumerate(spec.params)), "\n    ")
    constants = join(("s->pars.$k = $(_c_literal(v));" for (k, v) in spec.constants), "\n    ")
    on_activate = spec.sample_rate_field === nothing ? "(void)sr;" :
                  "s->pars.$(spec.sample_rate_field) = sr;"
    output_read = spec.sub_clock ?
                  "if (o.has_$(spec.output)) s->held[c] = o.$(spec.output);\n" *
                  "            double y = s->held[c];" :
                  "double y = o.$(spec.output);"
    return Dict(
        "HEADER" => spec.base * ".h",
        "BASE" => spec.base,
        "PARS" => spec.pars,
        "CHANNELS" => string(spec.channels),
        "N_PARAMS" => string(length(spec.params)),
        "FEATURES" => join((_c_string(f) * "," for f in spec.features), " "),
        "ID" => _c_string(spec.id),
        "NAME" => _c_string(spec.name),
        "VENDOR" => _c_string(spec.vendor),
        "URL" => _c_string(spec.url),
        "VERSION" => _c_string(spec.version),
        "DESCRIPTION" => _c_string(spec.description),
        "PARAM_INFO" => param_info,
        "PARAM_APPLY" => param_apply,
        "PORT_TYPE" => port_type,
        "ON_ACTIVATE" => on_activate,
        "STEP_ARGS" => step_args * ",",
        "OUTPUT" => spec.output,
        "OUTPUT_READ" => output_read,
        "CONSTANTS" => constants,
    )
end

"""
    emit_wrapper(::CLAP, spec, dir) -> (; sources, include_dirs)

Render `csrc/clap_plugin_template.c` for `spec` into `dir/clap_plugin.c`.
The wrapper includes `<base>.h`, which for a C step is the descriptor's
header and for a Julia step the generated one.
"""
function emit_wrapper(::CLAP, spec::PluginSpec, dir::AbstractString)
    subs = _clap_substitutions(spec)
    spec.step isa CStep && (subs["HEADER"] = basename(spec.step.header))
    src = _render_template(read(CLAP_TEMPLATE, String), subs)
    path = joinpath(dir, "clap_plugin.c")
    write(path, src)
    return (; sources = [path], include_dirs = [VENDOR_DIR])
end

# ---------------------------------------------------------------------------
# Building
# ---------------------------------------------------------------------------

function _resolve_compiler(compiler)
    compiler === nothing || return String(compiler)
    cc = _c_compiler()
    cc === nothing && error("export_plugin needs a C compiler on PATH (tried cc, gcc, clang), " *
                            "or one passed as `compiler`. Hosting plugins does not.")
    return cc
end

function _run(cmd::Cmd, verbose::Bool)
    verbose && println(stderr, "[export_plugin] ", cmd)
    run(cmd)
    return nothing
end

"""
    place_library(::CLAP, spec, library, out)

Move the linked shared library at `library` into the CLAP bundle at
`out`: renamed in place on Linux and Windows, inside a `Contents/MacOS`
directory bundle with an `Info.plist` on macOS.
"""
function place_library(::CLAP, spec::PluginSpec, library::AbstractString, out::AbstractString)
    if Sys.isapple()
        stem = first(splitext(basename(out)))
        macos = joinpath(out, "Contents", "MacOS")
        mkpath(macos)
        write(joinpath(out, "Contents", "Info.plist"), _info_plist(spec, stem))
        write(joinpath(out, "Contents", "PkgInfo"), "BNDL????")
        mv(library, joinpath(macos, stem); force = true)
    else
        mkpath(dirname(out))
        mv(library, out; force = true)
    end
    return out
end

"""
    runtime_layout(::CLAP, out) -> (; dir, rpath)

Where a bundled Julia runtime goes for the plugin at `out`, and the
rpath, relative to the plugin binary, that finds it: `Name.clap.runtime/`
beside a Linux or Windows `.clap`, `Contents/Resources/julia/` inside a
macOS bundle. The runtime's libraries land under `<dir>/lib`.
"""
function runtime_layout(::CLAP, out::AbstractString)
    Sys.isapple() && return (; dir = joinpath(out, "Contents", "Resources", "julia"),
                             rpath = joinpath("..", "Resources", "julia", "lib"))
    return (; dir = out * ".runtime", rpath = joinpath(basename(out) * ".runtime", "lib"))
end

function _info_plist(spec::PluginSpec, stem::AbstractString)
    esc(s) = replace(s, "&" => "&amp;", "<" => "&lt;", ">" => "&gt;")
    return """
    <?xml version="1.0" encoding="UTF-8"?>
    <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
    <plist version="1.0">
    <dict>
        <key>CFBundleDevelopmentRegion</key><string>English</string>
        <key>CFBundleExecutable</key><string>$(esc(stem))</string>
        <key>CFBundleIdentifier</key><string>$(esc(spec.id))</string>
        <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
        <key>CFBundleName</key><string>$(esc(spec.name))</string>
        <key>CFBundlePackageType</key><string>BNDL</string>
        <key>CFBundleShortVersionString</key><string>$(esc(spec.version))</string>
        <key>CFBundleVersion</key><string>$(esc(spec.version))</string>
    </dict>
    </plist>
    """
end

"""
    export_plugin(spec::PluginSpec, out; format = CLAP(), compiler = nothing,
                  verbose = false) -> out

Build the plugin described by `spec` into the bundle at `out`, whose
extension must be the format's (`.clap`).

For a [`CStep`](@ref): the wrapper is rendered and compiled under
`-Wall -Wextra -Werror`; the step's C is compiled without those, since
generated C tends to carry harmless unused temporaries; both are built
with hidden visibility so that two plugins sharing an ABI base name can
coexist in one host process; link flags come from the descriptor's `.pc`
file, and an undefined symbol fails the link rather than the first
`dlopen`.

For a [`JuliaStep`](@ref): the step's structs are read off its
`@ccallable` signature and declared by a generated header, and juliac
compiles the Julia file and the wrapper into one trimmed shared library.
This needs `using JuliaC` and Julia ≥ 1.12.

Both need a C compiler: `cc`, `gcc` or `clang` on `PATH`, or
`compiler = "/path/to/cc"`. Hosting plugins does not.
"""
function export_plugin(spec::PluginSpec, out::AbstractString; format::PluginFormat = CLAP(),
                       compiler = nothing, verbose::Bool = false)
    ext = bundle_extension(format)
    endswith(out, ext) ||
        throw(ArgumentError("output $(repr(out)) must end in $ext for format $(format_name(format))"))
    out = abspath(out)
    spec.step isa JuliaStep && return _export_julia_step(format, spec, out; compiler, verbose)
    step = spec.step::CStep
    cc = _resolve_compiler(compiler)
    pc = step.pkgconfig === nothing ? (; cflags = String[], libs = String[]) :
         pkgconfig_flags(step.pkgconfig)
    mktempdir() do dir
        wrapper = emit_wrapper(format, spec, dir)
        # -isystem rather than -I for the wrapper: the ABI header is someone
        # else's code and must not fail our -Werror build.
        incs = String[]
        for d in [wrapper.include_dirs; dirname(step.header); dirname(step.source); step.include_dirs]
            push!(incs, "-isystem", d)
        end
        objects = String[]
        strict = ["-std=gnu99", "-Wall", "-Wextra", "-Werror"]
        # Windows code is position independent already, and MinGW warns about -fPIC.
        pic = Sys.iswindows() ? String[] : ["-fPIC"]
        common = ["-O2", pic..., "-fvisibility=hidden", incs..., pc.cflags...]
        for (i, src) in enumerate(wrapper.sources)
            obj = joinpath(dir, "wrapper_$i.o")
            _run(`$cc $strict $common -c $src -o $obj`, verbose)
            push!(objects, obj)
        end
        model = joinpath(dir, "model.o")
        _run(`$cc $common -c $(step.source) -o $model`, verbose)
        push!(objects, model)
        library = joinpath(dir, "plugin." * Base.BinaryPlatforms.platform_dlext())
        undefined = Sys.isapple() ? String[] : ["-Wl,--no-undefined"]
        _run(`$cc -shared $pic -o $library $objects $(pc.libs) $undefined`, verbose)
        place_library(format, spec, library, out)
    end
    return out
end

"""
    _export_julia_step(format, spec, out; compiler, verbose)

Implemented by the `AudioPluginsJuliaCExt` extension, which loads with
`using JuliaC`.
"""
function _export_julia_step(format, spec, out; compiler, verbose)
    error("export_plugin: building a plugin from a JuliaStep needs JuliaC loaded " *
          "(`using JuliaC`) and Julia ≥ 1.12; this is Julia $VERSION")
end

register_plugin_format!(CLAP())

@static if VERSION >= v"1.11"
    eval(Meta.parse("public format_name, bundle_extension, emit_wrapper, place_library, " *
                    "runtime_layout, pkgconfig_flags, julia_step_header"))
end
