/* Internal processor interface: CLAP's lifecycle/event structs also serve as
 * the native engine's format-neutral vtable. LV2/VST3 adapters are private
 * objects, never exported as CLAP binaries, and never touch offline hosts. */
#ifndef AP_LIVE_ADAPTER_H
#define AP_LIVE_ADAPTER_H
#include "clap_live.h"
#include "vendor/clap/clap.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef const clap_plugin_t *(*ap_adapter_create)(const clap_host_t *, const char *, const char *, const ap_live_config *);
int ap_live_open_adapter(const char *, const char *, const ap_live_config *, ap_live **, ap_adapter_create);
int ap_live_open_lv2(const char *, const char *, const ap_live_config *, ap_live **);
int ap_live_open_vst3(const char *, const char *, const ap_live_config *, ap_live **);
#ifdef __cplusplus
}
#endif
#endif
