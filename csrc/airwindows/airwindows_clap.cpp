/* airwindows_clap.cpp -- the Airwindows effect collection as a single CLAP
 * module, with no plugin framework underneath it.
 *
 * baconpaul/airwin2rack consolidates Chris Johnson's Airwindows effects into
 * one MIT-licensed static library, `airwin-registry`, which hands out a table
 * of 504 effects and a factory lambda for each. This file is the whole of the
 * adapter from that table to CLAP: a descriptor per registry entry, the three
 * extensions a host needs to drive an effect (audio-ports, params, state),
 * and a process() that copies samples in and out. Nothing else.
 *
 * The point of it being thin is the licence boundary. airwin-registry and
 * src/autogen_airwin are MIT; the GPL3 material in that repository is the
 * JUCE and Rack front ends under src-juce/ and res/. Linking only the
 * registry keeps the resulting .clap MIT, which is what lets it ship as a
 * JLL. Do not add src-juce to the link line.
 *
 * The .clap is an ordinary shared object exporting only clap_entry; the whole
 * build is one c++ -shared against -lairwin-registry -lawdoc_resources, which
 * is what .github/workflows/CProbe.yml runs and what a Yggdrasil recipe does.
 *
 * NOT IMPLEMENTED, deliberately and stated here rather than discovered:
 *
 *  - The float64 path. Every effect implements processDoubleReplacing, and
 *    the base class's canDoubleReplacing() returns false for all of them
 *    regardless, so the query is useless; this adapter simply never declares
 *    CLAP_AUDIO_PORT_SUPPORTS_64BITS and always runs the float32 path.
 *  - Sample-accurate parameter automation. Parameter events are applied at
 *    the top of the block rather than splitting the block at each event's
 *    frame. Airwindows setParameter is a bare assignment with no smoothing,
 *    so a split block would step just as hard as an unsplit one; what is
 *    lost is the sub-block timing, not the smoothness.
 *  - clap.latency and clap.tail. No effect in the collection reports any
 *    latency -- there is no setInitialDelay anywhere in the tree -- so there
 *    is nothing for a latency extension to report.
 */

#include "AirwinRegistry.h"

#include "../vendor/clap/clap.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef AIRWINDOWS_VERSION
#  define AIRWINDOWS_VERSION "1.0.0"
#endif

/* getParameterName/Label/Display write through a char* with no length, and
 * vst_strncpy does not terminate on truncation, so the buffer is oversized
 * against their kVstMaxParamStrLen of 32, and zeroed before every call. */
#define AW_TEXT_BUF 64

/* ---------------------------------------------------------------- *
 * 1. The registry, read once
 *
 * completeRegistry() is what fills nameToIndex and the category maps, and it
 * appends to ordering vectors rather than rebuilding them, so running it
 * twice duplicates entries -- hence the latch rather than a refcount.
 * ---------------------------------------------------------------- */

static bool REGISTRY_READY = false;

/* One CLAP descriptor and the storage its char pointers refer into. The
 * descriptor is filled in a second pass, after the vector has stopped
 * growing, because a reallocation would move every string it points at. */
struct Entry {
    std::string id;
    std::string name;
    std::string what;
    std::string catfeat;
    std::vector<const char *> features;
    clap_plugin_descriptor_t desc;
};

static std::vector<Entry> *ENTRIES;
static std::unordered_map<std::string, int> *BY_ID;

/* An id has to survive an upstream insertion into the middle of the registry,
 * so it comes from the effect's name and never from its index. Every
 * character outside [A-Za-z0-9] becomes '-'; no name currently holds one, and
 * test/probe_airwindows.c re-checks the 504 ids are distinct on whatever
 * revision it was built against. */
static std::string id_for(const std::string &name) {
    std::string s = "org.airwindows.";
    for (char c : name) {
        bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        s += plain ? c : '-';
    }
    return s;
}

/* Airwindows' 23 categories onto CLAP's coarser vocabulary: several land on
 * the same keyword and "Unclassified" on none. The category also goes out
 * verbatim as an "airwindows:" feature, so nothing is lost in translation. */
static const char *clap_keyword(const std::string &cat) {
    if (cat == "Ambience" || cat == "Reverb")        return CLAP_PLUGIN_FEATURE_REVERB;
    if (cat == "Amp Sims" || cat == "Distortion")    return CLAP_PLUGIN_FEATURE_DISTORTION;
    if (cat == "Saturation" || cat == "Tape")        return CLAP_PLUGIN_FEATURE_DISTORTION;
    if (cat == "Lo-Fi")                              return CLAP_PLUGIN_FEATURE_DISTORTION;
    if (cat == "Clipping")                           return CLAP_PLUGIN_FEATURE_LIMITER;
    if (cat == "Dynamics")                           return CLAP_PLUGIN_FEATURE_COMPRESSOR;
    if (cat == "Filter" || cat == "XYZ Filters")     return CLAP_PLUGIN_FEATURE_FILTER;
    if (cat == "Biquads")                            return CLAP_PLUGIN_FEATURE_FILTER;
    if (cat == "Bass" || cat == "Brightness")        return CLAP_PLUGIN_FEATURE_EQUALIZER;
    if (cat == "Tone Color")                         return CLAP_PLUGIN_FEATURE_EQUALIZER;
    if (cat == "Consoles")                           return CLAP_PLUGIN_FEATURE_MIXING;
    if (cat == "Dithers")                            return CLAP_PLUGIN_FEATURE_MASTERING;
    if (cat == "Effects")                            return CLAP_PLUGIN_FEATURE_MULTI_EFFECTS;
    if (cat == "Utility" || cat == "Subtlety")       return CLAP_PLUGIN_FEATURE_UTILITY;
    if (cat == "Noise")                              return CLAP_PLUGIN_FEATURE_UTILITY;
    if (cat == "Stereo")                             return CLAP_PLUGIN_FEATURE_STEREO;
    return nullptr;
}

static void build_entries() {
    const auto &reg = AirwinRegistry::registry;
    ENTRIES = new std::vector<Entry>();
    BY_ID = new std::unordered_map<std::string, int>();
    ENTRIES->resize(reg.size());

    for (size_t i = 0; i < reg.size(); i++) {
        Entry &e = (*ENTRIES)[i];
        e.id = id_for(reg[i].name);
        e.name = reg[i].name;
        e.what = reg[i].whatText;
        e.catfeat = "airwindows:" + reg[i].category;
        (*BY_ID)[e.id] = (int)i;
    }

    for (size_t i = 0; i < reg.size(); i++) {
        Entry &e = (*ENTRIES)[i];
        e.features.push_back(CLAP_PLUGIN_FEATURE_AUDIO_EFFECT);
        e.features.push_back(CLAP_PLUGIN_FEATURE_STEREO);
        if (const char *kw = clap_keyword(reg[i].category))
            if (strcmp(kw, CLAP_PLUGIN_FEATURE_STEREO) != 0) e.features.push_back(kw);
        e.features.push_back(e.catfeat.c_str());
        e.features.push_back(nullptr);

        memset(&e.desc, 0, sizeof e.desc);
        e.desc.clap_version = CLAP_VERSION_INIT;
        e.desc.id = e.id.c_str();
        e.desc.name = e.name.c_str();
        e.desc.vendor = "Airwindows";
        e.desc.url = "https://www.airwindows.com";
        e.desc.manual_url = "";
        e.desc.support_url = "https://github.com/baconpaul/airwin2rack";
        e.desc.version = AIRWINDOWS_VERSION;
        e.desc.description = e.what.c_str();
        e.desc.features = e.features.data();
    }
}

/* ---------------------------------------------------------------- *
 * 2. Instance state
 *
 * `values` is the host-facing truth and `fx` mirrors it, because activate()
 * throws the effect away and builds a new one -- the only way to clear its
 * state, the base class having neither reset() nor suspend().
 * ---------------------------------------------------------------- */

struct Inst {
    clap_plugin_t plugin;
    int index = 0;
    int nparams = 0;

    std::unique_ptr<AirwinConsolidatedBase> fx;
    std::unique_ptr<AirwinConsolidatedBase> textfx;

    std::vector<float> values;
    std::vector<float> defaults;
    std::vector<std::string> pnames;

    /* Input is copied here before every processReplacing, so the two input
     * channels are distinct memory and distinct from the outputs whatever the
     * host handed over. Both halves matter: the effects are hard-wired
     * 2-in/2-out and dereference inputs[1] unconditionally, so a mono source
     * still needs two pointers; and several (BitShiftPan most plainly) write
     * outputs[0] before reading inputs[1], so letting those be one buffer
     * would have the second channel read back the first channel's output. */
    std::vector<float> inbuf[2];
    std::vector<float> pad[2];
    uint32_t cap = 0;
};

static Inst *inst(const clap_plugin_t *p) { return (Inst *)p->plugin_data; }

static void read_defaults(Inst *s) {
    char buf[AW_TEXT_BUF];
    s->values.assign((size_t)s->nparams, 0.0f);
    s->defaults.assign((size_t)s->nparams, 0.0f);
    s->pnames.assign((size_t)s->nparams, std::string());
    for (int i = 0; i < s->nparams; i++) {
        memset(buf, 0, sizeof buf);
        s->fx->getParameterName(i, buf);
        s->pnames[i] = buf;
        s->defaults[i] = s->fx->getParameter(i);
        s->values[i] = s->defaults[i];
    }
}

/* ---------------------------------------------------------------- *
 * 3. clap.audio-ports
 *
 * Always one stereo port each way, for every effect, including the 369 the
 * registry marks isMono: "mono" there means the effect gives the same answer
 * on both channels, not that it can be run with one buffer.
 * ---------------------------------------------------------------- */

static uint32_t ports_count(const clap_plugin_t *p, bool is_input) {
    (void)p; (void)is_input;
    return 1;
}

static bool ports_get(const clap_plugin_t *p, uint32_t index, bool is_input,
                      clap_audio_port_info_t *info) {
    (void)p;
    if (index != 0) return false;
    memset(info, 0, sizeof *info);
    info->id = 0;
    snprintf(info->name, sizeof info->name, "%s", is_input ? "In" : "Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t PORTS_EXT = { ports_count, ports_get };

/* ---------------------------------------------------------------- *
 * 4. clap.params
 *
 * Every Airwindows parameter is a bare float in 0..1 with no units, no
 * stepping and no clamping in setParameter, so the CLAP range is 0..1 for all
 * of them and the interesting work is in the text conversions. 47 of the 504
 * effects have no parameters at all, so every entry point here is written to
 * survive a count of zero rather than assume a host will not ask.
 * ---------------------------------------------------------------- */

static uint32_t params_count(const clap_plugin_t *p) { return (uint32_t)inst(p)->nparams; }

static bool params_get_info(const clap_plugin_t *p, uint32_t index, clap_param_info_t *info) {
    Inst *s = inst(p);
    if (index >= (uint32_t)s->nparams) return false;
    memset(info, 0, sizeof *info);
    info->id = index;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    info->cookie = nullptr;
    snprintf(info->name, sizeof info->name, "%s", s->pnames[index].c_str());
    info->module[0] = '\0';
    info->min_value = 0.0;
    info->max_value = 1.0;
    info->default_value = s->defaults[index];
    return true;
}

static bool params_get_value(const clap_plugin_t *p, clap_id id, double *out) {
    Inst *s = inst(p);
    if (id >= (clap_id)s->nparams) return false;
    *out = s->values[id];
    return true;
}

/* getParameterDisplay renders whatever the effect currently holds, so
 * rendering an arbitrary value means setting it first -- on a second instance
 * built on demand, because this call is main-thread, process() is not, and
 * poking the live effect to format a string would be heard. */
static bool params_value_to_text(const clap_plugin_t *p, clap_id id, double v,
                                 char *buf, uint32_t cap) {
    Inst *s = inst(p);
    if (id >= (clap_id)s->nparams || cap == 0) return false;
    if (!s->textfx) {
        s->textfx = AirwinRegistry::registry[s->index].generator();
        if (!s->textfx) return false;
        s->textfx->setSampleRate(AirwinConsolidatedBase::defaultSampleRate);
    }
    char raw[AW_TEXT_BUF], label[AW_TEXT_BUF];
    memset(raw, 0, sizeof raw);
    memset(label, 0, sizeof label);
    s->textfx->setParameter((VstInt32)id, (float)v);
    s->textfx->getParameterDisplay((VstInt32)id, raw);
    s->textfx->getParameterLabel((VstInt32)id, label);

    const char *r = raw;
    while (*r == ' ') r++;                 /* float2string pads to width 8 */
    size_t n = strlen(label);
    while (n > 0 && label[n - 1] == ' ') label[--n] = '\0';
    if (n > 0) snprintf(buf, cap, "%s %s", r, label);
    else snprintf(buf, cap, "%s", r);
    return true;
}

static bool params_text_to_value(const clap_plugin_t *p, clap_id id,
                                 const char *txt, double *out) {
    Inst *s = inst(p);
    if (id >= (clap_id)s->nparams || !txt || !s->fx) return false;
    if (!s->fx->canConvertParameterTextToValue((VstInt32)id)) return false;
    float v = 0.0f;
    if (!s->fx->parameterTextToValue((VstInt32)id, txt, v)) return false;
    *out = v;
    return true;
}

static void set_param(Inst *s, clap_id id, double v) {
    if (id >= (clap_id)s->nparams) return;
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    s->values[id] = (float)v;
    if (s->fx) s->fx->setParameter((VstInt32)id, (float)v);
}

static void apply_events(Inst *s, const clap_input_events_t *in) {
    if (!in) return;
    uint32_t n = in->size(in);
    for (uint32_t i = 0; i < n; i++) {
        const clap_event_header_t *h = in->get(in, i);
        if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
        if (h->type != CLAP_EVENT_PARAM_VALUE) continue;
        const clap_event_param_value_t *e = (const clap_event_param_value_t *)h;
        set_param(s, e->param_id, e->value);
    }
}

static void params_flush(const clap_plugin_t *p, const clap_input_events_t *in,
                         const clap_output_events_t *out) {
    (void)out;
    apply_events(inst(p), in);
}

static const clap_plugin_params_t PARAMS_EXT = {
    params_count, params_get_info, params_get_value,
    params_value_to_text, params_text_to_value, params_flush,
};

/* ---------------------------------------------------------------- *
 * 5. clap.state
 *
 * The parameter vector and nothing else: Airwindows effects carry no presets,
 * no programs and no setting that is not a parameter. Written little-endian
 * byte by byte rather than as a struct, because a blob saved on one machine
 * is routinely reloaded on another.
 * ---------------------------------------------------------------- */

#define AW_STATE_MAGIC 0x58465741u        /* 'A','W','F','X' little-endian */
#define AW_STATE_VERSION 1u

/* Ours is 12 bytes plus four per parameter, and the widest effect has 13, so
 * a stream longer than this is not a patch of ours -- and reading it to the
 * end to find out is how a corrupt file becomes an allocation failure. */
#define AW_STATE_MAX 4096

static void put_u32(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)(v); b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static bool state_save(const clap_plugin_t *p, const clap_ostream_t *os) {
    Inst *s = inst(p);
    std::vector<uint8_t> b(12 + 4 * (size_t)s->nparams);
    put_u32(&b[0], AW_STATE_MAGIC);
    put_u32(&b[4], AW_STATE_VERSION);
    put_u32(&b[8], (uint32_t)s->nparams);
    for (int i = 0; i < s->nparams; i++) {
        uint32_t bits;
        memcpy(&bits, &s->values[i], 4);
        put_u32(&b[12 + 4 * (size_t)i], bits);
    }
    size_t off = 0;
    while (off < b.size()) {
        int64_t w = os->write(os, b.data() + off, b.size() - off);
        if (w <= 0) return false;
        off += (size_t)w;
    }
    return true;
}

static bool state_load(const clap_plugin_t *p, const clap_istream_t *is) {
    Inst *s = inst(p);
    std::vector<uint8_t> b;
    uint8_t chunk[512];
    for (;;) {
        int64_t r = is->read(is, chunk, sizeof chunk);
        if (r < 0) return false;
        if (r == 0) break;
        b.insert(b.end(), chunk, chunk + r);
        if (b.size() > AW_STATE_MAX) return false;
    }
    if (b.size() < 12) return false;
    if (get_u32(&b[0]) != AW_STATE_MAGIC) return false;
    if (get_u32(&b[4]) != AW_STATE_VERSION) return false;

    /* A blob from a build whose effect had a different parameter count is
     * honoured as far as it goes: upstream does add knobs, and dropping a
     * whole patch over one of them loses more than it protects. */
    uint32_t n = get_u32(&b[8]);
    if (b.size() < 12 + 4 * (size_t)n) return false;
    if (n > (uint32_t)s->nparams) n = (uint32_t)s->nparams;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t bits = get_u32(&b[12 + 4 * (size_t)i]);
        float v;
        memcpy(&v, &bits, 4);
        set_param(s, i, v);
    }
    return true;
}

static const clap_plugin_state_t STATE_EXT = { state_save, state_load };

/* ---------------------------------------------------------------- *
 * 6. Lifecycle
 * ---------------------------------------------------------------- */

static bool plug_init(const clap_plugin_t *p) { (void)p; return true; }

static void plug_destroy(const clap_plugin_t *p) { delete inst(p); }

/* Rebuilding the effect is the only way to clear its state, and it allocates,
 * so it belongs here and never in process(). defaultSampleRate is a
 * process-global static read by the constructor: it has to be set before
 * generator() runs, not only afterwards. */
static bool plug_activate(const clap_plugin_t *p, double sr, uint32_t minf, uint32_t maxf) {
    (void)minf;
    Inst *s = inst(p);
    if (maxf < 1) return false;
    if (!(sr > 2000.0)) return false;      /* getSampleRate() asserts this */

    AirwinConsolidatedBase::defaultSampleRate = (float)sr;
    auto fresh = AirwinRegistry::registry[s->index].generator();
    if (!fresh) return false;
    fresh->setSampleRate((float)sr);
    for (int i = 0; i < s->nparams; i++) fresh->setParameter(i, s->values[i]);
    s->fx = std::move(fresh);

    for (int c = 0; c < 2; c++) {
        s->inbuf[c].assign(maxf, 0.0f);
        s->pad[c].assign(maxf, 0.0f);
    }
    s->cap = maxf;
    return true;
}

static void plug_deactivate(const clap_plugin_t *p) { (void)p; }
static bool plug_start(const clap_plugin_t *p) { (void)p; return true; }
static void plug_stop(const clap_plugin_t *p) { (void)p; }

static void plug_reset(const clap_plugin_t *p) {
    Inst *s = inst(p);
    if (s->cap == 0) return;
    AirwinConsolidatedBase::defaultSampleRate = s->fx ? s->fx->sampleRate : 48000.0f;
    auto fresh = AirwinRegistry::registry[s->index].generator();
    if (!fresh) return;
    fresh->setSampleRate(AirwinConsolidatedBase::defaultSampleRate);
    for (int i = 0; i < s->nparams; i++) fresh->setParameter(i, s->values[i]);
    s->fx = std::move(fresh);
}

/* Allocation-free: every effect keeps its state in fixed-size members, the
 * scratch was sized in activate, and the chunk loop makes an over-long block
 * a slow call rather than an overrun. */
static clap_process_status plug_process(const clap_plugin_t *p, const clap_process_t *pr) {
    Inst *s = inst(p);
    if (!pr) return CLAP_PROCESS_ERROR;
    apply_events(s, pr->in_events);
    if (!s->fx || s->cap == 0) return CLAP_PROCESS_ERROR;
    if (pr->audio_inputs_count < 1 || pr->audio_outputs_count < 1) return CLAP_PROCESS_ERROR;

    const clap_audio_buffer_t *ib = &pr->audio_inputs[0];
    clap_audio_buffer_t *ob = &pr->audio_outputs[0];
    if (!ib->data32 || !ob->data32) return CLAP_PROCESS_ERROR;

    const float *src[2] = { nullptr, nullptr };
    src[0] = (ib->channel_count > 0) ? ib->data32[0] : nullptr;
    src[1] = (ib->channel_count > 1) ? ib->data32[1] : src[0];
    float *dst[2] = { nullptr, nullptr };
    dst[0] = (ob->channel_count > 0) ? ob->data32[0] : nullptr;
    dst[1] = (ob->channel_count > 1) ? ob->data32[1] : nullptr;

    for (uint32_t off = 0; off < pr->frames_count; off += s->cap) {
        uint32_t k = pr->frames_count - off;
        if (k > s->cap) k = s->cap;
        for (int c = 0; c < 2; c++) {
            if (src[c]) memcpy(s->inbuf[c].data(), src[c] + off, (size_t)k * sizeof(float));
            else memset(s->inbuf[c].data(), 0, (size_t)k * sizeof(float));
        }
        float *ins[2] = { s->inbuf[0].data(), s->inbuf[1].data() };
        float *outs[2] = {
            dst[0] ? dst[0] + off : s->pad[0].data(),
            dst[1] ? dst[1] + off : s->pad[1].data(),
        };
        s->fx->processReplacing(ins, outs, (VstInt32)k);
    }
    ob->constant_mask = 0;
    return CLAP_PROCESS_CONTINUE;
}

static const void *plug_get_extension(const clap_plugin_t *p, const char *id) {
    (void)p;
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &PORTS_EXT;
    if (strcmp(id, CLAP_EXT_PARAMS) == 0) return &PARAMS_EXT;
    if (strcmp(id, CLAP_EXT_STATE) == 0) return &STATE_EXT;
    return nullptr;
}

static void plug_on_main_thread(const clap_plugin_t *p) { (void)p; }

/* The effect is built here as well as in activate, so that a host may read
 * parameter names, defaults and text before it ever activates anything. */
static const clap_plugin_t *make(int index) {
    if (AirwinConsolidatedBase::defaultSampleRate <= 2000.0f)
        AirwinConsolidatedBase::defaultSampleRate = 48000.0f;

    Inst *s = new Inst();
    s->index = index;
    s->nparams = AirwinRegistry::registry[index].nParams;
    s->fx = AirwinRegistry::registry[index].generator();
    if (!s->fx) { delete s; return nullptr; }
    s->fx->setSampleRate(AirwinConsolidatedBase::defaultSampleRate);
    read_defaults(s);

    s->plugin.desc = &(*ENTRIES)[index].desc;
    s->plugin.plugin_data = s;
    s->plugin.init = plug_init;
    s->plugin.destroy = plug_destroy;
    s->plugin.activate = plug_activate;
    s->plugin.deactivate = plug_deactivate;
    s->plugin.start_processing = plug_start;
    s->plugin.stop_processing = plug_stop;
    s->plugin.reset = plug_reset;
    s->plugin.process = plug_process;
    s->plugin.get_extension = plug_get_extension;
    s->plugin.on_main_thread = plug_on_main_thread;
    return &s->plugin;
}

/* ---------------------------------------------------------------- *
 * 7. Factory and entry
 * ---------------------------------------------------------------- */

static uint32_t factory_count(const clap_plugin_factory_t *f) {
    (void)f;
    return ENTRIES ? (uint32_t)ENTRIES->size() : 0u;
}

static const clap_plugin_descriptor_t *factory_desc(const clap_plugin_factory_t *f,
                                                    uint32_t index) {
    (void)f;
    if (!ENTRIES || index >= ENTRIES->size()) return nullptr;
    return &(*ENTRIES)[index].desc;
}

static const clap_plugin_t *factory_create(const clap_plugin_factory_t *f,
                                           const clap_host_t *host, const char *id) {
    (void)f;
    if (!ENTRIES || !BY_ID || !id) return nullptr;
    if (host && !clap_version_is_compatible(host->clap_version)) return nullptr;
    auto it = BY_ID->find(id);
    if (it == BY_ID->end()) return nullptr;
    return make(it->second);
}

static const clap_plugin_factory_t FACTORY = {
    factory_count, factory_desc, factory_create,
};

/* Refcounted: a second loader in the same process must not rebuild the
 * tables under the first, nor tear them down out from under it. */
static int ENTRY_REFS = 0;

static bool entry_init(const char *path) {
    (void)path;
    if (ENTRY_REFS++ > 0) return true;
    if (!REGISTRY_READY) {
        AirwinRegistry::completeRegistry();
        REGISTRY_READY = true;
    }
    build_entries();
    return ENTRIES && !ENTRIES->empty();
}

static void entry_deinit(void) {
    if (ENTRY_REFS > 0 && --ENTRY_REFS > 0) return;
    delete ENTRIES; ENTRIES = nullptr;
    delete BY_ID;   BY_ID = nullptr;
}

static const void *entry_get_factory(const char *id) {
    return (strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0) ? &FACTORY : nullptr;
}

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory,
};
