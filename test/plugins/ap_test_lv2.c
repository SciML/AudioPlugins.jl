/* ap_test_lv2.c -- the LV2 bundle of test plugins, ours, for testing the
 * host. Mirrors test/plugins/ap_test_plugins.c (the CLAP bundle): a gain,
 * a one-pole and a 16-sample lookahead, so every expectation in the suite
 * is arithmetic rather than a recording. Port layouts are declared in
 * ap_test_lv2.ttl, which is what the host reads through lilv.
 *
 *   urn:audioplugins:test:gain       ports: 0 gain (ctrl in), 1 in, 2 out
 *   urn:audioplugins:test:onepole    ports: 0 a    (ctrl in), 1 in, 2 out
 *   urn:audioplugins:test:lookahead  ports: 0 in, 1 out, 2 latency (ctrl out)
 */
#include "../../csrc/vendor/lv2/core/lv2.h"
#include <stdlib.h>
#include <string.h>

#define LOOKAHEAD 16

typedef struct {
    const float *ctrl, *in;
    float *out;
    float *latency;
    float  state;                /* one-pole memory */
    float  delay[LOOKAHEAD];     /* lookahead ring */
    int    pos;
} inst_t;

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate,
                              const char *bundle, const LV2_Feature *const *f) {
    (void)d; (void)rate; (void)bundle; (void)f;
    return calloc(1, sizeof(inst_t));
}
static void cleanup(LV2_Handle h) { free(h); }
static const void *extension_data(const char *uri) { (void)uri; return NULL; }

/* --- gain / onepole share a port layout: 0 ctrl, 1 in, 2 out ------------ */
static void connect_ctrl_in_out(LV2_Handle h, uint32_t port, void *data) {
    inst_t *s = (inst_t *)h;
    switch (port) {
    case 0: s->ctrl = (const float *)data; break;
    case 1: s->in   = (const float *)data; break;
    case 2: s->out  = (float *)data; break;
    default: break;
    }
}
static void activate_reset(LV2_Handle h) {
    inst_t *s = (inst_t *)h;
    s->state = 0.0f;
    memset(s->delay, 0, sizeof s->delay);
    s->pos = 0;
    if (s->latency) *s->latency = (float)LOOKAHEAD;
}
static void deactivate(LV2_Handle h) { (void)h; }

static void run_gain(LV2_Handle h, uint32_t n) {
    inst_t *s = (inst_t *)h;
    if (!s->in || !s->out) return;
    float g = s->ctrl ? *s->ctrl : 1.0f;
    for (uint32_t i = 0; i < n; i++) s->out[i] = s->in[i] * g;
}

/* y[n] = y[n-1] + a * (x[n] - y[n-1]); step response 1 - (1-a)^n */
static void run_onepole(LV2_Handle h, uint32_t n) {
    inst_t *s = (inst_t *)h;
    if (!s->in || !s->out) return;
    float a = s->ctrl ? *s->ctrl : 0.5f;
    float y = s->state;
    for (uint32_t i = 0; i < n; i++) {
        y += a * (s->in[i] - y);
        s->out[i] = y;
    }
    s->state = y;
}

/* --- lookahead: 0 in, 1 out, 2 latency ---------------------------------- */
static void connect_lookahead(LV2_Handle h, uint32_t port, void *data) {
    inst_t *s = (inst_t *)h;
    switch (port) {
    case 0: s->in      = (const float *)data; break;
    case 1: s->out     = (float *)data; break;
    case 2: s->latency = (float *)data; break;
    default: break;
    }
}
static void run_lookahead(LV2_Handle h, uint32_t n) {
    inst_t *s = (inst_t *)h;
    if (!s->in || !s->out) return;
    for (uint32_t i = 0; i < n; i++) {
        float x = s->in[i];
        s->out[i] = s->delay[s->pos];
        s->delay[s->pos] = x;
        s->pos = (s->pos + 1) % LOOKAHEAD;
    }
    if (s->latency) *s->latency = (float)LOOKAHEAD;
}

static const LV2_Descriptor DESCS[] = {
    { "urn:audioplugins:test:gain",      instantiate, connect_ctrl_in_out, activate_reset,
      run_gain,      deactivate, cleanup, extension_data },
    { "urn:audioplugins:test:onepole",   instantiate, connect_ctrl_in_out, activate_reset,
      run_onepole,   deactivate, cleanup, extension_data },
    { "urn:audioplugins:test:lookahead", instantiate, connect_lookahead,   activate_reset,
      run_lookahead, deactivate, cleanup, extension_data },
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index) {
    return (index < 3) ? &DESCS[index] : NULL;
}
