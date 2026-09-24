# Independent CLAP instances, with the same scalar-double node ABI.
export ClapInstance, clap_copy!

"""
    ClapInstance(f::Function, path; kwargs...)
    ClapInstance(path; plugin_id = "", sample_rate = 48000, block_size = 512, channels = 1)

Open an independent CLAP effect without replacing the default plugin or any
other instance. `path` may also be a registered plugin id, as in [`clap_open!`](@ref).
Pass the instance as the first argument to `clap_fill!`, `clap_out`, the
configuration/parameter readers, and `AudioPlugins.clp_*` operators. Generated
nodes can pass its numeric `handle` instead to the `clp_*` operators.

Use the do-block form to close automatically, including on exceptions, or
close explicitly with `clap_close!(instance)` in a `finally` block. Closing
is idempotent; a closed handle never names a later instance. Calls into the
host must be serialized: instances may coexist, but parallel calls from
multiple threads are not supported. Latency is reported, not compensated.

Requires a host library built from the accompanying C sources with the
instance API; see [`clap_lib_path`](@ref) for the development override.
"""
struct ClapInstance
    handle::Float64
end

Base.cconvert(::Type{Cdouble}, instance::ClapInstance) = instance.handle

function ClapInstance(
        path::AbstractString; plugin_id::AbstractString = "",
        sample_rate::Real = 48000, block_size::Integer = 512, channels::Integer = 1
    )
    bundle, id = _resolve_plugin(path, plugin_id)
    handle = ccall(
        (:clap_host_open_instance, CLAP_LIB), Cdouble,
        (Cstring, Cstring, Cdouble, Cdouble, Cdouble),
        bundle, id, sample_rate, block_size, channels
    )
    isnan(handle) && error("ClapInstance($(repr(path))) failed: $(clap_last_error())")
    return ClapInstance(handle)
end

function ClapInstance(f::Function, path::AbstractString; kwargs...)
    instance = ClapInstance(path; kwargs...)
    try
        return f(instance)
    finally
        clap_close!(instance)
    end
end

function clap_close!(instance::ClapInstance)
    ccall((:clap_host_close_instance, CLAP_LIB), Cvoid, (Cdouble,), instance)
    return nothing
end

function clap_fill!(instance::ClapInstance, samples::AbstractVector{<:Real}; channels::Integer = 1)
    channels > 0 || throw(ArgumentError("channels must be positive"))
    length(samples) % channels == 0 || throw(ArgumentError("samples must contain complete frames"))
    v = Vector{Cdouble}(samples)
    return ccall(
        (:clap_in_fill_for, CLAP_LIB), Cdouble, (Cdouble, Ptr{Cdouble}, Clong, Clong),
        instance, v, length(v) ÷ channels, channels
    )
end

"""
    clap_copy!(destination::ClapInstance, source::ClapInstance, token) -> token

Copy the current output block of `source` into `destination` and return its
input token. Both instances must have the same block size, sample rate and
host channel count. Invalid handles, stale output and mismatches return `NaN`.
The C copy allocates nothing and discards any pending destination parameter
chain. Use `AudioPlugins.clp_copy(destination.handle, source.handle, token)`
from a generated node. Latency is not compensated.
"""
clap_copy!(destination::ClapInstance, source::ClapInstance, token::Real) =
    clp_copy(destination, source, token)

"Scalar-double node operator for [`clap_copy!`](@ref), also accepting instances."
clp_copy(destination, source, token) = ccall(
    (:clap_in_copy_for, CLAP_LIB), Cdouble, (Cdouble, Cdouble, Cdouble),
    destination, source, token
)

function clap_out(instance::ClapInstance, token::Real; channel::Integer = 0)
    n = ccall((:clap_out_count_for, CLAP_LIB), Cdouble, (Cdouble, Cdouble), instance, token)
    isnan(n) && return Float64[]
    return [clp_out_sample(instance, token, i, channel) for i in 0:(Int(n) - 1)]
end

clap_plugin_name(instance::ClapInstance) =
    unsafe_string(ccall((:clap_host_plugin_name_for, CLAP_LIB), Cstring, (Cdouble,), instance))

clap_plugin_index(instance::ClapInstance) =
    ccall((:clap_host_open_index_for, CLAP_LIB), Clong, (Cdouble,), instance)

clap_is_open(instance::ClapInstance) =
    ccall((:clap_host_is_open_for, CLAP_LIB), Cdouble, (Cdouble,), instance) > 0.5

clap_block_size(instance::ClapInstance) =
    Int(ccall((:clap_host_block_size_for, CLAP_LIB), Cdouble, (Cdouble,), instance))

clap_sample_rate(instance::ClapInstance) =
    ccall((:clap_host_sample_rate_for, CLAP_LIB), Cdouble, (Cdouble,), instance)

clap_n_process(instance::ClapInstance) =
    ccall((:clap_host_n_process_for, CLAP_LIB), Clong, (Cdouble,), instance)

clap_param_count(instance::ClapInstance) =
    ccall((:clap_host_n_params_for, CLAP_LIB), Clong, (Cdouble,), instance)

clap_n_audio_in(instance::ClapInstance) =
    Int(ccall((:clap_host_n_audio_in_for, CLAP_LIB), Cdouble, (Cdouble,), instance))

clap_n_audio_out(instance::ClapInstance) =
    Int(ccall((:clap_host_n_audio_out_for, CLAP_LIB), Cdouble, (Cdouble,), instance))

clap_latency(instance::ClapInstance) =
    ccall((:clap_host_latency_for, CLAP_LIB), Cdouble, (Cdouble,), instance)

clap_reset_counters!(instance::ClapInstance) =
    ccall((:clap_host_reset_counters_for, CLAP_LIB), Cvoid, (Cdouble,), instance)

clap_param_value(instance::ClapInstance, id::Real) = ccall(
    (:clap_host_param_value_for, CLAP_LIB), Cdouble, (Cdouble, Cdouble), instance, id
)

function clap_params(instance::ClapInstance)
    return [
        (
            id = ccall((:clap_host_param_id_for, CLAP_LIB), Cdouble, (Cdouble, Clong), instance, i),
            name = unsafe_string(ccall((:clap_host_param_name_for, CLAP_LIB), Cstring, (Cdouble, Clong), instance, i)),
            min = ccall((:clap_host_param_min_for, CLAP_LIB), Cdouble, (Cdouble, Clong), instance, i),
            max = ccall((:clap_host_param_max_for, CLAP_LIB), Cdouble, (Cdouble, Clong), instance, i),
            default = ccall((:clap_host_param_default_for, CLAP_LIB), Cdouble, (Cdouble, Clong), instance, i),
        ) for i in 0:(clap_param_count(instance) - 1)
    ]
end

# Each overload remains a named ccall, so generated C can use the same ABI.
clp_in_tone(instance, t, waveform, freq, amp) = ccall(
    (:clap_in_tone_for, CLAP_LIB), Cdouble, (Cdouble, Cdouble, Cdouble, Cdouble, Cdouble),
    instance, t, waveform, freq, amp
)

clp_process(instance, dep, id0, v0, id1, v1, id2, v2, id3, v3) = ccall(
    (:clap_process_for, CLAP_LIB), Cdouble, (Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble),
    instance, dep, id0, v0, id1, v1, id2, v2, id3, v3
)

clp_set(instance, dep, id, value) = ccall(
    (:clap_set_param_for, CLAP_LIB), Cdouble, (Cdouble, Cdouble, Cdouble, Cdouble),
    instance, dep, id, value
)

clp_expect(instance, dep, index) = ccall(
    (:clap_expect_for, CLAP_LIB), Cdouble, (Cdouble, Cdouble, Cdouble),
    instance, dep, index
)

clp_out_rms(instance, dep) = ccall(
    (:clap_out_rms_for, CLAP_LIB), Cdouble, (Cdouble, Cdouble),
    instance, dep
)

clp_out_peak(instance, dep) = ccall(
    (:clap_out_peak_for, CLAP_LIB), Cdouble, (Cdouble, Cdouble),
    instance, dep
)

clp_out_valid(instance, dep) = ccall(
    (:clap_out_valid_for, CLAP_LIB), Cdouble, (Cdouble, Cdouble),
    instance, dep
)

clp_in_sample(instance, dep, i, ch) = ccall(
    (:clap_in_sample_for, CLAP_LIB), Cdouble, (Cdouble, Cdouble, Cdouble, Cdouble),
    instance, dep, i, ch
)

clp_out_sample(instance, dep, i, ch) = ccall(
    (:clap_out_sample_for, CLAP_LIB), Cdouble, (Cdouble, Cdouble, Cdouble, Cdouble),
    instance, dep, i, ch
)
