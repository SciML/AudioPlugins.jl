/* probe_airwindows.c -- C-level verification of csrc/airwindows, the
 * Airwindows collection as one CLAP module, hosted through the package's own
 * clap_host.c. Every expectation is arithmetic or a count, not a recording.
 *
 * Three effects carry the arithmetic, because in a collection of 504 mostly
 * analogue-modelling processors almost nothing has a closed form:
 *
 *   LeftoMono   out1 = in1, out2 = in1     -- exact, and it has no parameters
 *   RightoMono  out1 = in2, out2 = in2     -- the same, off the other channel
 *   DCVoltage   out = in + (2A - 1)        -- exact, and it has one parameter
 *
 * Between them they cover both shapes the params extension has to handle, and
 * the pair of mono-ers is what discriminates the aliasing bug: an adapter that
 * handed the same pointer over as both input channels passes the LeftoMono
 * check and fails the RightoMono one, provided -- as here -- the two input
 * channels are fed different signals.
 *
 * Build and run from the repository root, against a checkout of airwin2rack
 * whose `airwin-registry` target has already been built:
 *
 *   AW=$HOME/sandbox/airwin2rack
 *   c++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -isystem $AW/src \
 *       -o Airwindows.clap csrc/airwindows/airwindows_clap.cpp \
 *       -L$AW/build -lairwin-registry -lawdoc_resources -Wl,--no-undefined
 *   cc -std=gnu99 -O2 -Wall -Wextra -o probe_airwindows \
 *       test/probe_airwindows.c csrc/clap_host.c -ldl -lm
 *   ./probe_airwindows "$PWD/Airwindows.clap"
 *
 * argv[2] optionally overrides the expected plugin count, for a run against a
 * revision of airwin2rack other than the pinned one. */

#include "../csrc/clap_host.h"
#include "../csrc/vendor/clap/clap.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  define DL_OPEN(p)     ((void *)LoadLibraryA(p))
#  define DL_SYM(h, s)   ((void *)GetProcAddress((HMODULE)(h), (s)))
#  define DL_CLOSE(h)    FreeLibrary((HMODULE)(h))
#else
#  include <dlfcn.h>
#  define DL_OPEN(p)     dlopen((p), RTLD_NOW | RTLD_LOCAL)
#  define DL_SYM(h, s)   dlsym((h), (s))
#  define DL_CLOSE(h)    dlclose(h)
#endif

/* What the registry holds at the airwin2rack commit .github/workflows pins.
 * The number matters twice over: it is the collection's size, and it is well
 * past the 32 a fixed-size descriptor cache used to stop at, so a scan that
 * silently truncated would show up here as 32 rather than as nothing. */
#define AW_N_PLUGINS 504
#define AW_BLOCK 64

static int fails;
static void ck(int ok, const char *what) {
    printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

/* ---------------------------------------------------------------- *
 * A direct CLAP client, for clap.state
 *
 * clap_host.c drives audio and parameters but exposes no state extension, so
 * the save/load round trip is checked by talking to the module itself. It is
 * the same bundle the host above scanned; a second dlopen of it is refcounted
 * by the loader, which is exactly the case entry->init has to survive.
 * ---------------------------------------------------------------- */

typedef struct { uint8_t buf[4096]; size_t n, rd; } mem_t;

static int64_t mem_write(const clap_ostream_t *s, const void *b, uint64_t n) {
    mem_t *m = s->ctx;
    if (m->n + n > sizeof m->buf) return -1;
    memcpy(m->buf + m->n, b, (size_t)n);
    m->n += (size_t)n;
    return (int64_t)n;
}

static int64_t mem_read(const clap_istream_t *s, void *b, uint64_t n) {
    mem_t *m = s->ctx;
    size_t left = m->n - m->rd;
    if (n > left) n = left;
    memcpy(b, m->buf + m->rd, (size_t)n);
    m->rd += (size_t)n;
    return (int64_t)n;
}

typedef struct { clap_event_param_value_t ev; int n; } evlist_t;

static uint32_t ev_size(const clap_input_events_t *l) { return (uint32_t)((evlist_t *)l->ctx)->n; }

static const clap_event_header_t *ev_get(const clap_input_events_t *l, uint32_t i) {
    evlist_t *e = l->ctx;
    return (i < (uint32_t)e->n) ? &e->ev.header : NULL;
}

static bool ev_push(const clap_output_events_t *l, const clap_event_header_t *h) {
    (void)l; (void)h;
    return true;
}

static const void *no_ext(const clap_host_t *h, const char *id) { (void)h; (void)id; return NULL; }
static void no_op(const clap_host_t *h) { (void)h; }

static const clap_host_t HOST = {
    CLAP_VERSION_INIT, NULL, "AudioPlugins probe", "JuliaHub", "", "0.1.0",
    no_ext, no_op, no_op, no_op,
};

/* Send one parameter value through params->flush, the route a host uses when
 * the plugin is not processing. */
static void flush_param(const clap_plugin_t *p, const clap_plugin_params_t *pe,
                        clap_id id, double v) {
    evlist_t e = { { { sizeof(clap_event_param_value_t), 0, CLAP_CORE_EVENT_SPACE_ID,
                       CLAP_EVENT_PARAM_VALUE, 0 },
                     id, NULL, -1, -1, -1, -1, v } , 1 };
    clap_input_events_t in = { &e, ev_size, ev_get };
    clap_output_events_t out = { NULL, ev_push };
    pe->flush(p, &in, &out);
}

static void probe_state(const char *bundle) {
    void *dl = DL_OPEN(bundle);
    ck(dl != NULL, "state: the bundle opens a second time");
    if (!dl) return;

    const clap_plugin_entry_t *entry = DL_SYM(dl, "clap_entry");
    ck(entry != NULL, "state: clap_entry resolves");
    if (!entry) { DL_CLOSE(dl); return; }
    ck(entry->init(bundle), "state: entry init succeeds on a second reference");

    const clap_plugin_factory_t *f = entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    ck(f != NULL, "state: the plugin factory is there");
    if (!f) { entry->deinit(); DL_CLOSE(dl); return; }

    const clap_plugin_t *p = f->create_plugin(f, &HOST, "org.airwindows.DCVoltage");
    ck(p != NULL, "state: DCVoltage instantiates");
    if (!p) { entry->deinit(); DL_CLOSE(dl); return; }
    ck(p->init(p), "state: it initialises");

    const clap_plugin_params_t *pe = p->get_extension(p, CLAP_EXT_PARAMS);
    const clap_plugin_state_t *se = p->get_extension(p, CLAP_EXT_STATE);
    ck(pe != NULL && se != NULL, "state: params and state extensions present");
    if (!pe || !se) { p->destroy(p); entry->deinit(); DL_CLOSE(dl); return; }

    flush_param(p, pe, 0, 0.875);
    double v = -1.0;
    ck(pe->get_value(p, 0, &v) && fabs(v - 0.875) < 1e-6, "state: parameter set to 0.875");

    mem_t m = { { 0 }, 0, 0 };
    clap_ostream_t os = { &m, mem_write };
    ck(se->save(p, &os), "state: save succeeds");
    ck(m.n == 16, "state: one parameter saves as a 16-byte blob");

    flush_param(p, pe, 0, 0.125);
    ck(pe->get_value(p, 0, &v) && fabs(v - 0.125) < 1e-6, "state: parameter moved to 0.125");

    clap_istream_t is = { &m, mem_read };
    ck(se->load(p, &is), "state: load succeeds");
    ck(pe->get_value(p, 0, &v) && fabs(v - 0.875) < 1e-6,
       "state: load restores 0.875, so the round trip holds");

    /* A blob that is not ours must be refused rather than half-applied. */
    mem_t junk = { { 0 }, 0, 0 };
    memcpy(junk.buf, "NOPE\0\0\0\0\0\0\0\0", 12);
    junk.n = 12;
    clap_istream_t bad = { &junk, mem_read };
    ck(!se->load(p, &bad), "state: a blob with the wrong magic is refused");
    ck(pe->get_value(p, 0, &v) && fabs(v - 0.875) < 1e-6, "  ... leaving the value alone");

    p->destroy(p);
    entry->deinit();
    DL_CLOSE(dl);
}

/* ---------------------------------------------------------------- *
 * The probe proper
 * ---------------------------------------------------------------- */

static int feature_present(long i, const char *want) {
    long n = clap_host_scan_n_features(i);
    for (long k = 0; k < n; k++)
        if (strcmp(clap_host_scan_feature(i, k), want) == 0) return 1;
    return 0;
}

static long index_of(const char *id) {
    long n = clap_host_scan_count();
    for (long i = 0; i < n; i++)
        if (strcmp(clap_host_scan_id(i), id) == 0) return i;
    return -1;
}

int main(int argc, char **argv) {
    const char *BUNDLE = argc > 1 ? argv[1] : "Airwindows.clap";
    const long WANT = argc > 2 ? atol(argv[2]) : AW_N_PLUGINS;

    /* --- discovery --------------------------------------------------- */
    long n = clap_host_scan(BUNDLE);
    printf("   scanned %ld plugins from %s\n", n, BUNDLE);
    ck(n == WANT, "the factory reports the whole registry");
    ck(n > 32, "  ... which is more than a 32-entry cache would have shown");
    ck(clap_host_scan_count() == n, "scan_count agrees with what scan returned");
    ck(strcmp(clap_host_scan_id(n), "") == 0, "one past the end is empty");

    /* Ids are derived from effect names, so the derivation has to be
     * injective over whatever registry this was built against. */
    int dupes = 0, misprefixed = 0;
    for (long i = 0; i < n; i++) {
        if (strncmp(clap_host_scan_id(i), "org.airwindows.", 15) != 0) misprefixed++;
        for (long j = i + 1; j < n; j++)
            if (strcmp(clap_host_scan_id(i), clap_host_scan_id(j)) == 0) { dupes++; break; }
    }
    ck(misprefixed == 0, "every id is under org.airwindows.");
    ck(dupes == 0, "no two effects sanitise to the same id");

    long gi = index_of("org.airwindows.Galactic");
    ck(gi >= 0, "Galactic is in the bundle, found by id and not by index");
    ck(strcmp(clap_host_scan_name(gi), "Galactic") == 0, "  its name is the registry name");
    ck(strcmp(clap_host_scan_vendor(gi), "Airwindows") == 0, "  vendor is Airwindows");
    ck(strlen(clap_host_scan_version(gi)) > 0, "  a version string is set");
    ck(strstr(clap_host_scan_description(gi), "super-reverb") != NULL,
       "  description is the registry whatText");
    ck(feature_present(gi, "audio-effect"), "  declares audio-effect");
    ck(feature_present(gi, "stereo"), "  declares stereo");
    ck(feature_present(gi, "reverb"), "  Reverb maps to the reverb keyword");
    ck(feature_present(gi, "airwindows:Reverb"), "  and carries its category verbatim");

    /* --- Galactic: parameters, and a reverb that must alter the signal -- */
    ck(clap_host_open(BUNDLE, "org.airwindows.Galactic", 48000, AW_BLOCK, 2) == 0,
       "open Galactic");
    ck(strcmp(clap_host_plugin_name(), "Galactic") == 0, "  the host opened Galactic");
    ck(clap_host_n_params() == 5, "  it exposes 5 parameters");
    ck(strcmp(clap_host_param_name(0), "Replace") == 0, "  parameter 0 is Replace");
    ck(strcmp(clap_host_param_name(4), "Dry/Wet") == 0, "  parameter 4 is Dry/Wet");
    ck(clap_host_param_min(0) == 0.0 && clap_host_param_max(0) == 1.0, "  range 0..1");
    ck(fabs(clap_host_param_default(0) - 0.5) < 1e-6, "  Replace defaults to 0.5");
    ck(fabs(clap_host_param_default(4) - 1.0) < 1e-6, "  Dry/Wet defaults to 1.0");
    ck(clap_host_latency() == 0.0, "  and reports no latency");

    double in[2 * AW_BLOCK];
    for (int i = 0; i < AW_BLOCK; i++) {
        in[2 * i] = 0.5 * sin(2.0 * M_PI * i / 16.0);      /* L */
        in[2 * i + 1] = -0.25;                             /* R, distinct from L */
    }
    /* Fully wet, so the dry path cannot be what comes back. A reverb that
     * returned its input unchanged would be a passthrough with a long name. */
    double tok = clap_in_fill(in, AW_BLOCK, 2);
    double out = clap_process(tok, 4, 1.0, 3, 1.0, -1, 0, -1, 0);
    ck(!isnan(out), "  process returns an output token");
    ck(clap_host_n_process() == 1, "  exactly one process() call");

    int changed = 0, finite = 1;
    for (int i = 0; i < AW_BLOCK; i++) {
        double y = clap_out_sample(out, i, 0);
        if (!isfinite(y)) finite = 0;
        if (fabs(y - in[2 * i]) > 1e-6) changed++;
    }
    printf("   Galactic altered %d of %d samples on the left channel\n", changed, AW_BLOCK);
    ck(finite, "  every output sample is finite");
    ck(changed > AW_BLOCK / 2, "  and the reverb actually alters the signal");

    /* --- LeftoMono: no parameters at all, and exact arithmetic --------- */
    ck(clap_host_open(BUNDLE, "org.airwindows.LeftoMono", 48000, AW_BLOCK, 2) == 0,
       "open LeftoMono, one of the 47 effects with no parameters");
    ck(clap_host_n_params() == 0, "  it exposes no parameters");
    ck(clap_host_param_id(0) == -1.0, "  and asking for one is refused, not answered");

    double t2 = clap_in_fill(in, AW_BLOCK, 2);
    double o2 = clap_process(t2, -1, 0, -1, 0, -1, 0, -1, 0);
    int exact_l = 1, exact_r = 1;
    for (int i = 0; i < AW_BLOCK; i++) {
        if (clap_out_sample(o2, i, 0) != clap_in_sample(t2, i, 0)) exact_l = 0;
        if (clap_out_sample(o2, i, 1) != clap_in_sample(t2, i, 0)) exact_r = 0;
    }
    ck(exact_l, "  out[0] == in[0], sample for sample");
    ck(exact_r, "  out[1] == in[0] too: L is copied over R, losslessly");
    ck(fabs(clap_in_sample(t2, 3, 1) - (-0.25)) < 1e-7,
       "  the right input really did differ, so that was not a tautology");

    /* --- RightoMono: the same, off the channel LeftoMono ignored ------- */
    ck(clap_host_open(BUNDLE, "org.airwindows.RightoMono", 48000, AW_BLOCK, 2) == 0,
       "open RightoMono");
    double t2r = clap_in_fill(in, AW_BLOCK, 2);
    double o2r = clap_process(t2r, -1, 0, -1, 0, -1, 0, -1, 0);
    int from_right = 1;
    for (int i = 0; i < AW_BLOCK; i++) {
        if (clap_out_sample(o2r, i, 0) != clap_in_sample(t2r, i, 1)) from_right = 0;
        if (clap_out_sample(o2r, i, 1) != clap_in_sample(t2r, i, 1)) from_right = 0;
    }
    ck(from_right, "  both outputs are in[1], so the two inputs are distinct memory");

    /* --- DCVoltage: one parameter, and out = in + (2A - 1) ------------- */
    ck(clap_host_open(BUNDLE, "org.airwindows.DCVoltage", 48000, AW_BLOCK, 2) == 0,
       "open DCVoltage");
    ck(clap_host_n_params() == 1, "  it exposes 1 parameter");
    ck(strcmp(clap_host_param_name(0), "Voltage") == 0, "  named Voltage");
    ck(fabs(clap_host_param_default(0) - 0.5) < 1e-6, "  defaulting to 0.5, which is 0 volts");

    double t3 = clap_in_fill(in, AW_BLOCK, 2);
    double o3 = clap_process(t3, 0, 0.75, -1, 0, -1, 0, -1, 0);
    int arith = 1;
    for (int i = 0; i < AW_BLOCK; i++) {
        float want = (float)((double)(float)in[2 * i] + 0.5);
        if (fabs(clap_out_sample(o3, i, 0) - (double)want) > 1e-7) { arith = 0; break; }
    }
    ck(arith, "  A=0.75 adds exactly 0.5 to every sample");
    ck(fabs(clap_host_param_value(0) - 0.75) < 1e-9, "  the plugin's own value reads back 0.75");

    double t4 = clap_in_fill(in, AW_BLOCK, 2);
    double o4 = clap_process(t4, 0, 0.5, -1, 0, -1, 0, -1, 0);
    int unity = 1;
    for (int i = 0; i < AW_BLOCK; i++)
        if (clap_out_sample(o4, i, 1) != clap_in_sample(t4, i, 1)) { unity = 0; break; }
    ck(unity, "  and A=0.5 is 0 volts: a bit-exact passthrough on both channels");

    /* --- every effect in the bundle, once ------------------------------
     * The checks above name four effects; the other 500 are only covered by
     * running them. One block each is enough to catch the failures that
     * matter at this layer -- an effect that will not instantiate, one that
     * refuses a stereo port, one that returns NaN from an uninitialised
     * member -- and none of those would show up in a spot check. */
    long bad_open = 0, bad_audio = 0;
    const char *worst = "";
    for (long i = 0; i < n; i++) {
        char id[256];
        /* Copied before the call: clap_host_open rescans the bundle, which
         * refills the very cache this string lives in. */
        snprintf(id, sizeof id, "%s", clap_host_scan_id(i));
        if (clap_host_open(BUNDLE, id, 48000, AW_BLOCK, 2) != 0) {
            if (!bad_open) worst = clap_host_last_error();
            bad_open++;
            continue;
        }
        double t = clap_in_fill(in, AW_BLOCK, 2);
        double o = clap_process(t, -1, 0, -1, 0, -1, 0, -1, 0);
        for (int k = 0; k < AW_BLOCK; k++) {
            if (isfinite(clap_out_sample(o, k, 0)) && isfinite(clap_out_sample(o, k, 1)))
                continue;
            bad_audio++;
            break;
        }
    }
    printf("   swept %ld effects: %ld failed to open, %ld produced non-finite audio\n",
           n, bad_open, bad_audio);
    if (bad_open) printf("   first open failure: %s\n", worst);
    ck(bad_open == 0, "every effect in the registry instantiates and activates");
    ck(bad_audio == 0, "every effect returns finite audio for a stereo block");

    /* --- a bad id is refused, naming itself ---------------------------- */
    ck(clap_host_open(BUNDLE, "org.airwindows.NoSuchEffect", 48000, AW_BLOCK, 2) != 0,
       "an id that is not in the registry fails");
    ck(strstr(clap_host_last_error(), "NoSuchEffect") != NULL, "  ... naming the id asked for");

    clap_host_close();
    ck(clap_host_is_open() == 0.0, "closed");

    probe_state(BUNDLE);

    printf("\n%s (%d failure%s)\n", fails ? "FAILURES" : "ALL PROBES PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
