#ifndef AP_LIVE_DEVICE_H
#define AP_LIVE_DEVICE_H
#include "clap_live.h"
typedef struct ap_audio_device ap_audio_device;
typedef struct {
    uint32_t underruns, overruns, lost, buffering_frames;
    uint32_t device_period_frames, device_sample_rate, capture, backend, workgroup_joined;
} ap_device_stats;
#ifdef __cplusplus
extern "C" {
#endif
/* Public control calls. Backend "null" is explicit, only for hardware-free
 * tests. The default backend never falls back to a fake successful device.
 * Indices are from a fresh enumeration and -1 selects the default device. */
int ap_live_configure_device(ap_live *, const char *backend, int capture, int playback_index, int capture_index);
int ap_live_audio_devices(const char *backend, int capture, int index, char *name, uint32_t capacity);
int ap_live_get_device_stats(ap_live *, ap_device_stats *);

/* Internal driver interface. wait/submit belong to the stable processing
 * worker; callbacks never wait for it, Julia, or a plugin. */
ap_audio_device *ap_audio_open(const ap_live_config *, const char *, int, int, int);
void ap_audio_close(ap_audio_device *);
int ap_audio_enter(ap_audio_device *);
void ap_audio_leave(ap_audio_device *);
void ap_audio_prepare(ap_audio_device *);
int ap_audio_start(ap_audio_device *);
void ap_audio_stop(ap_audio_device *);
void ap_audio_wake(ap_audio_device *);
int ap_audio_wait(ap_audio_device *, const float **capture, uint64_t *sequence);
void ap_audio_submit(ap_audio_device *, const float *, uint64_t sequence);
void ap_audio_stats(ap_audio_device *, ap_device_stats *);
#ifdef __cplusplus
}
#endif
#endif
