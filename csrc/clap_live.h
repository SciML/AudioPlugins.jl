/* Experimental live ABI v1. Build clap_live.c separately with C11 and -pthread.
 * Linux only for now; no changes to the offline host or its ABI. */
#ifndef AP_CLAP_LIVE_H
#define AP_CLAP_LIVE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct ap_live ap_live;
enum { AP_LIVE_OK = 0, AP_LIVE_AGAIN = 1, AP_LIVE_STOPPED = 2,
       AP_LIVE_INVALID = -1, AP_LIVE_THREAD = -2, AP_LIVE_STATE = -3,
       AP_LIVE_PLUGIN = -4, AP_LIVE_PRIORITY = -5, AP_LIVE_UNSUPPORTED = -6 };
enum { AP_LIVE_TIMER = 0, AP_LIVE_DEVICE = 1 };
enum { AP_LIVE_PREPARED = 0, AP_LIVE_RUNNING = 1, AP_LIVE_FINISHED = 2 };
enum { AP_LIVE_PARAM = 0, AP_LIVE_MIDI = 1 };
#define AP_LIVE_MAX_EVENTS 64

typedef struct {
    double sample_rate;
    uint32_t block_size, channels, queue_blocks;
    uint32_t driver;             /* TIMER: native thread; DEVICE: native caller */
    uint32_t priority;           /* 0: normal, 1: best effort FIFO, 2: strict FIFO */
} ap_live_config;

/* Events are ordered by frame within their accompanying block. Parameter values
 * are plain units. MIDI is three-byte MIDI 1.0 on note port zero. */
typedef struct {
    uint32_t type, frame, param_id;
    uint8_t midi[4];
    double value;
} ap_live_event;

typedef struct {
    uint32_t state, priority_granted, blocks, underruns, overruns;
    uint32_t deadline_misses, process_errors, output_events_dropped;
    uint32_t restart_requested, callback_requested;
    int32_t error;
} ap_live_stats;

uint32_t ap_live_abi_version(void);
/* All control and queue operations must run on the OS thread that opens the
 * session. Use a stable main/control thread, never a migratable Julia task.
 * Calls on different sessions are allowed. Plugin entry init/deinit and offline
 * module operations must be serialized on this same thread. */
int ap_live_open(const char *binary, const char *id, const ap_live_config *, ap_live **);
int ap_live_start(ap_live *);
/* TIMER: join, deactivate. DEVICE: request stop and return AGAIN until the
 * native device owner has called device_end and quiesced its callbacks. */
int ap_live_stop(ap_live *);
/* Does not implicitly stop: refuses a running/unjoined session. On success the
 * pointer is invalid; caller must exclude all concurrent/future accesses. */
int ap_live_close(ap_live *);
/* Services one coalesced main-thread callback. A restart request stops TIMER
 * sessions; DEVICE sessions first need device_end. Returns STOPPED after
 * deactivation; reopen to renegotiate ports/parameters before restarting. */
int ap_live_poll(ap_live *);

/* Nonblocking copies of exactly block_size * channels interleaved floats.
 * Queue full/empty returns AGAIN. No pointer is retained. Input sequence is an
 * application tag, output tick is the processing sequence (gaps expose drops).
 * UINT64_MAX input sequence in an output means silence due to missing input.
 * Producer and consumer are the control thread; no concurrent queue callers.
 * A stop/start discards queued audio and preserves plugin parameters. */
int ap_live_try_write(ap_live *, const float *, const ap_live_event *, uint32_t n,
                      uint64_t input_sequence);
int ap_live_try_read(ap_live *, float *, uint64_t *tick, uint64_t *input_sequence);
/* Independent atomic counters, not a transactionally consistent snapshot.
 * Counters wrap modulo 2^32; tick and input_sequence are 64-bit ring payloads. */
int ap_live_get_stats(const ap_live *, ap_live_stats *);

/* Native device backend contract (never call these from Julia): start arms the
 * session on control; begin/process/end run on one stable native audio thread.
 * begin enters processing, process consumes the Julia ring and optionally
 * copies output directly to a device buffer. The device clock owns cadence.
 * Call end even after process returns STOPPED/error, then quiesce callbacks
 * before control calls stop/close. A concurrent/wrong-thread call is rejected.
 * Capture routing and variable device block sizes belong to the device adapter.
 * No device dependency, device discovery, or resampler is supplied by this ABI. */
int ap_live_device_begin(ap_live *);
int ap_live_device_process(ap_live *, float *device_output);
int ap_live_device_end(ap_live *);
#ifdef __cplusplus
}
#endif
#endif
