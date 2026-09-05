/* probe_two.c -- two plugins in one process, the way a DAW holds a chain:
 * load bundle A and bundle B, instantiate and activate both, and run the
 * same block through each, alternating, so that two juliac plugins with
 * their own Julia runtimes are shown to coexist (or, unprivatised, not).
 *
 *   probe_two <bundle A> <bundle B> <sample_rate> <block_size> <n_blocks> \
 *             [<idA> <valueA> [<idB> <valueB>]]
 *
 * reads block_size * n_blocks samples from stdin and prints, per block,
 * the output of A then of B, one sample per line, prefixed "A " / "B ".
 * One parameter per plugin, sent once before the first block.
 *
 * Build from the repository root (drop -ldl on macOS):
 *   cc -O2 -Icsrc/vendor -o probe_two test/export/probe_two.c -ldl -lm
 */

#include "clap/clap.h"

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BLOCK 4096

static const void *host_get_extension(const clap_host_t *h, const char *id) {
    (void)h; (void)id;
    return NULL;
}
static void host_noop(const clap_host_t *h) { (void)h; }

static const clap_host_t HOST = {
    .clap_version = CLAP_VERSION_INIT, .host_data = NULL, .name = "probe_two",
    .vendor = "AudioPlugins", .url = "", .version = "0",
    .get_extension = host_get_extension, .request_restart = host_noop,
    .request_process = host_noop, .request_callback = host_noop,
};

typedef struct {
    const char *label;
    void *dl;
    const clap_plugin_entry_t *entry;
    const clap_plugin_t *plugin;
    clap_event_param_value_t ev;
    int have_ev;
} slot_t;

static uint32_t ev_size(const clap_input_events_t *l) { return ((slot_t *)l->ctx)->have_ev; }
static const clap_event_header_t *ev_get(const clap_input_events_t *l, uint32_t i) {
    slot_t *s = l->ctx;
    return (i == 0 && s->have_ev) ? &s->ev.header : NULL;
}
static bool ev_try_push(const clap_output_events_t *l, const clap_event_header_t *e) {
    (void)l; (void)e;
    return true;
}

static void *open_bundle(const char *path) {
    void *dl = dlopen(path, RTLD_LOCAL | RTLD_NOW);
    if (dl) return dl;
    char inner[4096], stem[1024];
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(stem, sizeof stem, "%s", base);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = '\0';
    snprintf(inner, sizeof inner, "%s/Contents/MacOS/%s", path, stem);
    return dlopen(inner, RTLD_LOCAL | RTLD_NOW);
}

static int load(slot_t *s, const char *path, double sr, uint32_t block) {
    s->dl = open_bundle(path);
    if (!s->dl) { fprintf(stderr, "%s: dlopen: %s\n", s->label, dlerror()); return 0; }
    s->entry = dlsym(s->dl, "clap_entry");
    if (!s->entry || !s->entry->init(path)) { fprintf(stderr, "%s: no entry\n", s->label); return 0; }
    const clap_plugin_factory_t *f = s->entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    const clap_plugin_descriptor_t *d = f->get_plugin_descriptor(f, 0);
    s->plugin = f->create_plugin(f, &HOST, d->id);
    if (!s->plugin || !s->plugin->init(s->plugin)) { fprintf(stderr, "%s: init\n", s->label); return 0; }
    if (!s->plugin->activate(s->plugin, sr, block, block)) { fprintf(stderr, "%s: activate\n", s->label); return 0; }
    if (!s->plugin->start_processing(s->plugin)) { fprintf(stderr, "%s: start\n", s->label); return 0; }
    printf("# %s %s\n", s->label, d->name);
    return 1;
}

static void set_event(slot_t *s, const char *id, const char *value) {
    memset(&s->ev, 0, sizeof s->ev);
    s->ev.header.size = sizeof s->ev;
    s->ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    s->ev.header.type = CLAP_EVENT_PARAM_VALUE;
    s->ev.param_id = (clap_id)strtoul(id, NULL, 10);
    s->ev.note_id = -1; s->ev.port_index = -1; s->ev.channel = -1; s->ev.key = -1;
    s->ev.value = atof(value);
    s->have_ev = 1;
}

static int run_block(slot_t *s, const float *in, uint32_t n) {
    static float out[MAX_BLOCK];
    float *inp[1] = { (float *)in }, *outp[1] = { out };
    clap_audio_buffer_t ab_in = { .data32 = inp, .data64 = NULL, .channel_count = 1,
                                  .latency = 0, .constant_mask = 0 };
    clap_audio_buffer_t ab_out = { .data32 = outp, .data64 = NULL, .channel_count = 1,
                                   .latency = 0, .constant_mask = 0 };
    const clap_input_events_t iev = { .ctx = s, .size = ev_size, .get = ev_get };
    const clap_output_events_t oev = { .ctx = s, .try_push = ev_try_push };
    clap_process_t pr = { .steady_time = -1, .frames_count = n, .transport = NULL,
                          .audio_inputs = &ab_in, .audio_outputs = &ab_out,
                          .audio_inputs_count = 1, .audio_outputs_count = 1,
                          .in_events = &iev, .out_events = &oev };
    clap_process_status st = s->plugin->process(s->plugin, &pr);
    s->have_ev = 0;                       /* the parameter was delivered once */
    if (st == CLAP_PROCESS_ERROR) { fprintf(stderr, "%s: process error\n", s->label); return 0; }
    for (uint32_t i = 0; i < n; i++) printf("%s %.17g\n", s->label, (double)out[i]);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: %s bundleA bundleB sample_rate block_size n_blocks [idA vA [idB vB]]\n",
                argv[0]);
        return 2;
    }
    double sr = atof(argv[3]);
    long block = atol(argv[4]), nblocks = atol(argv[5]);
    if (block < 1 || block > MAX_BLOCK) { fprintf(stderr, "block size out of range\n"); return 2; }
    slot_t a = { .label = "A" }, b = { .label = "B" };
    if (!load(&a, argv[1], sr, (uint32_t)block)) return 1;
    if (!load(&b, argv[2], sr, (uint32_t)block)) return 1;
    if (argc >= 8) set_event(&a, argv[6], argv[7]);
    if (argc >= 10) set_event(&b, argv[8], argv[9]);

    static float in[MAX_BLOCK];
    for (long k = 0; k < nblocks; k++) {
        for (long i = 0; i < block; i++) {
            double v;
            if (scanf("%lf", &v) != 1) { fprintf(stderr, "short input\n"); return 1; }
            in[i] = (float)v;
        }
        if (!run_block(&a, in, (uint32_t)block)) return 1;
        if (!run_block(&b, in, (uint32_t)block)) return 1;
    }
    a.plugin->stop_processing(a.plugin); a.plugin->deactivate(a.plugin); a.plugin->destroy(a.plugin);
    b.plugin->stop_processing(b.plugin); b.plugin->deactivate(b.plugin); b.plugin->destroy(b.plugin);
    a.entry->deinit();
    b.entry->deinit();
    return 0;
}
