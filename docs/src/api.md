# API

```@meta
CurrentModule = AudioPlugins
```

Every exported name is listed here. Names shown as `AudioPlugins.something` are not
exported: they are the format-extension interface and the node-side operators, documented
because writing a new format or driving the processing path needs them.

## The module

```@docs
AudioPlugins
```

## Hosting: the library

```@docs
clap_host_available
clap_lib_path
clap_src_path
build_clap_host!
clap_test_bundle
```

## Hosting: lifecycle and discovery

```@docs
clap_scan
clap_open!
clap_close!
clap_is_open
clap_plugin_name
clap_last_error
```

## Hosting: the bundle registry

Which `.clap` modules are available, and what is in them. Nothing is registered until
something registers it — see [Plugin collections: the bundle registry](@ref).

```@docs
register_bundle!
unregister_bundle!
bundles
plugins
AudioPlugins.find_plugin
AudioPlugins.RegisteredBundle
```

## Hosting: configuration in force

```@docs
clap_block_size
clap_sample_rate
clap_latency
clap_compensating
clap_flush!
clap_n_process
clap_reset_counters!
```

## Hosting: parameters

```@docs
clap_params
clap_param_count
AudioPlugins.clap_param_value
```

## Hosting: audio in and out

```@docs
clap_fill!
clap_out
```

## Node-side operators

One equation, one call. Each takes the token of the block it depends on, so nothing can be
scheduled before the block it reads. Not exported: a Julia caller reaches them as
`AudioPlugins.clp_process` and so on.

```@docs
AudioPlugins.clp_in_tone
AudioPlugins.clp_process
AudioPlugins.clp_in_sample
AudioPlugins.clp_out_sample
AudioPlugins.clp_out_rms
AudioPlugins.clp_out_peak
AudioPlugins.clp_out_valid
```

### Waveform codes

```@docs
CLAP_WAVE_SILENCE
CLAP_WAVE_SINE
CLAP_WAVE_SQUARE
CLAP_WAVE_RAMP
CLAP_WAVE_IMPULSE
```

## LV2 hosting: the library

```@docs
lv2_lib_path
lv2_src_path
lv2_default_path
lv2_test_bundle
```

## LV2 hosting: lifecycle and discovery

```@docs
lv2_scan
lv2_open!
lv2_close!
lv2_is_open
lv2_plugin_name
lv2_plugin_uri
lv2_last_error
```

## LV2 hosting: configuration in force

```@docs
lv2_block_size
lv2_sample_rate
lv2_channels
lv2_latency
lv2_n_process
lv2_reset_counters!
```

## LV2 hosting: parameters

An LV2 parameter is a control input port, so its `id` is a port index rather
than a format-assigned id, and its value is in the port's own plain units.

```@docs
lv2_params
lv2_param_count
lv2_param_value
```

## LV2 hosting: audio in and out

```@docs
lv2_fill!
lv2_out
```

## LV2 node-side operators

The LV2 half of the node-side surface, shaped exactly like the CLAP one above.

```@docs
AudioPlugins.lv2_in_tone
AudioPlugins.lv2_process
AudioPlugins.lv2_in_sample
AudioPlugins.lv2_out_sample
AudioPlugins.lv2_out_rms
AudioPlugins.lv2_out_peak
AudioPlugins.lv2_out_valid
```

### LV2 waveform codes

```@docs
LV2_WAVE_SILENCE
LV2_WAVE_SINE
LV2_WAVE_SQUARE
LV2_WAVE_RAMP
LV2_WAVE_IMPULSE
```

## Authoring: the descriptor

```@docs
PluginSpec
read_plugin_spec
PluginParam
StepInput
```

## Authoring: step sources

```@docs
StepSource
CStep
JuliaStep
```

## Authoring: building

```@docs
export_plugin
```

## Authoring: formats

```@docs
PluginFormat
CLAP
register_plugin_format!
plugin_format
```

The methods a [`PluginFormat`](@ref) implements, with CLAP's as the worked example:

```@docs
AudioPlugins.format_name
AudioPlugins.bundle_extension
AudioPlugins.emit_wrapper
AudioPlugins.place_library
AudioPlugins.runtime_layout
```

## Authoring: helpers

```@docs
AudioPlugins.pkgconfig_flags
AudioPlugins.julia_step_header
```
