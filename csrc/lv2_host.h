/* lv2_host.h -- hosting an LV2 audio plugin from a synchronous node.
 *
 * The LV2 counterpart of clap_host.h, with the same shape for the same
 * reasons: every entry point is a named symbol in a shared library, every
 * value crossing the node-side boundary is a scalar double, a block is
 * named by a token threaded through `dep`, and every failure is loud.
 *
 * Discovery is lilv's. An LV2 plugin's port layout -- which index is audio
 * in, which is audio out, which is a control and what its range is --
 * lives in Turtle manifests next to the binary, not in the binary, and
 * lilv (with serd, sord, sratom and the LV2 specification bundles) is the
 * reference reader of those manifests. This host asks lilv for the port
 * map and connects every port itself, so a caller names a plugin by URI
 * and never sees a port index unless it wants one.
 *
 * Parameters are the plugin's control INPUT ports, identified by port
 * index (a double, exactly like a CLAP id is a uint32). Latency is the
 * plugin's designated lv2:latency control output port, surfaced and not
 * compensated, exactly like CLAP's.
 *
 * Which plugins this host can open: those whose required features are a
 * subset of what it provides (urid:map, urid:unmap, bufsz:fixedBlockLength,
 * bufsz:boundedBlockLength) and whose ports are audio, control, or
 * connection-optional. A plugin that requires an atom, CV or event port,
 * or a host feature this host does not offer, is refused at open with a
 * message that says which. Refusing is the honest answer: an unconnected
 * required port is undefined behaviour in the LV2 spec.
 */
#ifndef LV2_HOST_H
#define LV2_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

#define LV2_HOST_MAX_BLOCK   8192
#define LV2_HOST_MAX_CHAN    2
#define LV2_HOST_MAX_PORTS   256
#define LV2_HOST_MAX_PARAMS  64
#define LV2_HOST_PARAM_SLOTS 4

/* ------------------------------------------------------------------ *
 * Driver-side lifecycle and discovery. Strings live here.
 * ------------------------------------------------------------------ */

/* Load every bundle found under `lv2_path` (a list of directories joined
 * by the platform's path separator -- ':' on POSIX, ';' on Windows) and
 * enumerate the plugins. Pass "" or NULL for lilv's default search path
 * (the LV2_PATH environment variable, or the platform's standard
 * directories). Closes any open plugin. Returns the plugin count, or -1
 * on failure (see lv2_host_last_error). */
long lv2_host_scan(const char *lv2_path);

/* Fields of the i-th plugin from the last successful scan, or "" when i
 * is out of range. */
const char *lv2_host_scan_uri(long i);
const char *lv2_host_scan_name(long i);

/* Instantiate and activate the plugin `uri` found under `lv2_path` (same
 * meaning as in lv2_host_scan; pass "" for the first plugin found).
 * `block_size` is the exact number of frames every process() call will
 * carry. `channels` is the number of host audio channels: the k-th audio
 * input port receives host channel min(k, channels-1) (so a mono host
 * feeds every input of a stereo plugin), and the k-th audio output port
 * writes host channel k, or is discarded when k >= channels. The plugin
 * must have at least `channels` audio output ports. Returns 0 on success,
 * non-zero on failure (see lv2_host_last_error). */
int lv2_host_open(const char *lv2_path, const char *uri,
                  double sample_rate, double block_size, double channels);

/* Deactivate, free the instance, free the world. Safe when nothing is open. */
void lv2_host_close(void);

const char *lv2_host_last_error(void);
const char *lv2_host_plugin_name(void);   /* "" when nothing is open */
const char *lv2_host_plugin_uri(void);

/* ------------------------------------------------------------------ *
 * Parameters: the control input ports. `id` is the port index.
 * ------------------------------------------------------------------ */

long   lv2_host_n_params(void);
double lv2_host_param_id(long i);        /* port index; -1 when i is out of range */
double lv2_host_param_min(long i);       /* NaN when the manifest gives none */
double lv2_host_param_max(long i);
double lv2_host_param_default(long i);
const char *lv2_host_param_name(long i);
const char *lv2_host_param_symbol(long i);

/* The value currently connected to a control input port, by port index.
 * NaN for an index that is not a control input. */
double lv2_host_param_value(double port_index);

/* Latency the plugin reports on its lv2:latency port, in samples; 0 when
 * it has no such port. A plugin writes this port from run(), so the value
 * is authoritative after the first block (the test plugins also set it in
 * activate). NOT COMPENSATED -- see clap_host.h. */
double lv2_host_latency(void);

/* Port census, for a driver that wants to know what it opened. */
double lv2_host_n_audio_in(void);
double lv2_host_n_audio_out(void);

double lv2_host_sample_rate(void);
double lv2_host_block_size(void);
double lv2_host_channels(void);
double lv2_host_is_open(void);
long   lv2_host_n_process(void);
void   lv2_host_reset_counters(void);

/* ------------------------------------------------------------------ *
 * The input block. Same contract as clap_in_*.
 * ------------------------------------------------------------------ */

double lv2_in_fill(const double *samples, long n, long channels);
double lv2_in_tone(double t, double waveform, double freq, double amp);
double lv2_in_sample(double dep, double i, double ch);

#define LV2_WAVE_SILENCE 0
#define LV2_WAVE_SINE    1
#define LV2_WAVE_SQUARE  2
#define LV2_WAVE_RAMP    3
#define LV2_WAVE_IMPULSE 4

/* ------------------------------------------------------------------ *
 * The node-side operator. One equation, one run().
 * ------------------------------------------------------------------ */

/* Run the plugin over the block named by `dep` and return the output
 * block's token. Up to LV2_HOST_PARAM_SLOTS (port index, value) pairs set
 * control input ports for this block; a negative index means the slot is
 * unused, a NaN value leaves the port as it was. A control port is a
 * single float the plugin reads at run(), so a change lands on exactly
 * the block it is passed with. Returns NaN when nothing is open or when
 * `dep` does not name the current input block. */
double lv2_process(double dep,
                   double id0, double v0, double id1, double v1,
                   double id2, double v2, double id3, double v3);

double lv2_out_sample(double dep, double i, double ch);
double lv2_out_rms(double dep);
double lv2_out_peak(double dep);
double lv2_out_count(double dep);
double lv2_out_valid(double dep);

#ifdef __cplusplus
}
#endif
#endif /* LV2_HOST_H */
