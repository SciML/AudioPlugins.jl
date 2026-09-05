/* probe_state.c -- what a DAW does with clap.state, done to an exported
 * plugin by a minimal host: set a parameter, save, load the blob into a
 * fresh instance, and refuse garbage.
 *
 *   probe_state <bundle> <param_id> <value>
 *
 * prints, one per line:
 *   set <value the plugin reports after the parameter event>
 *   saved <blob length in bytes>
 *   fresh <value a new instance reports before loading>
 *   loaded <value it reports after loading the blob>
 *   garbage <rejected|accepted>
 * and exits non-zero when the plugin has no clap.state at all. The memory
 * streams deliver a few bytes per call, so partial reads and writes are
 * exercised the way a real host's streams would.
 *
 * Build from the repository root (drop -ldl on macOS and Windows):
 *   cc -O2 -Icsrc/vendor -o probe_state test/export/probe_state.c -ldl
 */

#include "clap/clap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static void *load_library(const char *path) { return (void *)LoadLibraryA(path); }
static void *load_symbol(void *h, const char *name) {
    return (void *)(uintptr_t)GetProcAddress((HMODULE)h, name);
}
static const char *load_error(void) { return "LoadLibrary failed"; }
#else
#include <dlfcn.h>
static void *load_library(const char *path) { return dlopen(path, RTLD_LOCAL | RTLD_NOW); }
static void *load_symbol(void *h, const char *name) { return dlsym(h, name); }
static const char *load_error(void) { return dlerror(); }
#endif

/* --- a host that offers nothing ---------------------------------- */

static const void *host_get_extension(const clap_host_t *h, const char *id) {
    (void)h; (void)id;
    return NULL;
}
static void host_noop(const clap_host_t *h) { (void)h; }

static const clap_host_t HOST = {
    .clap_version = CLAP_VERSION_INIT, .host_data = NULL, .name = "probe_state",
    .vendor = "AudioPlugins", .url = "", .version = "0",
    .get_extension = host_get_extension, .request_restart = host_noop,
    .request_process = host_noop, .request_callback = host_noop,
};

/* --- memory streams that move at most 3 bytes per call ----------- */

typedef struct { unsigned char *buf; size_t len, cap, pos; } mem_t;

static int64_t mem_write(const clap_ostream_t *s, const void *b, uint64_t n) {
    mem_t *m = s->ctx;
    if (n > 3) n = 3;
    if (m->len + n > m->cap) {
        m->cap = (m->len + n) * 2;
        m->buf = realloc(m->buf, m->cap);
        if (!m->buf) return -1;
    }
    memcpy(m->buf + m->len, b, n);
    m->len += n;
    return (int64_t)n;
}

static int64_t mem_read(const clap_istream_t *s, void *b, uint64_t n) {
    mem_t *m = s->ctx;
    size_t avail = m->len - m->pos;
    if (n > avail) n = avail;
    if (n > 3) n = 3;
    memcpy(b, m->buf + m->pos, n);
    m->pos += n;
    return (int64_t)n;
}

/* --- one parameter event ----------------------------------------- */

static clap_event_param_value_t EV;

static uint32_t ev_size(const clap_input_events_t *l) { (void)l; return 1; }
static const clap_event_header_t *ev_get(const clap_input_events_t *l, uint32_t i) {
    (void)l;
    return i == 0 ? &EV.header : NULL;
}
static bool ev_try_push(const clap_output_events_t *l, const clap_event_header_t *e) {
    (void)l; (void)e;
    return true;
}

static void *open_bundle(const char *path) {
    void *dl = load_library(path);
    if (dl) return dl;
    /* macOS: Name.clap/Contents/MacOS/Name */
    char inner[4096], stem[1024];
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(stem, sizeof stem, "%s", base);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = '\0';
    snprintf(inner, sizeof inner, "%s/Contents/MacOS/%s", path, stem);
    return load_library(inner);
}

static double value_of(const clap_plugin_t *p, const clap_plugin_params_t *params, clap_id id) {
    double v = 0.0;
    if (!params->get_value(p, id, &v)) { fprintf(stderr, "get_value(%u) failed\n", id); exit(1); }
    return v;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s bundle param_id value\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    clap_id id = (clap_id)strtoul(argv[2], NULL, 10);
    double value = atof(argv[3]);

    void *dl = open_bundle(path);
    if (!dl) { fprintf(stderr, "load: %s\n", load_error()); return 1; }
    const clap_plugin_entry_t *entry = load_symbol(dl, "clap_entry");
    if (!entry) { fprintf(stderr, "no clap_entry\n"); return 1; }
    if (!entry->init(path)) { fprintf(stderr, "entry init failed\n"); return 1; }
    const clap_plugin_factory_t *factory = entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    const clap_plugin_descriptor_t *desc = factory->get_plugin_descriptor(factory, 0);
    const clap_plugin_t *a = factory->create_plugin(factory, &HOST, desc->id);
    if (!a || !a->init(a)) { fprintf(stderr, "create/init failed\n"); return 1; }

    const clap_plugin_params_t *params = a->get_extension(a, CLAP_EXT_PARAMS);
    const clap_plugin_state_t *state = a->get_extension(a, CLAP_EXT_STATE);
    if (!params) { fprintf(stderr, "no clap.params\n"); return 1; }
    if (!state) { fprintf(stderr, "no clap.state\n"); return 3; }

    memset(&EV, 0, sizeof EV);
    EV.header.size = sizeof EV;
    EV.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    EV.header.type = CLAP_EVENT_PARAM_VALUE;
    EV.param_id = id;
    EV.note_id = -1; EV.port_index = -1; EV.channel = -1; EV.key = -1;
    EV.value = value;
    const clap_input_events_t in = { .ctx = NULL, .size = ev_size, .get = ev_get };
    const clap_output_events_t out = { .ctx = NULL, .try_push = ev_try_push };
    params->flush(a, &in, &out);
    printf("set %.17g\n", value_of(a, params, id));

    mem_t blob = { NULL, 0, 0, 0 };
    const clap_ostream_t os = { .ctx = &blob, .write = mem_write };
    if (!state->save(a, &os)) { fprintf(stderr, "save failed\n"); return 1; }
    printf("saved %zu\n", blob.len);

    const clap_plugin_t *b = factory->create_plugin(factory, &HOST, desc->id);
    if (!b || !b->init(b)) { fprintf(stderr, "second create/init failed\n"); return 1; }
    printf("fresh %.17g\n", value_of(b, params, id));
    const clap_istream_t is = { .ctx = &blob, .read = mem_read };
    if (!state->load(b, &is)) { fprintf(stderr, "load failed\n"); return 1; }
    printf("loaded %.17g\n", value_of(b, params, id));

    mem_t junk = { (unsigned char *)"not a state blob at all", 23, 23, 0 };
    const clap_istream_t junk_is = { .ctx = &junk, .read = mem_read };
    printf("garbage %s\n", state->load(b, &junk_is) ? "accepted" : "rejected");
    printf("after_garbage %.17g\n", value_of(b, params, id));

    a->destroy(a);
    b->destroy(b);
    entry->deinit();
    free(blob.buf);
    return 0;
}
