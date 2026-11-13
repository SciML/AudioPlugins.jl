/* vst3_host.cpp -- see vst3_host.h for what this is and why it is shaped
 * this way. Headless VST3 host: load a bundle through the SDK's module
 * loader, instantiate one audio-effect class, activate it at a fixed
 * block size, and run it once per tick from a clocked equation.
 *
 * C++ inside, C outside. Everything the SDK needs from a host is here:
 * an IHostApplication (the SDK's example HostApplication, which also
 * serves IMessage / IAttributeList), and an IComponentHandler that counts
 * the plugin's edit requests and honours restartComponent(kLatencyChanged)
 * by re-reading the latency. No editor is ever created.
 *
 * Allocation-free after open, like the other hosts: the block buffers and
 * the parameter-change queues are sized once at open. The SDK allocates
 * during scan and open, which is driver-side.
 */

#define VST3_HOST_BUILDING 1
#include "vst3_host.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

#include <cmath>
#ifndef M_PI                     /* MinGW's <cmath> hides it without _USE_MATH_DEFINES */
#define M_PI 3.14159265358979323846
#endif
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <algorithm>
#include <string>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

/* ---------------------------------------------------------------- *
 * 1. Error reporting
 * ---------------------------------------------------------------- */

char ERR[1024];

void set_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ERR, sizeof ERR, fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------- *
 * 2. The host side we present to the plugin
 * ---------------------------------------------------------------- */

struct State;

/* IComponentHandler: the plugin telling the host about edits it initiates
 * (a meter, a GUI it does not have) and asking to be restarted. A
 * synchronous model has no scheduler to ask, so edits are counted and
 * restart flags are honoured where a headless host can: latency is
 * re-read on kLatencyChanged; everything else is recorded. */
class ComponentHandler : public IComponentHandler {
public:
    State *owner = nullptr;
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, IComponentHandler)
        QUERY_INTERFACE(iid, obj, IComponentHandler::iid, IComponentHandler)
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1000; }   /* owned by the instance */
    uint32 PLUGIN_API release() override { return 1000; }

    tresult PLUGIN_API beginEdit(ParamID) override;
    tresult PLUGIN_API performEdit(ParamID, ParamValue) override { return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32 flags) override;
};

HostApplication HOST_APP;       /* IHostApplication + IMessage/IAttributeList factory */

/* ---------------------------------------------------------------- *
 * 3. State
 * ---------------------------------------------------------------- */

struct ParamInfo {
    ParamID     id;
    double      min, max, def, def_norm;
    double      steps;
    bool        readonly, bypass;
    std::string name, units;
};

struct State {
    State *next = nullptr;
    double handle = 0;
    long open_index = -1;
    long n_restart_reqs = 0, n_plugin_edits = 0;
    ComponentHandler handler;
    double pending[VST3_HOST_MAX_PARAMS] = {};
    bool queued[VST3_HOST_MAX_PARAMS] = {};
    bool in_armed = false;
    VST3::Hosting::Module::Ptr        module;
    std::unique_ptr<PlugProvider>     provider;
    IPtr<IComponent>                  component;
    IPtr<IEditController>             controller;
    IPtr<IAudioProcessor>             processor;

    bool   open = false;
    bool   processing = false;
    double sample_rate = 0;
    long   block = 0, chan = 0;

    /* Descriptor cache from the last scan: audio-effect classes only. */
    std::vector<VST3::Hosting::ClassInfo> classes;
    std::string plugin_name, plugin_id, path;

    std::vector<ParamInfo> params;
    double latency = 0;

    /* Per-block machinery, sized at open. */
    HostProcessData   pd;
    ParameterChanges  in_changes;
    ParameterChanges  out_changes;
    EventList         in_events;
    EventList         out_events;
    ProcessContext    ctx;

    double sent[VST3_HOST_MAX_PARAMS];

    float  in[VST3_HOST_MAX_CHAN][VST3_HOST_MAX_BLOCK];
    float  out[VST3_HOST_MAX_CHAN][VST3_HOST_MAX_BLOCK];

    double in_token = 0, out_token = 0;
    long   in_n = 0, out_n = 0;
    long   n_process = 0;
    int64  steady = 0;
};

State DEFAULT_STATE;
State *INSTANCES;
double NEXT_HANDLE, NEXT_TOKEN;

tresult ComponentHandler::beginEdit(ParamID) {
    owner->n_plugin_edits++;
    return kResultOk;
}
tresult ComponentHandler::restartComponent(int32 flags) {
    owner->n_restart_reqs++;
    if ((flags & kLatencyChanged) && owner->processor && owner->open)
        owner->latency = (double)owner->processor->getLatencySamples();
    return kResultOk;
}

static State *instance(double handle) {
    for (State *st = INSTANCES; st; st = st->next)
        if (st->handle == handle) return st;
    set_err("invalid or closed VST3 instance %g", handle);
    return NULL;
}
static double next_token(State *st, double previous) {
    if (!st->handle) return previous + 1;
    if (NEXT_TOKEN >= 9007199254740991.0) return NAN;
    return -(++NEXT_TOKEN);
}
static int token_matches(State *st, double dep, double token) {
    if (!std::isfinite(dep) || token == 0) return 0;
    return st->handle ? dep == token : (dep >= 0 && dep < 9007199254740991.0 && std::floor(dep + 0.5) == token);
}
static const ParamInfo *find_param_ctx(State *st, double id) {
    if (!std::isfinite(id) || id < 0 || id > UINT32_MAX || std::floor(id) != id) return nullptr;
    ParamID pid = (ParamID)(uint32)id;
    for (const auto &p : st->params) if (p.id == pid) return &p;
    return nullptr;
}

std::string utf8(const String128 s) { return Steinberg::Vst::StringConvert::convert(s); }

/* ---------------------------------------------------------------- *
 * 4. Discovery
 * ---------------------------------------------------------------- */

static void unload_ctx(State *st) {
    st->provider.reset();      /* terminates component + controller */
    st->processor = nullptr;
    st->controller = nullptr;
    st->component = nullptr;
    st->module.reset();
}

} // namespace

extern "C" {

static double vst3_out_valid_ctx(State *st, double dep);
static double vst3_out_count_ctx(State *st, double dep);
static double vst3_out_peak_ctx(State *st, double dep);
static double vst3_out_rms_ctx(State *st, double dep);
static double vst3_out_sample_ctx(State *st, double dep, double i, double ch);
static double vst3_process_ctx(State *st, double dep,
                    double id0, double v0, double id1, double v1,
                    double id2, double v2, double id3, double v3);
static double vst3_in_sample_ctx(State *st, double dep, double i, double ch);
static double vst3_in_tone_ctx(State *st, double t, double waveform, double freq, double amp);
static double vst3_in_fill_ctx(State *st, const double *samples, long n, long channels);
static void vst3_host_reset_counters_ctx(State *st);
static long   vst3_host_n_plugin_edits_ctx(State *st);
static long   vst3_host_n_restart_requests_ctx(State *st);
static long   vst3_host_n_process_ctx(State *st);
static double vst3_host_is_open_ctx(State *st);
static double vst3_host_channels_ctx(State *st);
static double vst3_host_block_size_ctx(State *st);
static double vst3_host_sample_rate_ctx(State *st);
static double vst3_host_latency_ctx(State *st);
static double vst3_host_param_value_ctx(State *st, double param_id);
static double vst3_host_param_normalized_ctx(State *st, double param_id, double plain);
static double vst3_host_param_plain_ctx(State *st, double param_id, double normalized);
static const char *vst3_host_param_units_ctx(State *st, long i);
static const char *vst3_host_param_name_ctx(State *st, long i);
static double vst3_host_param_is_bypass_ctx(State *st, long i);
static double vst3_host_param_is_readonly_ctx(State *st, long i);
static double vst3_host_param_steps_ctx(State *st, long i);
static double vst3_host_param_default_normalized_ctx(State *st, long i);
static double vst3_host_param_default_ctx(State *st, long i);
static double vst3_host_param_max_ctx(State *st, long i);
static double vst3_host_param_min_ctx(State *st, long i);
static double vst3_host_param_id_ctx(State *st, long i);
static long vst3_host_n_params_ctx(State *st);
static const char *vst3_host_plugin_id_ctx(State *st);
static const char *vst3_host_plugin_name_ctx(State *st);
static void vst3_host_close_ctx(State *st);
static int vst3_host_open_ctx(State *st, const char *path, const char *class_id,
                   double sample_rate, double block_size, double channels);
static const char *vst3_host_scan_category_ctx(State *st, long i);
static const char *vst3_host_scan_name_ctx(State *st, long i);
static const char *vst3_host_scan_id_ctx(State *st, long i);
static long vst3_host_scan_ctx(State *st, const char *path);


const char *vst3_host_last_error(void) { return ERR; }

static long vst3_host_scan_ctx(State *st, const char *path) {
    ERR[0] = '\0';
    vst3_host_close_ctx(st);
    if (!path || !path[0]) { set_err("no plugin path given"); return -1; }

    std::string err;
    st->module = VST3::Hosting::Module::create(path, err);
    if (!st->module) {
        set_err("cannot load VST3 bundle %s: %s", path,
                err.empty() ? "not a VST3 bundle or shared object" : err.c_str());
        return -1;
    }
    st->path = path;
    st->classes.clear();
    for (const auto &ci : st->module->getFactory().classInfos())
        if (ci.category() == kVstAudioEffectClass) st->classes.push_back(ci);
    if (st->classes.empty()) {
        set_err("%s loads but exports no audio-effect class (%d classes total)", path,
                (int)st->module->getFactory().classInfos().size());
        unload_ctx(st);
        return -1;
    }
    return (long)st->classes.size();
}

static std::string SCAN_TMP[3];
static const char *vst3_host_scan_id_ctx(State *st, long i) {
    if (i < 0 || i >= (long)st->classes.size()) return "";
    SCAN_TMP[0] = st->classes[(size_t)i].ID().toString();
    return SCAN_TMP[0].c_str();
}
static const char *vst3_host_scan_name_ctx(State *st, long i) {
    if (i < 0 || i >= (long)st->classes.size()) return "";
    SCAN_TMP[1] = st->classes[(size_t)i].name();
    return SCAN_TMP[1].c_str();
}
static const char *vst3_host_scan_category_ctx(State *st, long i) {
    if (i < 0 || i >= (long)st->classes.size()) return "";
    SCAN_TMP[2] = st->classes[(size_t)i].subCategoriesString();
    return SCAN_TMP[2].c_str();
}

/* ---------------------------------------------------------------- *
 * 5. Open / close
 * ---------------------------------------------------------------- */

static void fail_open_ctx(State *st) { vst3_host_close_ctx(st); }

static void read_params_ctx(State *st) {
    st->params.clear();
    if (!st->controller) return;
    int32 n = st->controller->getParameterCount();
    for (int32 i = 0; i < n && (long)st->params.size() < VST3_HOST_MAX_PARAMS; i++) {
        ParameterInfo info;
        memset(&info, 0, sizeof info);
        if (st->controller->getParameterInfo(i, info) != kResultOk) continue;
        ParamInfo p;
        p.id       = info.id;
        p.def_norm = info.defaultNormalizedValue;
        p.min      = st->controller->normalizedParamToPlain(info.id, 0.0);
        p.max      = st->controller->normalizedParamToPlain(info.id, 1.0);
        p.def      = st->controller->normalizedParamToPlain(info.id, info.defaultNormalizedValue);
        p.steps    = (double)info.stepCount;
        p.readonly = (info.flags & ParameterInfo::kIsReadOnly) != 0;
        p.bypass   = (info.flags & ParameterInfo::kIsBypass) != 0;
        p.name     = utf8(info.title);
        p.units    = utf8(info.units);
        st->params.push_back(p);
    }
}

static int vst3_host_open_ctx(State *st, const char *path, const char *class_id,
                   double sample_rate, double block_size, double channels) {
    long n = vst3_host_scan_ctx(st, path);      /* also clears state and sets ERR */
    if (n < 0) return 1;

    if (!std::isfinite(block_size) || !std::isfinite(channels) || !std::isfinite(sample_rate)) return 1;
    long blk = (long)(block_size + 0.5);
    long ch  = (long)(channels + 0.5);
    if (blk < 1 || blk > VST3_HOST_MAX_BLOCK) {
        set_err("block_size %ld out of range 1..%d", blk, VST3_HOST_MAX_BLOCK);
        fail_open_ctx(st);
        return 1;
    }
    if (ch < 1 || ch > VST3_HOST_MAX_CHAN) {
        set_err("channels %ld out of range 1..%d", ch, VST3_HOST_MAX_CHAN);
        fail_open_ctx(st);
        return 1;
    }
    if (!(sample_rate > 0.0)) {
        set_err("sample_rate must be positive, got %g", sample_rate);
        fail_open_ctx(st);
        return 1;
    }

    /* Pick the class. */
    const VST3::Hosting::ClassInfo *ci = nullptr;
    if (class_id && class_id[0]) {
        for (const auto &c : st->classes)
            if (c.ID().toString() == class_id) { ci = &c; break; }
        if (!ci) {
            set_err("no audio-effect class with id '%s' in %s (it has %ld: first is '%s' %s)",
                    class_id, path, (long)st->classes.size(),
                    st->classes[0].ID().toString().c_str(), st->classes[0].name().c_str());
            fail_open_ctx(st);
            return 1;
        }
    } else {
        ci = &st->classes[0];
    }
    st->open_index = (long)(ci - st->classes.data());
    st->plugin_name = ci->name();
    st->plugin_id   = ci->ID().toString();

    /* Instantiate component and controller, connect them (PlugProvider
     * does the IConnectionPoint dance and the separate-controller case),
     * with our IHostApplication as the context. */
    PluginContextFactory::instance().setPluginContext(&HOST_APP);
    st->provider.reset(new PlugProvider(st->module->getFactory(), *ci, true));
    if (!st->provider->initialize()) {
        set_err("class '%s' could not be instantiated or initialised", st->plugin_name.c_str());
        fail_open_ctx(st);
        return 1;
    }
    st->component  = st->provider->getComponentPtr();
    st->controller = st->provider->getControllerPtr();
    if (!st->component) {
        set_err("class '%s' has no IComponent", st->plugin_name.c_str());
        fail_open_ctx(st);
        return 1;
    }
    st->processor = FUnknownPtr<IAudioProcessor>(st->component);
    if (!st->processor) {
        set_err("class '%s' does not implement IAudioProcessor", st->plugin_name.c_str());
        fail_open_ctx(st);
        return 1;
    }
    st->handler.owner = st;
    if (st->controller) st->controller->setComponentHandler(&st->handler);

    /* Bus arrangement: mono or stereo on the main input and output. A
     * plugin without a main input (an instrument) is still fine; one
     * without an audio output is not. */
    int32 n_in  = st->component->getBusCount(kAudio, kInput);
    int32 n_out = st->component->getBusCount(kAudio, kOutput);
    if (n_out < 1) {
        set_err("class '%s' has no audio output bus", st->plugin_name.c_str());
        fail_open_ctx(st);
        return 1;
    }
    SpeakerArrangement arr = (ch == 2) ? SpeakerArr::kStereo : SpeakerArr::kMono;
    std::vector<SpeakerArrangement> ins((size_t)n_in, arr), outs((size_t)n_out, arr);
    /* Only the main buses carry our channels; ask for the same layout on
     * the others but deactivate them below. */
    tresult r = st->processor->setBusArrangements(ins.empty() ? nullptr : ins.data(), n_in,
                                                outs.data(), n_out);
    if (r != kResultOk) {
        set_err("class '%s' refused a %s arrangement on its main buses (%d in, %d out)",
                st->plugin_name.c_str(), ch == 2 ? "stereo" : "mono", n_in, n_out);
        fail_open_ctx(st);
        return 1;
    }
    /* Verify what the plugin actually settled on for the main buses. */
    SpeakerArrangement got_out = 0;
    st->processor->getBusArrangement(kOutput, 0, got_out);
    if (SpeakerArr::getChannelCount(got_out) != (int32)ch) {
        set_err("class '%s' set its main output to %d channel(s), not the %ld asked for",
                st->plugin_name.c_str(), SpeakerArr::getChannelCount(got_out), ch);
        fail_open_ctx(st);
        return 1;
    }
    if (n_in > 0) {
        SpeakerArrangement got_in = 0;
        st->processor->getBusArrangement(kInput, 0, got_in);
        if (SpeakerArr::getChannelCount(got_in) != (int32)ch) {
            set_err("class '%s' set its main input to %d channel(s), not the %ld asked for",
                    st->plugin_name.c_str(), SpeakerArr::getChannelCount(got_in), ch);
            fail_open_ctx(st);
            return 1;
        }
    }
    for (int32 i = 0; i < n_in;  i++) st->component->activateBus(kAudio, kInput,  i, i == 0);
    for (int32 i = 0; i < n_out; i++) st->component->activateBus(kAudio, kOutput, i, i == 0);
    for (int32 i = 0; i < st->component->getBusCount(kEvent, kInput);  i++)
        st->component->activateBus(kEvent, kInput, i, false);
    for (int32 i = 0; i < st->component->getBusCount(kEvent, kOutput); i++)
        st->component->activateBus(kEvent, kOutput, i, false);

    if (st->processor->canProcessSampleSize(kSample32) != kResultOk) {
        set_err("class '%s' cannot process 32-bit float", st->plugin_name.c_str());
        fail_open_ctx(st);
        return 1;
    }

    ProcessSetup setup;
    setup.processMode        = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = (int32)blk;
    setup.sampleRate         = sample_rate;
    if (st->processor->setupProcessing(setup) != kResultOk) {
        set_err("class '%s' refused setupProcessing(%g Hz, %ld frames, float32)",
                st->plugin_name.c_str(), sample_rate, blk);
        fail_open_ctx(st);
        return 1;
    }
    if (st->component->setActive(true) != kResultOk) {
        set_err("class '%s' refused setActive(true)", st->plugin_name.c_str());
        fail_open_ctx(st);
        return 1;
    }
    st->open = true;      /* from here on, close() must deactivate */

    /* Buffers for every bus, owned by HostProcessData, sized once. The
     * main buses are copied from / to the static block buffers per tick. */
    if (!st->pd.prepare(*st->component, (int32)blk, kSample32)) {
        set_err("could not prepare process buffers for class '%s'", st->plugin_name.c_str());
        fail_open_ctx(st);
        return 1;
    }
    st->in_changes.setMaxParameters(VST3_HOST_MAX_PARAMS);
    st->out_changes.setMaxParameters(VST3_HOST_MAX_PARAMS);
    st->pd.inputParameterChanges  = &st->in_changes;
    st->pd.outputParameterChanges = &st->out_changes;
    st->pd.inputEvents  = &st->in_events;
    st->pd.outputEvents = &st->out_events;
    memset(&st->ctx, 0, sizeof st->ctx);
    st->ctx.sampleRate = sample_rate;
    st->ctx.state = ProcessContext::kPlaying | ProcessContext::kContTimeValid;
    st->pd.processContext = &st->ctx;

    read_params_ctx(st);
    st->latency = (double)st->processor->getLatencySamples();

    if (st->processor->setProcessing(true) != kResultOk) {
        /* Optional per spec: a plugin may return kNotImplemented. */
    }
    st->processing = true;

    st->sample_rate = sample_rate;
    st->block = blk;
    st->chan  = ch;
    for (long i = 0; i < VST3_HOST_MAX_PARAMS; i++) st->sent[i] = NAN;
    memset(st->in, 0, sizeof st->in);
    memset(st->out, 0, sizeof st->out);
    st->in_token = st->out_token = 0;
    st->in_n = st->out_n = 0;
    st->n_process = 0;
    st->steady = 0;
    return 0;
}

static void vst3_host_close_ctx(State *st) {
    if (st->processor && st->processing) st->processor->setProcessing(false);
    st->processing = false;
    if (st->component && st->open) st->component->setActive(false);
    st->pd.unprepare();
    st->pd.inputParameterChanges = nullptr;
    st->pd.outputParameterChanges = nullptr;
    st->pd.inputEvents = nullptr;
    st->pd.outputEvents = nullptr;
    st->pd.processContext = nullptr;
    st->in_changes.clearQueue();
    st->out_changes.clearQueue();
    memset(st->queued, 0, sizeof st->queued);
    st->in_armed = 0;
    st->open_index = -1;
    st->open = false;
    st->params.clear();
    st->latency = 0;
    st->in_token = st->out_token = 0;
    st->in_n = st->out_n = 0;
    st->n_process = 0;
    st->steady = 0;
    st->plugin_name.clear();
    st->plugin_id.clear();
    /* The class cache survives, as in the other hosts. */
    unload_ctx(st);
}

static const char *vst3_host_plugin_name_ctx(State *st) { return st->plugin_name.c_str(); }
static const char *vst3_host_plugin_id_ctx(State *st) { return st->plugin_id.c_str(); }

/* ---------------------------------------------------------------- *
 * 6. Parameter and configuration reporting
 * ---------------------------------------------------------------- */

static long vst3_host_n_params_ctx(State *st) { return (long)st->params.size(); }

static const ParamInfo *pat_ctx(State *st, long i) {
    return (i >= 0 && i < (long)st->params.size()) ? &st->params[(size_t)i] : nullptr;
}

static double vst3_host_param_id_ctx(State *st, long i) { const ParamInfo *p = pat_ctx(st, i); return p ? (double)p->id : -1.0; }
static double vst3_host_param_min_ctx(State *st, long i) { const ParamInfo *p = pat_ctx(st, i); return p ? p->min : NAN; }
static double vst3_host_param_max_ctx(State *st, long i) { const ParamInfo *p = pat_ctx(st, i); return p ? p->max : NAN; }
static double vst3_host_param_default_ctx(State *st, long i) { const ParamInfo *p = pat_ctx(st, i); return p ? p->def : NAN; }
static double vst3_host_param_default_normalized_ctx(State *st, long i) { const ParamInfo *p = pat_ctx(st, i); return p ? p->def_norm : NAN; }
static double vst3_host_param_steps_ctx(State *st, long i) { const ParamInfo *p = pat_ctx(st, i); return p ? p->steps : NAN; }
static double vst3_host_param_is_readonly_ctx(State *st, long i) { const ParamInfo *p = pat_ctx(st, i); return p ? (p->readonly ? 1.0 : 0.0) : NAN; }
static double vst3_host_param_is_bypass_ctx(State *st, long i) { const ParamInfo *p = pat_ctx(st, i); return p ? (p->bypass ? 1.0 : 0.0) : NAN; }
static const char *vst3_host_param_name_ctx(State *st, long i) { const ParamInfo *p = pat_ctx(st, i); return p ? p->name.c_str() : ""; }
static const char *vst3_host_param_units_ctx(State *st, long i) { const ParamInfo *p = pat_ctx(st, i); return p ? p->units.c_str() : ""; }

static double vst3_host_param_plain_ctx(State *st, double param_id, double normalized) {
    const ParamInfo *p = find_param_ctx(st, param_id);
    if (!p || !st->controller) return NAN;
    return st->controller->normalizedParamToPlain(p->id, normalized);
}
static double vst3_host_param_normalized_ctx(State *st, double param_id, double plain) {
    const ParamInfo *p = find_param_ctx(st, param_id);
    if (!p || !st->controller) return NAN;
    return st->controller->plainParamToNormalized(p->id, plain);
}
static double vst3_host_param_value_ctx(State *st, double param_id) {
    const ParamInfo *p = find_param_ctx(st, param_id);
    if (!p || !st->controller || !st->open) return NAN;
    return st->controller->getParamNormalized(p->id);
}

static double vst3_host_latency_ctx(State *st) { return st->open ? st->latency : 0.0; }
static double vst3_host_sample_rate_ctx(State *st) { return st->open ? st->sample_rate : 0.0; }
static double vst3_host_block_size_ctx(State *st) { return st->open ? (double)st->block : 0.0; }
static double vst3_host_channels_ctx(State *st) { return st->open ? (double)st->chan : 0.0; }
static double vst3_host_is_open_ctx(State *st) { return st->open ? 1.0 : 0.0; }
static long   vst3_host_n_process_ctx(State *st) { return st->n_process; }
static long   vst3_host_n_restart_requests_ctx(State *st) { return st->n_restart_reqs; }
static long   vst3_host_n_plugin_edits_ctx(State *st) { return st->n_plugin_edits; }

static void vst3_host_reset_counters_ctx(State *st) {
    st->n_process = 0;
    st->steady = 0;
    st->n_restart_reqs = st->n_plugin_edits = 0;
}

/* ---------------------------------------------------------------- *
 * 7. The input block
 * ---------------------------------------------------------------- */

static double vst3_in_fill_ctx(State *st, const double *samples, long n, long channels) {
    if (!st->open) { set_err("no plugin is open"); return NAN; }
    if (!samples || n < 0) { set_err("vst3_in_fill: no samples"); return NAN; }
    if (n > st->block) n = st->block;
    long ch = (channels < 1) ? 1 : (channels > st->chan ? st->chan : channels);
    for (long i = 0; i < n; i++)
        for (long c = 0; c < st->chan; c++) {
            long src = (c < ch) ? (i * ch + c) : (i * ch);   /* mono -> all */
            st->in[c][i] = (float)samples[src];
        }
    for (long i = n; i < st->block; i++)
        for (long c = 0; c < st->chan; c++) st->in[c][i] = 0.0f;
    memset(st->queued, 0, sizeof st->queued);
    st->in_armed = 1;
    st->in_n = n;
    return (st->in_token = next_token(st, st->in_token));
}

static double wave_at_ctx(State *st, long k, int w, double freq, double amp) {
    double t = (double)k / st->sample_rate;
    switch (w) {
    case VST3_WAVE_SINE:    return amp * sin(2.0 * M_PI * freq * t);
    case VST3_WAVE_SQUARE:  return amp * (sin(2.0 * M_PI * freq * t) >= 0.0 ? 1.0 : -1.0);
    case VST3_WAVE_RAMP:    return amp * (2.0 * fmod(freq * t, 1.0) - 1.0);
    case VST3_WAVE_IMPULSE: return (k == 0) ? amp : 0.0;
    default:                return 0.0;
    }
}

static double vst3_in_tone_ctx(State *st, double t, double waveform, double freq, double amp) {
    if (!st->open) { set_err("no plugin is open"); return NAN; }
    double end = t * st->sample_rate;
    if (!std::isfinite(end) || !std::isfinite(waveform)) return NAN;
    long first = (long)(end + 0.5) - st->block;
    int w = (int)(waveform + 0.5);
    for (long i = 0; i < st->block; i++) {
        double v = wave_at_ctx(st, first + i, w, freq, amp);
        for (long c = 0; c < st->chan; c++) st->in[c][i] = (float)v;
    }
    memset(st->queued, 0, sizeof st->queued);
    st->in_armed = 1;
    st->in_n = st->block;
    return (st->in_token = next_token(st, st->in_token));
}

static double vst3_in_sample_ctx(State *st, double dep, double i, double ch) {
    if (!st->open || !std::isfinite(i) || !std::isfinite(ch)) return NAN;
    if (!token_matches(st, dep, st->in_token)) return NAN;
    long k = (long)(i + 0.5), c = (long)(ch + 0.5);
    if (k < 0 || k >= st->in_n || c < 0 || c >= st->chan) return NAN;
    return (double)st->in[c][k];
}

/* ---------------------------------------------------------------- *
 * 8. process() -- the node-side operator
 * ---------------------------------------------------------------- */

static double vst3_process_ctx(State *st, double dep,
                    double id0, double v0, double id1, double v1,
                    double id2, double v2, double id3, double v3) {
    if (!st->open) { set_err("no plugin is open"); return NAN; }
    if (!std::isfinite(dep) || (st->handle && !st->in_armed) || !token_matches(st, dep, st->in_token) || st->in_token == 0) return NAN;

    /* Only send what changed, through inputParameterChanges at offset 0,
     * mirrored into the controller so the two views agree. */
    const double ids[VST3_HOST_PARAM_SLOTS] = { id0, id1, id2, id3 };
    const double vals[VST3_HOST_PARAM_SLOTS] = { v0, v1, v2, v3 };
    for (int i = 0; i < VST3_HOST_PARAM_SLOTS; i++) {
        const ParamInfo *p = find_param_ctx(st, ids[i]);
        if (!p || p->readonly || std::isnan(vals[i])) continue;
        size_t k = (size_t)(p - st->params.data());
        st->queued[k] = true;
        st->pending[k] = vals[i];
    }
    st->in_changes.clearQueue();
    st->out_changes.clearQueue();
    for (size_t k = 0; k < st->params.size(); k++) {
        if (!st->queued[k]) continue;
        st->queued[k] = false;
        double v = std::max(0.0, std::min(1.0, st->pending[k]));
        if (st->sent[k] == v) continue;
        st->sent[k] = v;
        int32 qi = 0, pi = 0;
        IParamValueQueue *q = st->in_changes.addParameterData(st->params[k].id, qi);
        if (q) q->addPoint(0, v, pi);
        if (st->controller) st->controller->setParamNormalized(st->params[k].id, v);
    }
    st->in_armed = false;

    /* Main buses: copy the block in, run, copy the block out. */
    AudioBusBuffers *ib = (st->pd.numInputs > 0) ? &st->pd.inputs[0] : nullptr;
    AudioBusBuffers *ob = &st->pd.outputs[0];
    if (ib) {
        for (int32 c = 0; c < ib->numChannels && c < (int32)st->chan; c++)
            memcpy(ib->channelBuffers32[c], st->in[c], sizeof(float) * (size_t)st->block);
        ib->silenceFlags = 0;
    }
    for (int32 c = 0; c < ob->numChannels; c++)
        memset(ob->channelBuffers32[c], 0, sizeof(float) * (size_t)st->block);
    st->pd.numSamples = (int32)st->block;
    st->ctx.continousTimeSamples = st->steady;
    st->ctx.projectTimeSamples   = st->steady;

    tresult r = st->processor->process(st->pd);
    st->n_process++;
    st->steady += st->block;
    if (r != kResultOk) {
        set_err("class '%s' returned %d from process()", st->plugin_name.c_str(), (int)r);
        st->out_n = 0;
        return NAN;
    }
    for (int32 c = 0; c < ob->numChannels && c < (int32)st->chan; c++)
        memcpy(st->out[c], ob->channelBuffers32[c], sizeof(float) * (size_t)st->block);
    st->out_n = st->block;
    return (st->out_token = next_token(st, st->out_token));
}

/* ---------------------------------------------------------------- *
 * 9. The output block
 * ---------------------------------------------------------------- */

static int out_ok_ctx(State *st, double dep) {
    return st->open && st->out_token != 0 && token_matches(st, dep, st->out_token);
}

static double vst3_out_sample_ctx(State *st, double dep, double i, double ch) {
    if (!out_ok_ctx(st, dep) || !std::isfinite(i) || !std::isfinite(ch)) return NAN;
    long k = (long)(i + 0.5), c = (long)(ch + 0.5);
    if (k < 0 || k >= st->out_n || c < 0 || c >= st->chan) return NAN;
    return (double)st->out[c][k];
}

static double vst3_out_rms_ctx(State *st, double dep) {
    if (!out_ok_ctx(st, dep)) return NAN;
    if (st->out_n <= 0) return 0.0;
    double s = 0.0;
    for (long c = 0; c < st->chan; c++)
        for (long i = 0; i < st->out_n; i++) s += (double)st->out[c][i] * (double)st->out[c][i];
    return sqrt(s / (double)(st->out_n * st->chan));
}

static double vst3_out_peak_ctx(State *st, double dep) {
    if (!out_ok_ctx(st, dep)) return NAN;
    double m = 0.0;
    for (long c = 0; c < st->chan; c++)
        for (long i = 0; i < st->out_n; i++) {
            double a = fabs((double)st->out[c][i]);
            if (a > m) m = a;
        }
    return m;
}

static double vst3_out_count_ctx(State *st, double dep) { return out_ok_ctx(st, dep) ? (double)st->out_n : NAN; }
static double vst3_out_valid_ctx(State *st, double dep) { return out_ok_ctx(st, dep) ? 1.0 : 0.0; }


long vst3_host_scan(const char *path) { return vst3_host_scan_ctx(&DEFAULT_STATE, path); }

const char *vst3_host_scan_id(long i) { return vst3_host_scan_id_ctx(&DEFAULT_STATE, i); }

const char *vst3_host_scan_name(long i) { return vst3_host_scan_name_ctx(&DEFAULT_STATE, i); }

const char *vst3_host_scan_category(long i) { return vst3_host_scan_category_ctx(&DEFAULT_STATE, i); }

int vst3_host_open(const char *path, const char *class_id,
                   double sample_rate, double block_size, double channels) { return vst3_host_open_ctx(&DEFAULT_STATE, path, class_id, sample_rate, block_size, channels); }

void vst3_host_close(void) { vst3_host_close_ctx(&DEFAULT_STATE); }

const char *vst3_host_plugin_name(void) { return vst3_host_plugin_name_ctx(&DEFAULT_STATE); }

const char *vst3_host_plugin_name_for(double handle) {
    State *st = instance(handle);
    if (!st) return "";
    return vst3_host_plugin_name_ctx(st);
}

const char *vst3_host_plugin_id(void) { return vst3_host_plugin_id_ctx(&DEFAULT_STATE); }

const char *vst3_host_plugin_id_for(double handle) {
    State *st = instance(handle);
    if (!st) return "";
    return vst3_host_plugin_id_ctx(st);
}

long vst3_host_n_params(void) { return vst3_host_n_params_ctx(&DEFAULT_STATE); }

long vst3_host_n_params_for(double handle) {
    State *st = instance(handle);
    if (!st) return 0;
    return vst3_host_n_params_ctx(st);
}

double vst3_host_param_id(long i) { return vst3_host_param_id_ctx(&DEFAULT_STATE, i); }

double vst3_host_param_id_for(double handle, long i) {
    State *st = instance(handle);
    if (!st) return NAN;
    return vst3_host_param_id_ctx(st, i);
}

double vst3_host_param_min(long i) { return vst3_host_param_min_ctx(&DEFAULT_STATE, i); }

double vst3_host_param_min_for(double handle, long i) {
    State *st = instance(handle);
    if (!st) return NAN;
    return vst3_host_param_min_ctx(st, i);
}

double vst3_host_param_max(long i) { return vst3_host_param_max_ctx(&DEFAULT_STATE, i); }

double vst3_host_param_max_for(double handle, long i) {
    State *st = instance(handle);
    if (!st) return NAN;
    return vst3_host_param_max_ctx(st, i);
}

double vst3_host_param_default(long i) { return vst3_host_param_default_ctx(&DEFAULT_STATE, i); }

double vst3_host_param_default_for(double handle, long i) {
    State *st = instance(handle);
    if (!st) return NAN;
    return vst3_host_param_default_ctx(st, i);
}

double vst3_host_param_default_normalized(long i) { return vst3_host_param_default_normalized_ctx(&DEFAULT_STATE, i); }

double vst3_host_param_default_normalized_for(double handle, long i) {
    State *st = instance(handle);
    if (!st) return NAN;
    return vst3_host_param_default_normalized_ctx(st, i);
}

double vst3_host_param_steps(long i) { return vst3_host_param_steps_ctx(&DEFAULT_STATE, i); }

double vst3_host_param_steps_for(double handle, long i) {
    State *st = instance(handle);
    if (!st) return NAN;
    return vst3_host_param_steps_ctx(st, i);
}

double vst3_host_param_is_readonly(long i) { return vst3_host_param_is_readonly_ctx(&DEFAULT_STATE, i); }

double vst3_host_param_is_readonly_for(double handle, long i) {
    State *st = instance(handle);
    if (!st) return NAN;
    return vst3_host_param_is_readonly_ctx(st, i);
}

double vst3_host_param_is_bypass(long i) { return vst3_host_param_is_bypass_ctx(&DEFAULT_STATE, i); }

double vst3_host_param_is_bypass_for(double handle, long i) {
    State *st = instance(handle);
    if (!st) return NAN;
    return vst3_host_param_is_bypass_ctx(st, i);
}

const char *vst3_host_param_name(long i) { return vst3_host_param_name_ctx(&DEFAULT_STATE, i); }

const char *vst3_host_param_name_for(double handle, long i) {
    State *st = instance(handle);
    if (!st) return "";
    return vst3_host_param_name_ctx(st, i);
}

const char *vst3_host_param_units(long i) { return vst3_host_param_units_ctx(&DEFAULT_STATE, i); }

const char *vst3_host_param_units_for(double handle, long i) {
    State *st = instance(handle);
    if (!st) return "";
    return vst3_host_param_units_ctx(st, i);
}

double vst3_host_param_plain(double param_id, double normalized) { return vst3_host_param_plain_ctx(&DEFAULT_STATE, param_id, normalized); }

double vst3_host_param_plain_for(double handle, double param_id, double normalized) {
    State *st = instance(handle);
    if (!st) return NAN;
    return vst3_host_param_plain_ctx(st, param_id, normalized);
}

double vst3_host_param_normalized(double param_id, double plain) { return vst3_host_param_normalized_ctx(&DEFAULT_STATE, param_id, plain); }

double vst3_host_param_normalized_for(double handle, double param_id, double plain) {
    State *st = instance(handle);
    if (!st) return NAN;
    return vst3_host_param_normalized_ctx(st, param_id, plain);
}

double vst3_host_param_value(double param_id) { return vst3_host_param_value_ctx(&DEFAULT_STATE, param_id); }

double vst3_host_param_value_for(double handle, double param_id) {
    State *st = instance(handle);
    if (!st) return NAN;
    return vst3_host_param_value_ctx(st, param_id);
}

double vst3_host_latency(void) { return vst3_host_latency_ctx(&DEFAULT_STATE); }

double vst3_host_latency_for(double handle) {
    State *st = instance(handle);
    if (!st) return NAN;
    return vst3_host_latency_ctx(st);
}

double vst3_host_sample_rate(void) { return vst3_host_sample_rate_ctx(&DEFAULT_STATE); }

double vst3_host_sample_rate_for(double handle) {
    State *st = instance(handle);
    if (!st) return NAN;
    return vst3_host_sample_rate_ctx(st);
}

double vst3_host_block_size(void) { return vst3_host_block_size_ctx(&DEFAULT_STATE); }

double vst3_host_block_size_for(double handle) {
    State *st = instance(handle);
    if (!st) return NAN;
    return vst3_host_block_size_ctx(st);
}

double vst3_host_channels(void) { return vst3_host_channels_ctx(&DEFAULT_STATE); }

double vst3_host_channels_for(double handle) {
    State *st = instance(handle);
    if (!st) return NAN;
    return vst3_host_channels_ctx(st);
}

double vst3_host_is_open(void) { return vst3_host_is_open_ctx(&DEFAULT_STATE); }

double vst3_host_is_open_for(double handle) {
    State *st = instance(handle);
    if (!st) return 0;
    return vst3_host_is_open_ctx(st);
}

long   vst3_host_n_process(void) { return vst3_host_n_process_ctx(&DEFAULT_STATE); }

long   vst3_host_n_process_for(double handle) {
    State *st = instance(handle);
    if (!st) return 0;
    return vst3_host_n_process_ctx(st);
}

long   vst3_host_n_restart_requests(void) { return vst3_host_n_restart_requests_ctx(&DEFAULT_STATE); }

long   vst3_host_n_restart_requests_for(double handle) {
    State *st = instance(handle);
    if (!st) return 0;
    return vst3_host_n_restart_requests_ctx(st);
}

long   vst3_host_n_plugin_edits(void) { return vst3_host_n_plugin_edits_ctx(&DEFAULT_STATE); }

long   vst3_host_n_plugin_edits_for(double handle) {
    State *st = instance(handle);
    if (!st) return 0;
    return vst3_host_n_plugin_edits_ctx(st);
}

void vst3_host_reset_counters(void) { vst3_host_reset_counters_ctx(&DEFAULT_STATE); }

void vst3_host_reset_counters_for(double handle) {
    State *st = instance(handle);
    if (!st) return;
    vst3_host_reset_counters_ctx(st);
}

double vst3_in_fill(const double *samples, long n, long channels) { return vst3_in_fill_ctx(&DEFAULT_STATE, samples, n, channels); }

double vst3_in_fill_for(double handle, const double *samples, long n, long channels) {
    State *st = instance(handle);
    if (!st) return NAN;
    return vst3_in_fill_ctx(st, samples, n, channels);
}

double vst3_in_tone(double t, double waveform, double freq, double amp) { return vst3_in_tone_ctx(&DEFAULT_STATE, t, waveform, freq, amp); }

double vst3_in_tone_for(double handle, double t, double waveform, double freq, double amp) {
    State *st = instance(handle);
    if (!st) return NAN;
    return vst3_in_tone_ctx(st, t, waveform, freq, amp);
}

double vst3_in_sample(double dep, double i, double ch) { return vst3_in_sample_ctx(&DEFAULT_STATE, dep, i, ch); }

double vst3_in_sample_for(double handle, double dep, double i, double ch) {
    State *st = instance(handle);
    if (!st) return NAN;
    return vst3_in_sample_ctx(st, dep, i, ch);
}

double vst3_process(double dep,
                    double id0, double v0, double id1, double v1,
                    double id2, double v2, double id3, double v3) { return vst3_process_ctx(&DEFAULT_STATE, dep, id0, v0, id1, v1, id2, v2, id3, v3); }

double vst3_process_for(double handle, double dep,
                    double id0, double v0, double id1, double v1,
                    double id2, double v2, double id3, double v3) {
    State *st = instance(handle);
    if (!st) return NAN;
    return vst3_process_ctx(st, dep, id0, v0, id1, v1, id2, v2, id3, v3);
}

double vst3_out_sample(double dep, double i, double ch) { return vst3_out_sample_ctx(&DEFAULT_STATE, dep, i, ch); }

double vst3_out_sample_for(double handle, double dep, double i, double ch) {
    State *st = instance(handle);
    if (!st) return NAN;
    return vst3_out_sample_ctx(st, dep, i, ch);
}

double vst3_out_rms(double dep) { return vst3_out_rms_ctx(&DEFAULT_STATE, dep); }

double vst3_out_rms_for(double handle, double dep) {
    State *st = instance(handle);
    if (!st) return NAN;
    return vst3_out_rms_ctx(st, dep);
}

double vst3_out_peak(double dep) { return vst3_out_peak_ctx(&DEFAULT_STATE, dep); }

double vst3_out_peak_for(double handle, double dep) {
    State *st = instance(handle);
    if (!st) return NAN;
    return vst3_out_peak_ctx(st, dep);
}

double vst3_out_count(double dep) { return vst3_out_count_ctx(&DEFAULT_STATE, dep); }

double vst3_out_count_for(double handle, double dep) {
    State *st = instance(handle);
    if (!st) return NAN;
    return vst3_out_count_ctx(st, dep);
}

double vst3_out_valid(double dep) { return vst3_out_valid_ctx(&DEFAULT_STATE, dep); }

double vst3_out_valid_for(double handle, double dep) {
    State *st = instance(handle);
    if (!st) return 0;
    return vst3_out_valid_ctx(st, dep);
}


double vst3_host_n_audio_in_for(double handle) {
    State *st = instance(handle);
    return st && st->open && st->pd.numInputs > 0 ? st->pd.inputs[0].numChannels : 0;
}
double vst3_host_n_audio_out_for(double handle) {
    State *st = instance(handle);
    return st && st->open && st->pd.numOutputs > 0 ? st->pd.outputs[0].numChannels : 0;
}

long vst3_host_open_index_for(double handle) {
    State *st = instance(handle);
    return st && st->open ? st->open_index : -1;
}

double vst3_host_open_instance(const char *path, const char *id,
                              double rate, double block, double channels) {
    if (!std::isfinite(rate) || rate <= 0 || !std::isfinite(block) || block < 1 ||
        block > VST3_HOST_MAX_BLOCK || std::floor(block) != block ||
        !std::isfinite(channels) || channels < 1 || channels > VST3_HOST_MAX_CHAN || std::floor(channels) != channels ||
        NEXT_HANDLE >= 9007199254740991.0) {
        set_err("invalid VST3 instance configuration"); return NAN;
    }
    State *st = new (std::nothrow) State;
    if (!st) { set_err("out of memory opening VST3 instance"); return NAN; }
    st->handle = ++NEXT_HANDLE;
    if (vst3_host_open_ctx(st, path, id, rate, block, channels)) {
        vst3_host_close_ctx(st);
        delete st;
        return NAN;
    }
    st->next = INSTANCES;
    INSTANCES = st;
    return st->handle;
}

void vst3_host_close_instance(double handle) {
    State **link = &INSTANCES;
    while (*link && (*link)->handle != handle) link = &(*link)->next;
    if (!*link) return;
    State *st = *link;
    *link = st->next;
    vst3_host_close_ctx(st);
    delete st;
}

static double vst3_set_param_ctx(State *st, double dep, double id, double value) {
    if (!st->open || !st->in_armed || !token_matches(st, dep, st->in_token) || !std::isfinite(value)) return NAN;
    const ParamInfo *p = find_param_ctx(st, id);
    if (!p || p->readonly) return NAN;
    long k = (long)(p - st->params.data());
    st->pending[k] = value;
    st->queued[k] = 1;
    return dep;
}
double vst3_set_param(double dep, double id, double value) {
    return vst3_set_param_ctx(&DEFAULT_STATE, dep, id, value);
}
double vst3_set_param_for(double handle, double dep, double id, double value) {
    State *st = instance(handle);
    return st ? vst3_set_param_ctx(st, dep, id, value) : NAN;
}

static double vst3_expect_ctx(State *st, double dep, double index) {
    if (!st->open || !std::isfinite(dep) || !std::isfinite(index) || index < 0 || std::floor(index) != index || index != st->open_index) return NAN;
    return dep;
}
double vst3_expect(double dep, double index) { return vst3_expect_ctx(&DEFAULT_STATE, dep, index); }
double vst3_expect_for(double handle, double dep, double index) {
    State *st = instance(handle);
    return st ? vst3_expect_ctx(st, dep, index) : NAN;
}

double vst3_in_copy_for(double destination, double source, double dep) {
    State *dst = instance(destination), *src = instance(source);
    if (!dst || !src || !dst->open || !out_ok_ctx(src, dep)) return NAN;
    if (dst->block != src->block || dst->chan != src->chan || dst->sample_rate != src->sample_rate) {
        set_err("VST3 block copy configuration mismatch"); return NAN;
    }
    for (long c = 0; c < dst->chan; c++)
        memcpy(dst->in[c], src->out[c], sizeof(float) * (size_t)dst->block);
    memset(dst->queued, 0, sizeof dst->queued);

    dst->in_n = src->out_n;
    dst->in_armed = 1;
    return (dst->in_token = next_token(dst, dst->in_token));
}

} // extern "C"
