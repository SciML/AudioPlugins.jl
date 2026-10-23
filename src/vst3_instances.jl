# Independent VST3 instances, with the same scalar-double node ABI.
export Vst3Instance, vst3_copy!, vst3_plugin_index, vst3_n_audio_in, vst3_n_audio_out

@static if isdefined(Base, :ispublic)
    eval(Expr(:public, :vst3_copy, :vst3_set, :vst3_expect))
end

"""
    Vst3Instance(f::Function, path; kwargs...)
    Vst3Instance(path; class_id = "", sample_rate = 48000, block_size = 512, channels = 1)

Open an independent VST3 effect without replacing the default plugin or any
other instance. Pass the same bundle path and class id as to [`vst3_open!`](@ref).
Pass the instance as the first argument to `vst3_fill!`, `vst3_out`, the
configuration/parameter readers, and `AudioPlugins.vst3_*` operators. Generated
nodes can pass its numeric `handle` instead to the `vst3_*` operators.

Use the do-block form to close automatically, including on exceptions, or
close explicitly with `vst3_close!(instance)` in a `finally` block. Closing
is idempotent; a closed handle never names a later instance. Calls into the
host must be serialized: instances may coexist, but parallel calls from
multiple threads are not supported. Latency is reported, not compensated.

Requires `VST3Host_jll` 1.4 or later; see [`vst3_lib_path`](@ref) for the development override.
"""
struct Vst3Instance
    handle::Float64
end

Base.cconvert(::Type{Cdouble}, instance::Vst3Instance) = instance.handle

function Vst3Instance(
        path::AbstractString; class_id::AbstractString = "",
        sample_rate::Real = 48000, block_size::Integer = 512, channels::Integer = 1
    )
    bundle = path
    id = class_id
    handle = ccall(
        (:vst3_host_open_instance, VST3_LIB), Cdouble,
        (Cstring, Cstring, Cdouble, Cdouble, Cdouble),
        bundle, id, sample_rate, block_size, channels
    )
    isnan(handle) && error("Vst3Instance($(repr(path))) failed: $(vst3_last_error())")
    return Vst3Instance(handle)
end

function Vst3Instance(f::Function, path::AbstractString; kwargs...)
    instance = Vst3Instance(path; kwargs...)
    try
        return f(instance)
    finally
        vst3_close!(instance)
    end
end

function vst3_close!(instance::Vst3Instance)
    ccall((:vst3_host_close_instance, VST3_LIB), Cvoid, (Cdouble,), instance)
    return nothing
end

function vst3_fill!(instance::Vst3Instance, samples::AbstractVector{<:Real}; channels::Integer = 1)
    channels > 0 || throw(ArgumentError("channels must be positive"))
    length(samples) % channels == 0 || throw(ArgumentError("samples must contain complete frames"))
    v = Vector{Cdouble}(samples)
    return ccall(
        (:vst3_in_fill_for, VST3_LIB), Cdouble, (Cdouble, Ptr{Cdouble}, Clong, Clong),
        instance, v, length(v) ÷ channels, channels
    )
end

"""
    vst3_copy!(destination::Vst3Instance, source::Vst3Instance, token) -> token

Copy the current output block of `source` into `destination` and return its
input token. Both instances must have the same block size, sample rate and
host channel count. Invalid handles, stale output and mismatches return `NaN`.
The C copy allocates nothing and discards any pending destination parameter
chain. Use `AudioPlugins.vst3_copy(destination.handle, source.handle, token)`
from a generated node. Latency is not compensated.
"""
vst3_copy!(destination::Vst3Instance, source::Vst3Instance, token::Real) =
    vst3_copy(destination, source, token)

"""
    AudioPlugins.vst3_copy(destination, source, token) -> token

Scalar-double node operator for [`vst3_copy!`](@ref). Pass instances or their
numeric handles; the destination receives the source's current output block.
"""
vst3_copy(destination, source, token) = ccall(
    (:vst3_in_copy_for, VST3_LIB), Cdouble, (Cdouble, Cdouble, Cdouble),
    destination, source, token
)

function vst3_out(instance::Vst3Instance, token::Real; channel::Integer = 0)
    n = ccall((:vst3_out_count_for, VST3_LIB), Cdouble, (Cdouble, Cdouble), instance, token)
    isnan(n) && return Float64[]
    return [vst3_out_sample(instance, token, i, channel) for i in 0:(Int(n) - 1)]
end

vst3_plugin_name(instance::Vst3Instance) =
    unsafe_string(ccall((:vst3_host_plugin_name_for, VST3_LIB), Cstring, (Cdouble,), instance))

vst3_is_open(instance::Vst3Instance) =
    ccall((:vst3_host_is_open_for, VST3_LIB), Cdouble, (Cdouble,), instance) > 0.5

vst3_block_size(instance::Vst3Instance) =
    Int(ccall((:vst3_host_block_size_for, VST3_LIB), Cdouble, (Cdouble,), instance))

vst3_sample_rate(instance::Vst3Instance) =
    ccall((:vst3_host_sample_rate_for, VST3_LIB), Cdouble, (Cdouble,), instance)

vst3_n_process(instance::Vst3Instance) =
    ccall((:vst3_host_n_process_for, VST3_LIB), Clong, (Cdouble,), instance)

vst3_param_count(instance::Vst3Instance) =
    ccall((:vst3_host_n_params_for, VST3_LIB), Clong, (Cdouble,), instance)

"Number of audio input channels declared by the instance."
vst3_n_audio_in(instance::Vst3Instance) =
    Int(ccall((:vst3_host_n_audio_in_for, VST3_LIB), Cdouble, (Cdouble,), instance))

"Number of audio output channels declared by the instance."
vst3_n_audio_out(instance::Vst3Instance) =
    Int(ccall((:vst3_host_n_audio_out_for, VST3_LIB), Cdouble, (Cdouble,), instance))

vst3_latency(instance::Vst3Instance) =
    ccall((:vst3_host_latency_for, VST3_LIB), Cdouble, (Cdouble,), instance)

vst3_reset_counters!(instance::Vst3Instance) =
    ccall((:vst3_host_reset_counters_for, VST3_LIB), Cvoid, (Cdouble,), instance)

vst3_param_value(instance::Vst3Instance, id::Real) = ccall(
    (:vst3_host_param_value_for, VST3_LIB), Cdouble, (Cdouble, Cdouble), instance, id
)

function vst3_params(instance::Vst3Instance)
    n = vst3_param_count(instance)
    return [
        (
            id = ccall((:vst3_host_param_id_for, VST3_LIB), Cdouble, (Cdouble, Clong), instance, i),
            name = unsafe_string(ccall((:vst3_host_param_name_for, VST3_LIB), Cstring, (Cdouble, Clong), instance, i)),
            units = unsafe_string(ccall((:vst3_host_param_units_for, VST3_LIB), Cstring, (Cdouble, Clong), instance, i)),
            min = ccall((:vst3_host_param_min_for, VST3_LIB), Cdouble, (Cdouble, Clong), instance, i),
            max = ccall((:vst3_host_param_max_for, VST3_LIB), Cdouble, (Cdouble, Clong), instance, i),
            default = ccall((:vst3_host_param_default_for, VST3_LIB), Cdouble, (Cdouble, Clong), instance, i),
            default_normalized = ccall((:vst3_host_param_default_normalized_for, VST3_LIB), Cdouble, (Cdouble, Clong), instance, i),
            steps = ccall((:vst3_host_param_steps_for, VST3_LIB), Cdouble, (Cdouble, Clong), instance, i),
            readonly = ccall((:vst3_host_param_is_readonly_for, VST3_LIB), Cdouble, (Cdouble, Clong), instance, i) > 0.5,
            bypass = ccall((:vst3_host_param_is_bypass_for, VST3_LIB), Cdouble, (Cdouble, Clong), instance, i) > 0.5,
        )
            for i in 0:(n - 1)
    ]
end

# Each overload remains a named ccall, so generated C can use the same ABI.
vst3_in_tone(instance, t, waveform, freq, amp) = ccall(
    (:vst3_in_tone_for, VST3_LIB), Cdouble, (Cdouble, Cdouble, Cdouble, Cdouble, Cdouble),
    instance, t, waveform, freq, amp
)

vst3_process(instance, dep, id0, v0, id1, v1, id2, v2, id3, v3) = ccall(
    (:vst3_process_for, VST3_LIB), Cdouble, (Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble),
    instance, dep, id0, v0, id1, v1, id2, v2, id3, v3
)

vst3_set(instance, dep, id, value) = ccall(
    (:vst3_set_param_for, VST3_LIB), Cdouble, (Cdouble, Cdouble, Cdouble, Cdouble),
    instance, dep, id, value
)

vst3_expect(instance, dep, index) = ccall(
    (:vst3_expect_for, VST3_LIB), Cdouble, (Cdouble, Cdouble, Cdouble),
    instance, dep, index
)

vst3_out_rms(instance, dep) = ccall(
    (:vst3_out_rms_for, VST3_LIB), Cdouble, (Cdouble, Cdouble),
    instance, dep
)

vst3_out_peak(instance, dep) = ccall(
    (:vst3_out_peak_for, VST3_LIB), Cdouble, (Cdouble, Cdouble),
    instance, dep
)

vst3_out_valid(instance, dep) = ccall(
    (:vst3_out_valid_for, VST3_LIB), Cdouble, (Cdouble, Cdouble),
    instance, dep
)

vst3_in_sample(instance, dep, i, ch) = ccall(
    (:vst3_in_sample_for, VST3_LIB), Cdouble, (Cdouble, Cdouble, Cdouble, Cdouble),
    instance, dep, i, ch
)

vst3_out_sample(instance, dep, i, ch) = ccall(
    (:vst3_out_sample_for, VST3_LIB), Cdouble, (Cdouble, Cdouble, Cdouble, Cdouble),
    instance, dep, i, ch
)

"Zero-based index of the instance's plugin in its discovery list; `-1` when closed."
vst3_plugin_index(instance::Vst3Instance) = ccall((:vst3_host_open_index_for, VST3_LIB), Clong, (Cdouble,), instance)

vst3_channels(instance::Vst3Instance) = Int(ccall((:vst3_host_channels_for, VST3_LIB), Cdouble, (Cdouble,), instance))

vst3_plugin_id(instance::Vst3Instance) = unsafe_string(ccall((:vst3_host_plugin_id_for, VST3_LIB), Cstring, (Cdouble,), instance))

vst3_param_plain(instance::Vst3Instance, param_id::Real, normalized::Real) = ccall((:vst3_host_param_plain_for, VST3_LIB), Cdouble, (Cdouble, Cdouble, Cdouble), instance, param_id, normalized)

vst3_param_normalized(instance::Vst3Instance, param_id::Real, plain::Real) = ccall((:vst3_host_param_normalized_for, VST3_LIB), Cdouble, (Cdouble, Cdouble, Cdouble), instance, param_id, plain)

"""
    AudioPlugins.vst3_set(dep, id, value) -> token
    AudioPlugins.vst3_set(instance, dep, id, value) -> token

Queue a normalized (0–1) parameter value for the armed block. Calls may
chain any number of declared writable parameters; repeated ids keep the last
value. Explicit process slots take precedence. A fresh input or block copy
discards the queue. Values are clamped to [0, 1] during processing.
Invalid handles, stale inputs and invalid parameters return `NaN`.
"""
vst3_set(dep, id, value) = ccall((:vst3_set_param, VST3_LIB), Cdouble, (Cdouble, Cdouble, Cdouble), dep, id, value)

"""
    AudioPlugins.vst3_expect(dep, index) -> token
    AudioPlugins.vst3_expect(instance, dep, index) -> token

Check the zero-based discovery index of the open plugin and return `dep`.
A mismatched identity or an invalid dependency returns `NaN`.
"""
vst3_expect(dep, index) = ccall((:vst3_expect, VST3_LIB), Cdouble, (Cdouble, Cdouble), dep, index)

vst3_n_restart_requests(instance::Vst3Instance) = ccall(
    (:vst3_host_n_restart_requests_for, VST3_LIB), Clong, (Cdouble,), instance
)

vst3_n_plugin_edits(instance::Vst3Instance) = ccall(
    (:vst3_host_n_plugin_edits_for, VST3_LIB), Clong, (Cdouble,), instance
)
