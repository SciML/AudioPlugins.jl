/* clap_host.h -- hosting a CLAP audio plugin from a Dyad synchronous node.
 *
 * The plugin instance of the pattern csrc/audio_hw.c established in
 * AudioComponents: an operator called from a clocked equation must be a
 * *named* ccall into a shared library at a compile-time-constant path, so
 * everything plugin-specific lives here behind a C ABI of scalar doubles,
 * and the same Dyad component compiles for the Julia backend, the
 * in-process C backend and standalone exported C alike.
 *
 * Why this is cheap: a plugin's process() contract *is* the editing-block
 * contract. A contiguous block of samples in, a block out; the block size
 * fixed at setup (activate(min,max) <-> the block_size structural
 * parameter); state persisting across blocks inside the plugin; and scalar
 * parameters that may change per block. Nothing about the frame-token
 * design had to bend to accommodate it.
 *
 * Where it does not line up: a plugin expects to be driven from a realtime
 * audio thread that never blocks and never allocates, and a Dyad solver is
 * neither. See the note on CLAP_HOST_REALTIME below -- the mismatch is
 * benign for offline work and is documented rather than hidden.
 *
 * CLAP is MIT and header-only: there is nothing to link, the host dlopen()s
 * (LoadLibrary()s, on Windows) the plugin bundle and talks to it through
 * the vendored declarations in vendor/clap (1.2.10, see
 * vendor/PROVENANCE.md). That is the whole
 * dependency -- no C++ compiler, no JLL, no system package.
 *
 * Two properties are load-bearing and asserted by the tests, the same two
 * the audio and vision boundaries carry:
 *
 *  1. Exactly one process() call per tick. stkcompile does no CSE and no
 *     DCE, so one equation is one call, and a second call in a tick would
 *     advance the plugin's internal state twice for one block of time.
 *  2. Ordering. clap_process() takes the input block's token as its `dep`,
 *     so it cannot be scheduled before the block it consumes exists, and a
 *     token that does not name the current block is refused with NaN rather
 *     than answered from whatever the buffer happens to hold.
 */
#ifndef CLAP_HOST_H
#define CLAP_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

/* Largest block this host will process, and the widest channel count. Both
 * are compile-time because they size the per-instance buffers -- a plugin host
 * that allocated per block would be violating the one rule every plugin
 * author is asked to keep. */
#define CLAP_HOST_MAX_BLOCK   8192
#define CLAP_HOST_MAX_CHAN    2
#define CLAP_HOST_MAX_PARAMS  64
/* Bounds on the audio-port layout a plugin may declare and still be served:
 * ports per direction, and channels summed across them. A plugin past
 * either is refused at open, so these size static tables rather than gate
 * anything legitimate -- a 7.1.4 port is 12 channels and even that fits. */
#define CLAP_HOST_MAX_PORTS      16
#define CLAP_HOST_MAX_PORT_CHAN  32
/* Parameters clap_process() itself carries, as (id, value) argument pairs.
 * Not the number a block can drive: clap_set_param() chains, and is what a
 * plugin with more parameters than this uses. */
#define CLAP_HOST_PARAM_SLOTS 4

/* Sizes of the per-plugin descriptor cache a scan fills. The number of
 * plugins is not bounded here -- a single CLAP module can hold hundreds,
 * and a cache that stopped at some round number would report a bundle as
 * smaller than it is, which is indistinguishable from a bundle that is. */
#define CLAP_HOST_DESC_STR     256
#define CLAP_HOST_MAX_FEATURES 12
#define CLAP_HOST_FEATURE_STR  48

/* ------------------------------------------------------------------ *
 * Driver-side lifecycle and discovery. Not called from inside the
 * node: these take and return strings, and every value crossing a
 * synchronous program's interface is a number. A plugin path and a
 * plugin id are `structural parameter`s in Dyad -- build-time values
 * that never enter the equations -- and the driver opens the same
 * plugin the model was built against, so the two cannot disagree.
 * ------------------------------------------------------------------ */

/* Enumerate a .clap bundle without instantiating anything: dlopen it,
 * resolve the clap_entry symbol, check ABI compatibility, and cache the
 * factory's descriptors. Returns the plugin count, or -1 on failure (see
 * clap_host_last_error). Safe to call again. */
long clap_host_scan(const char *path);

/* How many plugins the last successful scan found, without rescanning.
 * 0 before the first scan and after one that failed. */
long clap_host_scan_count(void);

/* Descriptor fields of the i-th plugin from the last successful scan, or
 * "" when i is out of range. Driver-side only -- these are strings.
 * `id` and `name` are mandatory in CLAP; the rest are optional and come
 * back "" when the plugin leaves them unset. */
const char *clap_host_scan_id(long i);
const char *clap_host_scan_name(long i);
const char *clap_host_scan_vendor(long i);
const char *clap_host_scan_version(long i);
const char *clap_host_scan_description(long i);

/* The i-th plugin's CLAP feature keywords -- "audio-effect", "reverb",
 * "stereo" and so on, the list a host indexes and classifies by. How many
 * there are, and the k-th of them ("" when either index is out of range).
 * A plugin declaring more than CLAP_HOST_MAX_FEATURES keeps the first
 * CLAP_HOST_MAX_FEATURES: these are classification hints, and no plugin
 * needs a dozen of them to say what it is. */
long        clap_host_scan_n_features(long i);
const char *clap_host_scan_feature(long i, long k);

/* Instantiate and activate. `plugin_id` selects from the bundle; pass ""
 * or NULL for the first plugin. `block_size` is the exact number of frames
 * every process() call will carry -- the plugin is activated with
 * min == max == block_size, so a plugin that cannot work at a fixed block
 * size fails here, loudly, rather than at the first tick.
 *
 * `channels` is the number of host audio channels, which is not the
 * plugin's own layout. The plugin is read for clap.audio-ports while still
 * deactivated and is handed exactly the ports and channel counts it
 * declares -- never more -- but only the main port (index 0) is routed:
 * main input channel k receives host channel min(k, channels-1), every
 * non-main input reads silence (an unrouted sidechain is silent, as in a
 * DAW), main output channel k writes host channel k or is discarded past
 * it, and every non-main output is discarded. A main output narrower than
 * `channels` is not refused: a mono one is duplicated onto the second host
 * channel, and a plugin with no output ports at all produces silence. A
 * main input narrower than `channels` *is* refused -- the host would have
 * to invent a channel -- as is any layout past CLAP_HOST_MAX_PORTS or
 * CLAP_HOST_MAX_PORT_CHAN, a zero-channel port, or a flagged main port
 * that is not first. A plugin without clap.audio-ports has, per the
 * extension's own definition, no audio ports: it is handed none.
 * Returns 0 on success, non-zero on failure (see clap_host_last_error). */
int clap_host_open(const char *path, const char *plugin_id,
                   double sample_rate, double block_size, double channels);

/* Deactivate, destroy, dlclose. Safe when nothing is open. */
void clap_host_close(void);

/* Human-readable reason for the last failure, or "" if none. */
const char *clap_host_last_error(void);

/* Which plugin is loaded, or "" -- for logs and for a driver that wants to
 * assert it opened what it meant to. */
const char *clap_host_plugin_name(void);

/* Its index within the descriptor list of the bundle that was scanned to
 * open it, or -1 when nothing is open. The node-side counterpart is
 * clap_expect(), which is how a model built against one plugin refuses to
 * process through another. */
long clap_host_open_index(void);

/* ------------------------------------------------------------------ *
 * Parameter discovery. The ids are what the node passes to
 * clap_process(), so they are doubles: a clap_id is a uint32 and every
 * uint32 is exactly representable, which is what lets a model name its
 * own parameters with nothing to keep in sync driver-side.
 * ------------------------------------------------------------------ */

long   clap_host_n_params(void);
double clap_host_param_id(long i);       /* -1 when i is out of range */
double clap_host_param_min(long i);
double clap_host_param_max(long i);
double clap_host_param_default(long i);
const char *clap_host_param_name(long i);  /* driver-side: a string */

/* The plugin's own value for a parameter right now, by id. NaN for an
 * unknown id. Reads through CLAP_EXT_PARAMS, so it reflects what the
 * plugin believes rather than what was last sent. */
double clap_host_param_value(double param_id);

/* Latency the plugin reports, in samples (0 when it reports none, or when
 * it does not implement CLAP_EXT_LATENCY).
 *
 * NOT COMPENSATED. This host reports the number and does nothing with it,
 * so hosting a lookahead limiter leaves its output shifted by this many
 * samples relative to the input. Compensating it means delaying the dry
 * path by the same amount, which needs a delay line this host does not
 * have; a model that cares must read this and align downstream itself. */
double clap_host_latency(void);

/* Configuration actually in force, so a test or a driver can assert it
 * rather than assume it. */
double clap_host_sample_rate(void);
double clap_host_block_size(void);
double clap_host_channels(void);
double clap_host_is_open(void);

/* Audio channels the open plugin declares, summed across its ports in each
 * direction -- the `channels` open was given is the host side of the
 * mapping, these are the plugin side, and only the main port of each is
 * actually routed. 0 for a plugin without clap.audio-ports. */
double clap_host_n_audio_in(void);
double clap_host_n_audio_out(void);

/* process() calls since the plugin was opened. The tests assert exactly
 * one per tick. */
long clap_host_n_process(void);
void clap_host_reset_counters(void);

/* ------------------------------------------------------------------ *
 * The input block. Driver-side filling for tests and for a source that
 * lives outside this package; `clap_in_tone` is node-side callable and
 * generates its block from its arguments alone, so a model can be
 * exercised with no driver-side setup at all.
 * ------------------------------------------------------------------ */

/* Fill the input block from caller-supplied interleaved samples and
 * return its token. `n` is per channel. Driver-side. */
double clap_in_fill(const double *samples, long n, long channels);

/* Generate a block of a waveform ending at source time `t`, and return
 * its token: waveform is a CLAP_WAVE_* code, freq in Hz, amp in [0,1].
 * Every argument is a number, so a test can state the expected output in
 * closed form. Node-side callable. */
double clap_in_tone(double t, double waveform, double freq, double amp);

#define CLAP_WAVE_SILENCE 0
#define CLAP_WAVE_SINE    1
#define CLAP_WAVE_SQUARE  2
#define CLAP_WAVE_RAMP    3
#define CLAP_WAVE_IMPULSE 4

/* Read the input block back: `i` is a sample index within the block,
 * `ch` a channel. NaN when `dep` does not name the current input block. */
double clap_in_sample(double dep, double i, double ch);

/* ------------------------------------------------------------------ *
 * The node-side operator. One equation, one call.
 * ------------------------------------------------------------------ */

/* Run the plugin over the block named by `dep` and return the output
 * block's token.
 *
 * Up to CLAP_HOST_PARAM_SLOTS parameters are driven per block by pushing
 * CLAP_EVENT_PARAM_VALUE into the plugin's input event list -- the correct
 * mechanism, rather than poking the controller behind the processor's back.
 * Each slot is an (id, value) pair; a negative id means the slot is unused,
 * and a value is only sent when it differs from what was last sent for that
 * id, so a held-constant parameter costs one event on the first block and
 * none afterwards.
 *
 * Four is a floor, not a ceiling: clap_set_param() below drives any number
 * of parameters by chaining, and the two may be mixed -- the slots and the
 * chain keep separate change-detection state, so driving the same id through
 * both in one block is the one combination that will send two events.
 *
 * Returns NaN when nothing is open or when `dep` does not name the current
 * input block. */
double clap_process(double dep,
                    double id0, double v0, double id1, double v1,
                    double id2, double v2, double id3, double v3);

/* Queue one parameter change for the block named by `dep` and return `dep`,
 * so that driving parameters is a dependency chain: one call per parameter,
 * each taking the previous one's result, and the last one's result passed to
 * clap_process(). That is what lifts the CLAP_HOST_PARAM_SLOTS limit -- a
 * plugin with thirteen parameters is thirteen equations rather than four --
 * and it is a chain rather than a set of independent calls because a
 * synchronous program orders by data dependency and by nothing else.
 *
 * Same change-detection rule as the clap_process() slots, keyed by id: a
 * value equal to the last one sent for that id queues no event, so a held
 * parameter costs one event on the first block and none after.
 *
 * Refuses with NaN -- rather than processing something plausible -- when
 * nothing is open, when `dep` does not name the current input block, when
 * `id` is not a parameter the open plugin declares, when `value` is NaN, or
 * when the queue is full. Filling a new input block abandons whatever chain
 * was pending, so a refused chain cannot leak into the next block. */
double clap_set_param(double dep, double id, double value);

/* Return `dep` when the plugin at descriptor `index` of the scanned bundle
 * is the one open, and NaN otherwise -- the guard a generated per-plugin
 * component puts in front of its parameter chain, so that a model built for
 * one effect refuses rather than silently processing through whichever
 * effect the driver happened to open. NaN in, NaN out. */
double clap_expect(double dep, double index);

/* Read the output block. NaN when `dep` does not name the current output
 * block, which is what makes a stale token a visible error rather than a
 * plausible-looking wrong answer. */
double clap_out_sample(double dep, double i, double ch);
double clap_out_rms(double dep);
double clap_out_peak(double dep);
double clap_out_count(double dep);
double clap_out_valid(double dep);

/* ------------------------------------------------------------------ *
 * CLAP_HOST_REALTIME -- the one place the contract does not line up.
 *
 * CLAP asks that process() be called from a realtime thread which does
 * not allocate, does not block and does not take locks, and it asks the
 * *host* to honour the same discipline. A Dyad solver stepping a clocked
 * partition is not a realtime thread: it may be called from anywhere,
 * a step may be retried, and the surrounding Julia process allocates and
 * GCs freely.
 *
 * For offline work over a fixed source this is harmless -- nobody is
 * listening to the output as it is produced, so a late block is simply a
 * slow simulation. It stops being harmless the moment this is driven from
 * a live capture with a deadline, and a plugin that allocates or blocks
 * inside process() will then produce dropouts that look like a modelling
 * error. Hosting third-party binary code in-process also means a plugin
 * that segfaults takes the Julia process with it: there is no sandbox
 * here, and out-of-process hosting is a different and much larger design.
 * ------------------------------------------------------------------ */

/* Independent instances. The legacy API above keeps its default instance;
 * scanning/opening/closing it does not touch these handles. All calls into
 * this host must be serialized (concurrent lifetimes, not parallel threads).
 * Open returns an exact positive integer double, or NaN on failure. Close is
 * idempotent. Handles are never reused; no pointer crosses the scalar ABI.
 * Every _for entry point addresses one handle. Invalid handles return NaN,
 * -1 or "" as appropriate (is_open and out_valid return 0).
 * Managed block tokens are opaque, negative and unique across instances;
 * stale and foreign tokens are refused. Buffers and event queues belong to
 * each instance. There is no allocation on the processing/copy path.
 * Discovery remains on the default instance; each open caches its own
 * descriptors. Module initialization is shared until the last user closes.
 */
double clap_host_open_instance(const char *path, const char *plugin_id,
                               double sample_rate, double block_size, double channels);
void clap_host_close_instance(double handle);

/* Copy a current output block into another instance's input and return its
 * input token. Requires matching sample rate, block size and host channels;
 * refuses stale output or incompatible configurations with NaN. No latency
 * compensation is performed. Filling abandons pending parameter events just
 * like clap_in_fill. source == destination is allowed for block feedback. */
double clap_in_copy_for(double destination, double source, double dep);

const char * clap_host_plugin_name_for(double handle);
long clap_host_open_index_for(double handle);
long clap_host_n_params_for(double handle);
double clap_host_param_id_for(double handle, long i);
double clap_host_param_min_for(double handle, long i);
double clap_host_param_max_for(double handle, long i);
double clap_host_param_default_for(double handle, long i);
const char * clap_host_param_name_for(double handle, long i);
double clap_host_param_value_for(double handle, double param_id);
double clap_host_latency_for(double handle);
double clap_host_sample_rate_for(double handle);
double clap_host_block_size_for(double handle);
double clap_host_channels_for(double handle);
double clap_host_is_open_for(double handle);
double clap_host_n_audio_in_for(double handle);
double clap_host_n_audio_out_for(double handle);
long clap_host_n_process_for(double handle);
void clap_host_reset_counters_for(double handle);
double clap_in_fill_for(double handle, const double *samples, long n, long channels);
double clap_in_tone_for(double handle, double t, double waveform, double freq, double amp);
double clap_in_sample_for(double handle, double dep, double i, double ch);
double clap_process_for(double handle, double dep, double id0, double v0, double id1, double v1, double id2, double v2, double id3, double v3);
double clap_set_param_for(double handle, double dep, double id, double value);
double clap_expect_for(double handle, double dep, double index);
double clap_out_sample_for(double handle, double dep, double i, double ch);
double clap_out_rms_for(double handle, double dep);
double clap_out_peak_for(double handle, double dep);
double clap_out_count_for(double handle, double dep);
double clap_out_valid_for(double handle, double dep);

#ifdef __cplusplus
}
#endif
#endif /* CLAP_HOST_H */
