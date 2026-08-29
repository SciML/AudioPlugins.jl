/* vst3_host.h -- hosting a VST3 audio plugin from a synchronous node.
 *
 * The VST3 counterpart of clap_host.h and lv2_host.h, with the same shape
 * for the same reasons: every entry point is a named symbol in a shared
 * library, every value crossing the node-side boundary is a scalar double,
 * a block is named by a token threaded through `dep`, and every failure is
 * loud. The implementation (vst3_host.cpp) is C++ because the VST3 SDK is,
 * but nothing C++ crosses this surface: no classes, no exceptions, no
 * function pointers.
 *
 * Built against the Steinberg VST3 SDK, version 3.8 or newer (MIT since
 * 3.8; https://github.com/steinbergmedia/vst3sdk). The SDK is not
 * vendored: the shim is compiled inside Yggdrasil against vst3sdk_jll and
 * shipped as VST3Host_jll, so a Julia user never needs a C++ toolchain.
 *
 * Headless only: no plugin editor is ever created. Parameter changes go
 * through ProcessData::inputParameterChanges (IParameterChanges /
 * IParamValueQueue, at sample offset 0), never by poking the edit
 * controller. Latency is IAudioProcessor::getLatencySamples(), surfaced
 * and not compensated. Parameter ids are VST3 ParamIDs (uint32), exactly
 * representable as doubles; VALUES ARE NORMALISED (0..1) as VST3 defines
 * them, which is what the plugin's processor consumes -- the controller's
 * plain-value mapping is reported (min, max, default in plain units) for
 * information, and `vst3_host_param_plain`/`_normalized` convert.
 */
#ifndef VST3_HOST_H
#define VST3_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

/* The shim is built with hidden visibility so that only this surface is
 * exported from the shared library: nothing of the SDK leaks out. */
#if defined(_WIN32) && defined(VST3_HOST_BUILDING)
#  define VST3_HOST_API __declspec(dllexport)
#elif defined(__GNUC__) && defined(VST3_HOST_BUILDING)
#  define VST3_HOST_API __attribute__((visibility("default")))
#else
#  define VST3_HOST_API
#endif

#define VST3_HOST_MAX_BLOCK   8192
#define VST3_HOST_MAX_CHAN    2
#define VST3_HOST_MAX_PARAMS  256
#define VST3_HOST_PARAM_SLOTS 4

/* ------------------------------------------------------------------ *
 * Driver-side lifecycle and discovery. Strings live here.
 * ------------------------------------------------------------------ */

/* Load a .vst3 bundle (a directory: Contents/<arch>-linux/<name>.so on
 * Linux, Contents/MacOS/<name> on macOS, Contents/<arch>-win/<name>.vst3
 * on Windows; a bare shared object is accepted too), resolve its factory,
 * and cache the audio-effect classes. Closes any open plugin. Returns the
 * class count, or -1 on failure (see vst3_host_last_error). */
VST3_HOST_API long vst3_host_scan(const char *path);

/* Fields of the i-th audio-effect class from the last successful scan,
 * or "" when i is out of range. The id is the class UID as 32 hex
 * characters, which is what vst3_host_open takes. */
VST3_HOST_API const char *vst3_host_scan_id(long i);
VST3_HOST_API const char *vst3_host_scan_name(long i);
VST3_HOST_API const char *vst3_host_scan_category(long i);   /* e.g. "Fx|Dynamics" */

/* Instantiate, initialise and activate the class `class_id` (32 hex chars;
 * "" or NULL for the first audio-effect class) at a FIXED block size:
 * ProcessSetup.maxSamplesPerBlock == block_size and every process() call
 * carries exactly that many frames. `channels` (1 or 2) is asked of the
 * plugin's main input and output buses as mono or stereo
 * (setBusArrangements); a plugin that refuses is refused. Returns 0 on
 * success, non-zero on failure (see vst3_host_last_error). */
VST3_HOST_API int vst3_host_open(const char *path, const char *class_id,
                   double sample_rate, double block_size, double channels);

/* setProcessing(false), setActive(false), terminate, release, unload.
 * Safe when nothing is open. */
VST3_HOST_API void vst3_host_close(void);

VST3_HOST_API const char *vst3_host_last_error(void);
VST3_HOST_API const char *vst3_host_plugin_name(void);       /* "" when nothing is open */
VST3_HOST_API const char *vst3_host_plugin_id(void);

/* ------------------------------------------------------------------ *
 * Parameters, from the edit controller, read once at open.
 * ------------------------------------------------------------------ */

VST3_HOST_API long   vst3_host_n_params(void);
VST3_HOST_API double vst3_host_param_id(long i);          /* ParamID; -1 when i is out of range */
VST3_HOST_API double vst3_host_param_min(long i);         /* plain-value range, per the controller */
VST3_HOST_API double vst3_host_param_max(long i);
VST3_HOST_API double vst3_host_param_default(long i);     /* plain */
VST3_HOST_API double vst3_host_param_default_normalized(long i);
VST3_HOST_API double vst3_host_param_steps(long i);       /* 0 = continuous, 1 = toggle, n = discrete */
VST3_HOST_API double vst3_host_param_is_readonly(long i); /* 1 for meters and other outputs */
VST3_HOST_API double vst3_host_param_is_bypass(long i);
VST3_HOST_API const char *vst3_host_param_name(long i);
VST3_HOST_API const char *vst3_host_param_units(long i);

/* Conversions through the controller, by ParamID. NaN for an unknown id. */
VST3_HOST_API double vst3_host_param_plain(double param_id, double normalized);
VST3_HOST_API double vst3_host_param_normalized(double param_id, double plain);

/* The controller's current normalised value for `param_id`, i.e. what
 * this host last sent (the host mirrors every change it sends into the
 * controller so the two views agree). NaN for an unknown id. */
VST3_HOST_API double vst3_host_param_value(double param_id);

/* IAudioProcessor::getLatencySamples(), read at open and after every
 * restartComponent(kLatencyChanged) the plugin requests. NOT COMPENSATED
 * -- see clap_host.h. */
VST3_HOST_API double vst3_host_latency(void);

VST3_HOST_API double vst3_host_sample_rate(void);
VST3_HOST_API double vst3_host_block_size(void);
VST3_HOST_API double vst3_host_channels(void);
VST3_HOST_API double vst3_host_is_open(void);
VST3_HOST_API long   vst3_host_n_process(void);
VST3_HOST_API void   vst3_host_reset_counters(void);

/* Counters of what the plugin asked of the host, for tests: restart
 * requests (IComponentHandler::restartComponent) and parameter edits it
 * initiated (beginEdit/performEdit/endEdit). */
VST3_HOST_API long vst3_host_n_restart_requests(void);
VST3_HOST_API long vst3_host_n_plugin_edits(void);

/* ------------------------------------------------------------------ *
 * The input block. Same contract as clap_in_*.
 * ------------------------------------------------------------------ */

VST3_HOST_API double vst3_in_fill(const double *samples, long n, long channels);
VST3_HOST_API double vst3_in_tone(double t, double waveform, double freq, double amp);
VST3_HOST_API double vst3_in_sample(double dep, double i, double ch);

#define VST3_WAVE_SILENCE 0
#define VST3_WAVE_SINE    1
#define VST3_WAVE_SQUARE  2
#define VST3_WAVE_RAMP    3
#define VST3_WAVE_IMPULSE 4

/* ------------------------------------------------------------------ *
 * The node-side operator. One equation, one process().
 * ------------------------------------------------------------------ */

/* Run the plugin over the block named by `dep` and return the output
 * block's token. Up to VST3_HOST_PARAM_SLOTS (ParamID, normalised value)
 * pairs are queued into ProcessData::inputParameterChanges at sample
 * offset 0 -- the correct mechanism, rather than the edit controller. A
 * negative id means the slot is unused; a value is only queued when it
 * differs from what was last sent for that id, so a held parameter costs
 * one change on the first block and none afterwards. Returns NaN when
 * nothing is open or when `dep` does not name the current input block. */
VST3_HOST_API double vst3_process(double dep,
                    double id0, double v0, double id1, double v1,
                    double id2, double v2, double id3, double v3);

VST3_HOST_API double vst3_out_sample(double dep, double i, double ch);
VST3_HOST_API double vst3_out_rms(double dep);
VST3_HOST_API double vst3_out_peak(double dep);
VST3_HOST_API double vst3_out_count(double dep);
VST3_HOST_API double vst3_out_valid(double dep);

#ifdef __cplusplus
}
#endif
#endif /* VST3_HOST_H */
