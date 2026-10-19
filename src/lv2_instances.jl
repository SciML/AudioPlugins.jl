# Independent LV2 instances, with the same scalar-double node ABI.
export Lv2Instance, lv2_copy!, lv2_plugin_index, lv2_n_audio_in, lv2_n_audio_out

@static if isdefined(Base, :ispublic)
    eval(Expr(:public, :lv2_copy, :lv2_set, :lv2_expect))
end

"""
    Lv2Instance(f::Function, path; kwargs...)
    Lv2Instance(path; uri = "", sample_rate = 48000, block_size = 512, channels = 1)

Open an independent LV2 effect without replacing the default plugin or any
other instance. Pass the same search path and URI as to [`lv2_open!`](@ref).
Pass the instance as the first argument to `lv2_fill!`, `lv2_out`, the
configuration/parameter readers, and `AudioPlugins.lv2_*` operators. Generated
nodes can pass its numeric `handle` instead to the `lv2_*` operators.

Use the do-block form to close automatically, including on exceptions, or
close explicitly with `lv2_close!(instance)` in a `finally` block. Closing
is idempotent; a closed handle never names a later instance. Calls into the
host must be serialized: instances may coexist, but parallel calls from
multiple threads are not supported. Latency is reported, not compensated.

Requires `LV2Host_jll` 1.4 or later; see [`lv2_lib_path`](@ref) for the development override.
"""
struct Lv2Instance
    handle::Float64
end

Base.cconvert(::Type{Cdouble}, instance::Lv2Instance) = instance.handle

function Lv2Instance(
        path::AbstractString; uri::AbstractString = "",
        sample_rate::Real = 48000, block_size::Integer = 512, channels::Integer = 1
    )
    bundle = path
    id = uri
    handle = ccall(
        (:lv2_host_open_instance, LV2_LIB), Cdouble,
        (Cstring, Cstring, Cdouble, Cdouble, Cdouble),
        bundle, id, sample_rate, block_size, channels
    )
    isnan(handle) && error("Lv2Instance($(repr(path))) failed: $(lv2_last_error())")
    return Lv2Instance(handle)
end

function Lv2Instance(f::Function, path::AbstractString; kwargs...)
    instance = Lv2Instance(path; kwargs...)
    try
        return f(instance)
    finally
        lv2_close!(instance)
    end
end

function lv2_close!(instance::Lv2Instance)
    ccall((:lv2_host_close_instance, LV2_LIB), Cvoid, (Cdouble,), instance)
    return nothing
end

function lv2_fill!(instance::Lv2Instance, samples::AbstractVector{<:Real}; channels::Integer = 1)
    channels > 0 || throw(ArgumentError("channels must be positive"))
    length(samples) % channels == 0 || throw(ArgumentError("samples must contain complete frames"))
    v = Vector{Cdouble}(samples)
    return ccall(
        (:lv2_in_fill_for, LV2_LIB), Cdouble, (Cdouble, Ptr{Cdouble}, Clong, Clong),
        instance, v, length(v) ÷ channels, channels
    )
end

"""
    lv2_copy!(destination::Lv2Instance, source::Lv2Instance, token) -> token

Copy the current output block of `source` into `destination` and return its
input token. Both instances must have the same block size, sample rate and
host channel count. Invalid handles, stale output and mismatches return `NaN`.
The C copy allocates nothing and discards any pending destination parameter
chain. Use `AudioPlugins.lv2_copy(destination.handle, source.handle, token)`
from a generated node. Latency is not compensated.
"""
lv2_copy!(destination::Lv2Instance, source::Lv2Instance, token::Real) =
    lv2_copy(destination, source, token)

"""
    AudioPlugins.lv2_copy(destination, source, token) -> token

Scalar-double node operator for [`lv2_copy!`](@ref). Pass instances or their
numeric handles; the destination receives the source's current output block.
"""
lv2_copy(destination, source, token) = ccall(
    (:lv2_in_copy_for, LV2_LIB), Cdouble, (Cdouble, Cdouble, Cdouble),
    destination, source, token
)

function lv2_out(instance::Lv2Instance, token::Real; channel::Integer = 0)
    n = ccall((:lv2_out_count_for, LV2_LIB), Cdouble, (Cdouble, Cdouble), instance, token)
    isnan(n) && return Float64[]
    return [lv2_out_sample(instance, token, i, channel) for i in 0:(Int(n) - 1)]
end

lv2_plugin_name(instance::Lv2Instance) =
    unsafe_string(ccall((:lv2_host_plugin_name_for, LV2_LIB), Cstring, (Cdouble,), instance))

lv2_is_open(instance::Lv2Instance) =
    ccall((:lv2_host_is_open_for, LV2_LIB), Cdouble, (Cdouble,), instance) > 0.5

lv2_block_size(instance::Lv2Instance) =
    Int(ccall((:lv2_host_block_size_for, LV2_LIB), Cdouble, (Cdouble,), instance))

lv2_sample_rate(instance::Lv2Instance) =
    ccall((:lv2_host_sample_rate_for, LV2_LIB), Cdouble, (Cdouble,), instance)

lv2_n_process(instance::Lv2Instance) =
    ccall((:lv2_host_n_process_for, LV2_LIB), Clong, (Cdouble,), instance)

lv2_param_count(instance::Lv2Instance) =
    ccall((:lv2_host_n_params_for, LV2_LIB), Clong, (Cdouble,), instance)

"Number of audio input channels declared by the instance."
lv2_n_audio_in(instance::Lv2Instance) =
    Int(ccall((:lv2_host_n_audio_in_for, LV2_LIB), Cdouble, (Cdouble,), instance))

"Number of audio output channels declared by the instance."
lv2_n_audio_out(instance::Lv2Instance) =
    Int(ccall((:lv2_host_n_audio_out_for, LV2_LIB), Cdouble, (Cdouble,), instance))

lv2_latency(instance::Lv2Instance) =
    ccall((:lv2_host_latency_for, LV2_LIB), Cdouble, (Cdouble,), instance)

lv2_reset_counters!(instance::Lv2Instance) =
    ccall((:lv2_host_reset_counters_for, LV2_LIB), Cvoid, (Cdouble,), instance)

lv2_param_value(instance::Lv2Instance, id::Real) = ccall(
    (:lv2_host_param_value_for, LV2_LIB), Cdouble, (Cdouble, Cdouble), instance, id
)

function lv2_params(instance::Lv2Instance)
    n = lv2_param_count(instance)
    return [
        (
            id = ccall((:lv2_host_param_id_for, LV2_LIB), Cdouble, (Cdouble, Clong), instance, i),
            name = unsafe_string(ccall((:lv2_host_param_name_for, LV2_LIB), Cstring, (Cdouble, Clong), instance, i)),
            symbol = unsafe_string(ccall((:lv2_host_param_symbol_for, LV2_LIB), Cstring, (Cdouble, Clong), instance, i)),
            min = ccall((:lv2_host_param_min_for, LV2_LIB), Cdouble, (Cdouble, Clong), instance, i),
            max = ccall((:lv2_host_param_max_for, LV2_LIB), Cdouble, (Cdouble, Clong), instance, i),
            default = ccall((:lv2_host_param_default_for, LV2_LIB), Cdouble, (Cdouble, Clong), instance, i),
        )
            for i in 0:(n - 1)
    ]
end

# Each overload remains a named ccall, so generated C can use the same ABI.
lv2_in_tone(instance, t, waveform, freq, amp) = ccall(
    (:lv2_in_tone_for, LV2_LIB), Cdouble, (Cdouble, Cdouble, Cdouble, Cdouble, Cdouble),
    instance, t, waveform, freq, amp
)

lv2_process(instance, dep, id0, v0, id1, v1, id2, v2, id3, v3) = ccall(
    (:lv2_process_for, LV2_LIB), Cdouble, (Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble),
    instance, dep, id0, v0, id1, v1, id2, v2, id3, v3
)

lv2_set(instance, dep, id, value) = ccall(
    (:lv2_set_param_for, LV2_LIB), Cdouble, (Cdouble, Cdouble, Cdouble, Cdouble),
    instance, dep, id, value
)

lv2_expect(instance, dep, index) = ccall(
    (:lv2_expect_for, LV2_LIB), Cdouble, (Cdouble, Cdouble, Cdouble),
    instance, dep, index
)

lv2_out_rms(instance, dep) = ccall(
    (:lv2_out_rms_for, LV2_LIB), Cdouble, (Cdouble, Cdouble),
    instance, dep
)

lv2_out_peak(instance, dep) = ccall(
    (:lv2_out_peak_for, LV2_LIB), Cdouble, (Cdouble, Cdouble),
    instance, dep
)

lv2_out_valid(instance, dep) = ccall(
    (:lv2_out_valid_for, LV2_LIB), Cdouble, (Cdouble, Cdouble),
    instance, dep
)

lv2_in_sample(instance, dep, i, ch) = ccall(
    (:lv2_in_sample_for, LV2_LIB), Cdouble, (Cdouble, Cdouble, Cdouble, Cdouble),
    instance, dep, i, ch
)

lv2_out_sample(instance, dep, i, ch) = ccall(
    (:lv2_out_sample_for, LV2_LIB), Cdouble, (Cdouble, Cdouble, Cdouble, Cdouble),
    instance, dep, i, ch
)

"Zero-based index of the instance's plugin in its discovery list; `-1` when closed."
lv2_plugin_index(instance::Lv2Instance) = ccall((:lv2_host_open_index_for, LV2_LIB), Clong, (Cdouble,), instance)

lv2_channels(instance::Lv2Instance) = Int(ccall((:lv2_host_channels_for, LV2_LIB), Cdouble, (Cdouble,), instance))

lv2_plugin_uri(instance::Lv2Instance) = unsafe_string(ccall((:lv2_host_plugin_uri_for, LV2_LIB), Cstring, (Cdouble,), instance))

lv2_port_value(instance::Lv2Instance, port_index::Real) = ccall((:lv2_host_port_value_for, LV2_LIB), Cdouble, (Cdouble, Cdouble), instance, port_index)

"""
    AudioPlugins.lv2_set(dep, id, value) -> token
    AudioPlugins.lv2_set(instance, dep, id, value) -> token

Queue a control input value for the armed block. Calls may chain any number
of declared parameters; a repeated id keeps the last value. Explicit process
slots take precedence. A fresh input or block copy discards the queue.
Values use LV2 plain units.
Invalid handles, stale inputs and invalid parameters return `NaN`.
"""
lv2_set(dep, id, value) = ccall((:lv2_set_param, LV2_LIB), Cdouble, (Cdouble, Cdouble, Cdouble), dep, id, value)

"""
    AudioPlugins.lv2_expect(dep, index) -> token
    AudioPlugins.lv2_expect(instance, dep, index) -> token

Check the zero-based discovery index of the open plugin and return `dep`.
A mismatched identity or an invalid dependency returns `NaN`.
"""
lv2_expect(dep, index) = ccall((:lv2_expect, LV2_LIB), Cdouble, (Cdouble, Cdouble), dep, index)

function lv2_atom_ports(instance::Lv2Instance)
    n = Int(ccall((:lv2_host_n_atom_ports_for, LV2_LIB), Cdouble, (Cdouble,), instance))
    return [
        (
            index = Int(ccall((:lv2_host_atom_port_index_for, LV2_LIB), Cdouble, (Cdouble, Cdouble), instance, i)),
            input = ccall((:lv2_host_atom_port_is_input_for, LV2_LIB), Cdouble, (Cdouble, Cdouble), instance, i) > 0.5,
            midi = ccall((:lv2_host_atom_port_midi_for, LV2_LIB), Cdouble, (Cdouble, Cdouble), instance, i) > 0.5,
            size = Int(ccall((:lv2_host_atom_port_size_for, LV2_LIB), Cdouble, (Cdouble, Cdouble), instance, i)),
        ) for i in 0:(n - 1)
    ]
end

lv2_in_midi(instance, dep, port, frame, b0, b1, b2) = ccall(
    (:lv2_in_midi_for, LV2_LIB), Cdouble,
    (Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble),
    instance, dep, port, frame, b0, b1, b2
)

function lv2_midi!(
        instance::Lv2Instance, token::Real, port::Integer, frame::Integer, b0::Integer,
        b1::Integer = -1, b2::Integer = -1
    )
    result = lv2_in_midi(instance, token, port, frame, b0, b1, b2)
    isnan(result) && error("lv2_midi!: $(lv2_last_error())")
    return result
end
