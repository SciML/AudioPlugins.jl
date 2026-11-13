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
 * bufsz:boundedBlockLength) and whose ports are audio, control,
 * connection-optional, or atom:AtomPort with atom:Sequence buffer type.
 * Atom ports are connected to one sequence buffer each, sized from the
 * port's declared rsz:minimumSize (LV2_HOST_ATOM_BUF when undeclared):
 * MIDI events reach an input sequence through lv2_in_midi, and an output
 * port is re-headed to an atom:Chunk of full capacity before every run().
 * A plugin that requires a CV or event port, an atom port of another buffer type,
 * or a host feature this host does not offer, is refused at open with a
 * message that says which. Refusing is the honest answer: an unconnected
 * required port is undefined behaviour in the LV2 spec.
 */
#ifndef LV2_HOST_H
#define LV2_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

#define LV2_HOST_MAX_BLOCK      8192
#define LV2_HOST_MAX_CHAN       2
#define LV2_HOST_MAX_PORTS      256
#define LV2_HOST_MAX_PARAMS     64
#define LV2_HOST_PARAM_SLOTS    4
#define LV2_HOST_MAX_ATOM_PORTS 32
/* Floor for an atom port's buffer when the manifest declares no
 * rsz:minimumSize: a few hundred small events per block. A declared
 * minimumSize is honoured exactly, whatever it is. */
#define LV2_HOST_ATOM_BUF       8192
/* The atom buffers live in one arena allocated at open; a manifest that
 * asks for more than this in total is refused rather than allocated. */
#define LV2_HOST_ATOM_ARENA_MAX (64u * 1024u * 1024u)

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
 * carry. `channels` is the number of host audio channels. The rule, for
 * inputs and outputs alike, is never to drop a channel. Only MAIN audio
 * ports take part: a port marked lv2:isSideChain, or carrying (or
 * grouped under) pg:sideChainOf, is non-main, as is any port outside a
 * plugin-declared pg:mainInput / pg:mainOutput group; non-main inputs
 * read a zeroed buffer and non-main outputs are discarded. The k-th
 * main audio input receives host channel min(k, channels-1), so a mono
 * host feeds every input of a stereo plugin, but a plugin with fewer
 * main audio inputs than `channels` is refused rather than leave a host
 * channel connected to nothing. The k-th main audio output writes host
 * channel k, or is discarded when k >= channels; when the plugin has
 * fewer main outputs than `channels`, the last one repeats on the
 * remaining channels (a mono plugin is centre-panned); a plugin with no
 * audio ports at all is silent, which is the shape of a MIDI tool.
 * Returns 0 on success, non-zero on failure (see lv2_host_last_error). */
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

/* The value currently connected to a control port, by port index, input
 * or output: how a control OUTPUT port (other than the designated latency
 * port, which lv2_host_latency covers) is read. NaN for an index that is
 * not a control port. */
double lv2_host_port_value(double port_index);

/* Latency the plugin reports on its lv2:latency port, in samples; 0 when
 * it has no such port. A plugin writes this port from run(), so the value
 * is authoritative after the first block (the test plugins also set it in
 * activate). NOT COMPENSATED -- see clap_host.h. */
double lv2_host_latency(void);

/* ------------------------------------------------------------------ *
 * Atom ports. Each atom:AtomPort is connected to one atom:Sequence
 * buffer; the functions below enumerate them in port order so a caller
 * can find which port index carries MIDI.
 * ------------------------------------------------------------------ */

/* Number of atom ports the open plugin has (0 when none or nothing open). */
double lv2_host_n_atom_ports(void);
/* Port index of the i-th atom port; NaN when i is out of range. */
double lv2_host_atom_port_index(double i);
/* 1.0 when the i-th atom port is an input, 0.0 when an output. */
double lv2_host_atom_port_is_input(double i);
/* 1.0 when the i-th atom port declares atom:supports midi:MidiEvent --
 * the only ports lv2_in_midi will write. 0.0 otherwise, including a port
 * that declares no atom:supports at all: undeclared does not mean MIDI. */
double lv2_host_atom_port_midi(double i);
/* Bytes of sequence buffer connected to the i-th atom port: the declared
 * rsz:minimumSize, or LV2_HOST_ATOM_BUF when undeclared. */
double lv2_host_atom_port_size(double i);

/* Main audio port census (sidechain and non-main ports are excluded),
 * for a driver that wants to know what it opened. */
double lv2_host_n_audio_in(void);
double lv2_host_n_audio_out(void);

double lv2_host_sample_rate(void);
double lv2_host_block_size(void);
double lv2_host_channels(void);
double lv2_host_is_open(void);
long   lv2_host_n_process(void);
void   lv2_host_reset_counters(void);

/* ------------------------------------------------------------------ *
 * The input block. The shape is clap_in_*'s, with one difference in the
 * contract: here a fill arms the block for exactly one lv2_process, which
 * consumes the token -- afterwards lv2_in_sample returns NaN and a second
 * lv2_process on the same dep refuses. The CLAP host leaves a consumed
 * block readable and lets it run again; this host does not, so queued
 * events can never replay.
 * ------------------------------------------------------------------ */

/* Fill the input block and return its token. Both also re-head every atom
 * input sequence to empty and discard any MIDI events queued for the
 * previous block: a block's events are queued after the fill that arms it. */
double lv2_in_fill(const double *samples, long n, long channels);
double lv2_in_tone(double t, double waveform, double freq, double amp);
double lv2_in_sample(double dep, double i, double ch);

/* Append one MIDI event to the input atom:Sequence of atom input port
 * `port`, at `frame` samples into the block `dep` names (0-based, so
 * sample-accurate). `b0` is the status byte (0x80..0xFF); `b1` and `b2`
 * are the data bytes that follow (0x00..0x7F), a negative value or NaN
 * marking the message shorter -- a byte after such a marker is an error.
 * The message must be complete for its status: 3 bytes for channel
 * voices except 0xC0-0xDF (2 bytes), the listed system lengths for
 * 0xF0-0xFF -- variable-length sysex is refused. Events must be queued
 * in non-decreasing frame order -- LV2 requires a sorted sequence --
 * between the fill that arms a block and the lv2_process that runs it.
 * Returns `dep` on success, so queued events chain into lv2_process the
 * way clp_set chains into clp_process, and NaN with a message in
 * lv2_host_last_error: no open plugin, `dep` not naming an armed block,
 * `port` not a MIDI-declared atom input, `frame` outside the block or
 * out of order, a malformed or incomplete message, or the sequence
 * buffer full. A refused event also poisons the block: the next
 * lv2_process refuses rather than run without it. */
double lv2_in_midi(double dep, double port, double frame,
                   double b0, double b1, double b2);

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
 * the block it is passed with. The block is consumed: a second call on
 * the same `dep` refuses. Returns NaN when nothing is open, when `dep`
 * does not name the armed input block, or when the block carries a
 * refused MIDI event. */
double lv2_process(double dep,
                   double id0, double v0, double id1, double v1,
                   double id2, double v2, double id3, double v3);

double lv2_out_sample(double dep, double i, double ch);
double lv2_out_rms(double dep);
double lv2_out_peak(double dep);
double lv2_out_count(double dep);
double lv2_out_valid(double dep);

/* Independent instances: serialize host calls. Handles are never reused;
 * source/output tokens belong to one instance and one current block.
 * Copy requires matching rate, block size and host channels. Queued parameters
 * apply at offset zero; the process call's explicit slots take precedence. */
const char *lv2_host_plugin_name_for(double handle);
const char *lv2_host_plugin_uri_for(double handle);
long lv2_host_n_params_for(double handle);
double lv2_host_param_id_for(double handle, long i);
double lv2_host_param_min_for(double handle, long i);
double lv2_host_param_max_for(double handle, long i);
double lv2_host_param_default_for(double handle, long i);
const char *lv2_host_param_name_for(double handle, long i);
const char *lv2_host_param_symbol_for(double handle, long i);
double lv2_host_param_value_for(double handle, double port_index);
double lv2_host_port_value_for(double handle, double port_index);
double lv2_host_n_atom_ports_for(double handle);
double lv2_host_atom_port_index_for(double handle, double i);
double lv2_host_atom_port_is_input_for(double handle, double i);
double lv2_host_atom_port_midi_for(double handle, double i);
double lv2_host_atom_port_size_for(double handle, double i);
double lv2_host_latency_for(double handle);
double lv2_host_n_audio_in_for(double handle);
double lv2_host_n_audio_out_for(double handle);
double lv2_host_sample_rate_for(double handle);
double lv2_host_block_size_for(double handle);
double lv2_host_channels_for(double handle);
double lv2_host_is_open_for(double handle);
long   lv2_host_n_process_for(double handle);
void   lv2_host_reset_counters_for(double handle);
double lv2_in_fill_for(double handle, const double *samples, long n, long channels);
double lv2_in_tone_for(double handle, double t, double waveform, double freq, double amp);
double lv2_in_sample_for(double handle, double dep, double i, double ch);
double lv2_in_midi_for(double handle, double dep, double port, double frame,
                   double b0, double b1, double b2);
double lv2_process_for(double handle, double dep,
                   double id0, double v0, double id1, double v1,
                   double id2, double v2, double id3, double v3);
double lv2_out_sample_for(double handle, double dep, double i, double ch);
double lv2_out_rms_for(double handle, double dep);
double lv2_out_peak_for(double handle, double dep);
double lv2_out_count_for(double handle, double dep);
double lv2_out_valid_for(double handle, double dep);
long lv2_host_open_index_for(double handle);
double lv2_host_open_instance(const char *path, const char *id, double rate, double block, double channels);
void lv2_host_close_instance(double handle);
double lv2_set_param(double dep, double id, double value);
double lv2_set_param_for(double handle, double dep, double id, double value);
double lv2_expect(double dep, double index);
double lv2_expect_for(double handle, double dep, double index);
double lv2_in_copy_for(double destination, double source, double dep);

#ifdef __cplusplus
}
#endif
#endif /* LV2_HOST_H */
