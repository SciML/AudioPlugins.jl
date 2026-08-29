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
#include <cstdio>
#include <cstring>
#include <memory>
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

long N_RESTART_REQS, N_PLUGIN_EDITS;
void on_restart(int32 flags);

/* IComponentHandler: the plugin telling the host about edits it initiates
 * (a meter, a GUI it does not have) and asking to be restarted. A
 * synchronous model has no scheduler to ask, so edits are counted and
 * restart flags are honoured where a headless host can: latency is
 * re-read on kLatencyChanged; everything else is recorded. */
class ComponentHandler : public IComponentHandler {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, IComponentHandler)
        QUERY_INTERFACE(iid, obj, IComponentHandler::iid, IComponentHandler)
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1000; }   /* static lifetime */
    uint32 PLUGIN_API release() override { return 1000; }

    tresult PLUGIN_API beginEdit(ParamID) override { N_PLUGIN_EDITS++; return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID, ParamValue) override { return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32 flags) override {
        N_RESTART_REQS++;
        on_restart(flags);
        return kResultOk;
    }
};

HostApplication HOST_APP;       /* IHostApplication + IMessage/IAttributeList factory */
ComponentHandler HANDLER;

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

    double slot_id[VST3_HOST_PARAM_SLOTS];
    double slot_val[VST3_HOST_PARAM_SLOTS];

    float  in[VST3_HOST_MAX_CHAN][VST3_HOST_MAX_BLOCK];
    float  out[VST3_HOST_MAX_CHAN][VST3_HOST_MAX_BLOCK];

    long   in_token = 0, out_token = 0;
    long   in_n = 0, out_n = 0;
    long   n_process = 0;
    int64  steady = 0;
};

State S;

const ParamInfo *find_param(double id) {
    if (!(id >= 0.0)) return nullptr;
    ParamID pid = (ParamID)(uint32)id;
    for (const auto &p : S.params) if (p.id == pid) return &p;
    return nullptr;
}

void on_restart(int32 flags) {
    if ((flags & kLatencyChanged) && S.processor && S.open)
        S.latency = (double)S.processor->getLatencySamples();
}

std::string utf8(const String128 s) { return Steinberg::Vst::StringConvert::convert(s); }

/* ---------------------------------------------------------------- *
 * 4. Discovery
 * ---------------------------------------------------------------- */

void unload() {
    S.provider.reset();      /* terminates component + controller */
    S.processor = nullptr;
    S.controller = nullptr;
    S.component = nullptr;
    S.module.reset();
}

} // namespace

extern "C" {

const char *vst3_host_last_error(void) { return ERR; }

long vst3_host_scan(const char *path) {
    ERR[0] = '\0';
    vst3_host_close();
    if (!path || !path[0]) { set_err("no plugin path given"); return -1; }

    std::string err;
    S.module = VST3::Hosting::Module::create(path, err);
    if (!S.module) {
        set_err("cannot load VST3 bundle %s: %s", path,
                err.empty() ? "not a VST3 bundle or shared object" : err.c_str());
        return -1;
    }
    S.path = path;
    S.classes.clear();
    for (const auto &ci : S.module->getFactory().classInfos())
        if (ci.category() == kVstAudioEffectClass) S.classes.push_back(ci);
    if (S.classes.empty()) {
        set_err("%s loads but exports no audio-effect class (%d classes total)", path,
                (int)S.module->getFactory().classInfos().size());
        unload();
        return -1;
    }
    return (long)S.classes.size();
}

static std::string SCAN_TMP[3];
const char *vst3_host_scan_id(long i) {
    if (i < 0 || i >= (long)S.classes.size()) return "";
    SCAN_TMP[0] = S.classes[(size_t)i].ID().toString();
    return SCAN_TMP[0].c_str();
}
const char *vst3_host_scan_name(long i) {
    if (i < 0 || i >= (long)S.classes.size()) return "";
    SCAN_TMP[1] = S.classes[(size_t)i].name();
    return SCAN_TMP[1].c_str();
}
const char *vst3_host_scan_category(long i) {
    if (i < 0 || i >= (long)S.classes.size()) return "";
    SCAN_TMP[2] = S.classes[(size_t)i].subCategoriesString();
    return SCAN_TMP[2].c_str();
}

/* ---------------------------------------------------------------- *
 * 5. Open / close
 * ---------------------------------------------------------------- */

static void fail_open(void) { vst3_host_close(); }

static void read_params(void) {
    S.params.clear();
    if (!S.controller) return;
    int32 n = S.controller->getParameterCount();
    for (int32 i = 0; i < n && (long)S.params.size() < VST3_HOST_MAX_PARAMS; i++) {
        ParameterInfo info;
        memset(&info, 0, sizeof info);
        if (S.controller->getParameterInfo(i, info) != kResultOk) continue;
        ParamInfo p;
        p.id       = info.id;
        p.def_norm = info.defaultNormalizedValue;
        p.min      = S.controller->normalizedParamToPlain(info.id, 0.0);
        p.max      = S.controller->normalizedParamToPlain(info.id, 1.0);
        p.def      = S.controller->normalizedParamToPlain(info.id, info.defaultNormalizedValue);
        p.steps    = (double)info.stepCount;
        p.readonly = (info.flags & ParameterInfo::kIsReadOnly) != 0;
        p.bypass   = (info.flags & ParameterInfo::kIsBypass) != 0;
        p.name     = utf8(info.title);
        p.units    = utf8(info.units);
        S.params.push_back(p);
    }
}

int vst3_host_open(const char *path, const char *class_id,
                   double sample_rate, double block_size, double channels) {
    long n = vst3_host_scan(path);      /* also clears state and sets ERR */
    if (n < 0) return 1;

    long blk = (long)(block_size + 0.5);
    long ch  = (long)(channels + 0.5);
    if (blk < 1 || blk > VST3_HOST_MAX_BLOCK) {
        set_err("block_size %ld out of range 1..%d", blk, VST3_HOST_MAX_BLOCK);
        fail_open();
        return 1;
    }
    if (ch < 1 || ch > VST3_HOST_MAX_CHAN) {
        set_err("channels %ld out of range 1..%d", ch, VST3_HOST_MAX_CHAN);
        fail_open();
        return 1;
    }
    if (!(sample_rate > 0.0)) {
        set_err("sample_rate must be positive, got %g", sample_rate);
        fail_open();
        return 1;
    }

    /* Pick the class. */
    const VST3::Hosting::ClassInfo *ci = nullptr;
    if (class_id && class_id[0]) {
        for (const auto &c : S.classes)
            if (c.ID().toString() == class_id) { ci = &c; break; }
        if (!ci) {
            set_err("no audio-effect class with id '%s' in %s (it has %ld: first is '%s' %s)",
                    class_id, path, (long)S.classes.size(),
                    S.classes[0].ID().toString().c_str(), S.classes[0].name().c_str());
            fail_open();
            return 1;
        }
    } else {
        ci = &S.classes[0];
    }
    S.plugin_name = ci->name();
    S.plugin_id   = ci->ID().toString();

    /* Instantiate component and controller, connect them (PlugProvider
     * does the IConnectionPoint dance and the separate-controller case),
     * with our IHostApplication as the context. */
    PluginContextFactory::instance().setPluginContext(&HOST_APP);
    S.provider.reset(new PlugProvider(S.module->getFactory(), *ci, true));
    if (!S.provider->initialize()) {
        set_err("class '%s' could not be instantiated or initialised", S.plugin_name.c_str());
        fail_open();
        return 1;
    }
    S.component  = S.provider->getComponentPtr();
    S.controller = S.provider->getControllerPtr();
    if (!S.component) {
        set_err("class '%s' has no IComponent", S.plugin_name.c_str());
        fail_open();
        return 1;
    }
    S.processor = FUnknownPtr<IAudioProcessor>(S.component);
    if (!S.processor) {
        set_err("class '%s' does not implement IAudioProcessor", S.plugin_name.c_str());
        fail_open();
        return 1;
    }
    if (S.controller) S.controller->setComponentHandler(&HANDLER);

    /* Bus arrangement: mono or stereo on the main input and output. A
     * plugin without a main input (an instrument) is still fine; one
     * without an audio output is not. */
    int32 n_in  = S.component->getBusCount(kAudio, kInput);
    int32 n_out = S.component->getBusCount(kAudio, kOutput);
    if (n_out < 1) {
        set_err("class '%s' has no audio output bus", S.plugin_name.c_str());
        fail_open();
        return 1;
    }
    SpeakerArrangement arr = (ch == 2) ? SpeakerArr::kStereo : SpeakerArr::kMono;
    std::vector<SpeakerArrangement> ins((size_t)n_in, arr), outs((size_t)n_out, arr);
    /* Only the main buses carry our channels; ask for the same layout on
     * the others but deactivate them below. */
    tresult r = S.processor->setBusArrangements(ins.empty() ? nullptr : ins.data(), n_in,
                                                outs.data(), n_out);
    if (r != kResultOk) {
        set_err("class '%s' refused a %s arrangement on its main buses (%d in, %d out)",
                S.plugin_name.c_str(), ch == 2 ? "stereo" : "mono", n_in, n_out);
        fail_open();
        return 1;
    }
    /* Verify what the plugin actually settled on for the main buses. */
    SpeakerArrangement got_out = 0;
    S.processor->getBusArrangement(kOutput, 0, got_out);
    if (SpeakerArr::getChannelCount(got_out) != (int32)ch) {
        set_err("class '%s' set its main output to %d channel(s), not the %ld asked for",
                S.plugin_name.c_str(), SpeakerArr::getChannelCount(got_out), ch);
        fail_open();
        return 1;
    }
    if (n_in > 0) {
        SpeakerArrangement got_in = 0;
        S.processor->getBusArrangement(kInput, 0, got_in);
        if (SpeakerArr::getChannelCount(got_in) != (int32)ch) {
            set_err("class '%s' set its main input to %d channel(s), not the %ld asked for",
                    S.plugin_name.c_str(), SpeakerArr::getChannelCount(got_in), ch);
            fail_open();
            return 1;
        }
    }
    for (int32 i = 0; i < n_in;  i++) S.component->activateBus(kAudio, kInput,  i, i == 0);
    for (int32 i = 0; i < n_out; i++) S.component->activateBus(kAudio, kOutput, i, i == 0);
    for (int32 i = 0; i < S.component->getBusCount(kEvent, kInput);  i++)
        S.component->activateBus(kEvent, kInput, i, false);
    for (int32 i = 0; i < S.component->getBusCount(kEvent, kOutput); i++)
        S.component->activateBus(kEvent, kOutput, i, false);

    if (S.processor->canProcessSampleSize(kSample32) != kResultOk) {
        set_err("class '%s' cannot process 32-bit float", S.plugin_name.c_str());
        fail_open();
        return 1;
    }

    ProcessSetup setup;
    setup.processMode        = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = (int32)blk;
    setup.sampleRate         = sample_rate;
    if (S.processor->setupProcessing(setup) != kResultOk) {
        set_err("class '%s' refused setupProcessing(%g Hz, %ld frames, float32)",
                S.plugin_name.c_str(), sample_rate, blk);
        fail_open();
        return 1;
    }
    if (S.component->setActive(true) != kResultOk) {
        set_err("class '%s' refused setActive(true)", S.plugin_name.c_str());
        fail_open();
        return 1;
    }
    S.open = true;      /* from here on, close() must deactivate */

    /* Buffers for every bus, owned by HostProcessData, sized once. The
     * main buses are copied from / to the static block buffers per tick. */
    if (!S.pd.prepare(*S.component, (int32)blk, kSample32)) {
        set_err("could not prepare process buffers for class '%s'", S.plugin_name.c_str());
        fail_open();
        return 1;
    }
    S.in_changes.setMaxParameters(VST3_HOST_PARAM_SLOTS);
    S.out_changes.setMaxParameters(VST3_HOST_MAX_PARAMS);
    S.pd.inputParameterChanges  = &S.in_changes;
    S.pd.outputParameterChanges = &S.out_changes;
    S.pd.inputEvents  = &S.in_events;
    S.pd.outputEvents = &S.out_events;
    memset(&S.ctx, 0, sizeof S.ctx);
    S.ctx.sampleRate = sample_rate;
    S.ctx.state = ProcessContext::kPlaying | ProcessContext::kContTimeValid;
    S.pd.processContext = &S.ctx;

    read_params();
    S.latency = (double)S.processor->getLatencySamples();

    if (S.processor->setProcessing(true) != kResultOk) {
        /* Optional per spec: a plugin may return kNotImplemented. */
    }
    S.processing = true;

    S.sample_rate = sample_rate;
    S.block = blk;
    S.chan  = ch;
    for (int i = 0; i < VST3_HOST_PARAM_SLOTS; i++) {
        S.slot_id[i]  = -1.0;
        S.slot_val[i] = NAN;
    }
    memset(S.in, 0, sizeof S.in);
    memset(S.out, 0, sizeof S.out);
    S.in_token = S.out_token = 0;
    S.in_n = S.out_n = 0;
    S.n_process = 0;
    S.steady = 0;
    return 0;
}

void vst3_host_close(void) {
    if (S.processor && S.processing) S.processor->setProcessing(false);
    S.processing = false;
    if (S.component && S.open) S.component->setActive(false);
    S.pd.unprepare();
    S.pd.inputParameterChanges = nullptr;
    S.pd.outputParameterChanges = nullptr;
    S.pd.inputEvents = nullptr;
    S.pd.outputEvents = nullptr;
    S.pd.processContext = nullptr;
    S.in_changes.clearQueue();
    S.out_changes.clearQueue();
    S.open = false;
    S.params.clear();
    S.latency = 0;
    S.in_token = S.out_token = 0;
    S.in_n = S.out_n = 0;
    S.n_process = 0;
    S.steady = 0;
    S.plugin_name.clear();
    S.plugin_id.clear();
    /* The class cache survives, as in the other hosts. */
    unload();
}

const char *vst3_host_plugin_name(void) { return S.plugin_name.c_str(); }
const char *vst3_host_plugin_id(void)   { return S.plugin_id.c_str(); }

/* ---------------------------------------------------------------- *
 * 6. Parameter and configuration reporting
 * ---------------------------------------------------------------- */

long vst3_host_n_params(void) { return (long)S.params.size(); }

static const ParamInfo *pat(long i) {
    return (i >= 0 && i < (long)S.params.size()) ? &S.params[(size_t)i] : nullptr;
}

double vst3_host_param_id(long i)      { const ParamInfo *p = pat(i); return p ? (double)p->id : -1.0; }
double vst3_host_param_min(long i)     { const ParamInfo *p = pat(i); return p ? p->min : NAN; }
double vst3_host_param_max(long i)     { const ParamInfo *p = pat(i); return p ? p->max : NAN; }
double vst3_host_param_default(long i) { const ParamInfo *p = pat(i); return p ? p->def : NAN; }
double vst3_host_param_default_normalized(long i) { const ParamInfo *p = pat(i); return p ? p->def_norm : NAN; }
double vst3_host_param_steps(long i)   { const ParamInfo *p = pat(i); return p ? p->steps : NAN; }
double vst3_host_param_is_readonly(long i) { const ParamInfo *p = pat(i); return p ? (p->readonly ? 1.0 : 0.0) : NAN; }
double vst3_host_param_is_bypass(long i)   { const ParamInfo *p = pat(i); return p ? (p->bypass ? 1.0 : 0.0) : NAN; }
const char *vst3_host_param_name(long i)  { const ParamInfo *p = pat(i); return p ? p->name.c_str() : ""; }
const char *vst3_host_param_units(long i) { const ParamInfo *p = pat(i); return p ? p->units.c_str() : ""; }

double vst3_host_param_plain(double param_id, double normalized) {
    const ParamInfo *p = find_param(param_id);
    if (!p || !S.controller) return NAN;
    return S.controller->normalizedParamToPlain(p->id, normalized);
}
double vst3_host_param_normalized(double param_id, double plain) {
    const ParamInfo *p = find_param(param_id);
    if (!p || !S.controller) return NAN;
    return S.controller->plainParamToNormalized(p->id, plain);
}
double vst3_host_param_value(double param_id) {
    const ParamInfo *p = find_param(param_id);
    if (!p || !S.controller || !S.open) return NAN;
    return S.controller->getParamNormalized(p->id);
}

double vst3_host_latency(void)     { return S.open ? S.latency : 0.0; }
double vst3_host_sample_rate(void) { return S.open ? S.sample_rate : 0.0; }
double vst3_host_block_size(void)  { return S.open ? (double)S.block : 0.0; }
double vst3_host_channels(void)    { return S.open ? (double)S.chan : 0.0; }
double vst3_host_is_open(void)     { return S.open ? 1.0 : 0.0; }
long   vst3_host_n_process(void)   { return S.n_process; }
long   vst3_host_n_restart_requests(void) { return N_RESTART_REQS; }
long   vst3_host_n_plugin_edits(void)     { return N_PLUGIN_EDITS; }

void vst3_host_reset_counters(void) {
    S.n_process = 0;
    S.steady = 0;
    N_RESTART_REQS = N_PLUGIN_EDITS = 0;
}

/* ---------------------------------------------------------------- *
 * 7. The input block
 * ---------------------------------------------------------------- */

double vst3_in_fill(const double *samples, long n, long channels) {
    if (!S.open) { set_err("no plugin is open"); return NAN; }
    if (!samples || n < 0) { set_err("vst3_in_fill: no samples"); return NAN; }
    if (n > S.block) n = S.block;
    long ch = (channels < 1) ? 1 : (channels > S.chan ? S.chan : channels);
    for (long i = 0; i < n; i++)
        for (long c = 0; c < S.chan; c++) {
            long src = (c < ch) ? (i * ch + c) : (i * ch);   /* mono -> all */
            S.in[c][i] = (float)samples[src];
        }
    for (long i = n; i < S.block; i++)
        for (long c = 0; c < S.chan; c++) S.in[c][i] = 0.0f;
    S.in_n = n;
    return (double)(++S.in_token);
}

static double wave_at(long k, int w, double freq, double amp) {
    double t = (double)k / S.sample_rate;
    switch (w) {
    case VST3_WAVE_SINE:    return amp * sin(2.0 * M_PI * freq * t);
    case VST3_WAVE_SQUARE:  return amp * (sin(2.0 * M_PI * freq * t) >= 0.0 ? 1.0 : -1.0);
    case VST3_WAVE_RAMP:    return amp * (2.0 * fmod(freq * t, 1.0) - 1.0);
    case VST3_WAVE_IMPULSE: return (k == 0) ? amp : 0.0;
    default:                return 0.0;
    }
}

double vst3_in_tone(double t, double waveform, double freq, double amp) {
    if (!S.open) { set_err("no plugin is open"); return NAN; }
    double end = t * S.sample_rate;
    long first = (long)(end + 0.5) - S.block;
    int w = (int)(waveform + 0.5);
    for (long i = 0; i < S.block; i++) {
        double v = wave_at(first + i, w, freq, amp);
        for (long c = 0; c < S.chan; c++) S.in[c][i] = (float)v;
    }
    S.in_n = S.block;
    return (double)(++S.in_token);
}

double vst3_in_sample(double dep, double i, double ch) {
    if (!S.open) return NAN;
    if ((long)(dep + 0.5) != S.in_token) return NAN;
    long k = (long)(i + 0.5), c = (long)(ch + 0.5);
    if (k < 0 || k >= S.in_n || c < 0 || c >= S.chan) return NAN;
    return (double)S.in[c][k];
}

/* ---------------------------------------------------------------- *
 * 8. process() -- the node-side operator
 * ---------------------------------------------------------------- */

double vst3_process(double dep,
                    double id0, double v0, double id1, double v1,
                    double id2, double v2, double id3, double v3) {
    if (!S.open) { set_err("no plugin is open"); return NAN; }
    if ((long)(dep + 0.5) != S.in_token || S.in_token == 0) return NAN;

    /* Only send what changed, through inputParameterChanges at offset 0,
     * mirrored into the controller so the two views agree. */
    const double ids[VST3_HOST_PARAM_SLOTS]  = { id0, id1, id2, id3 };
    const double vals[VST3_HOST_PARAM_SLOTS] = { v0,  v1,  v2,  v3  };
    S.in_changes.clearQueue();
    S.out_changes.clearQueue();
    for (int i = 0; i < VST3_HOST_PARAM_SLOTS; i++) {
        if (!(ids[i] >= 0.0)) { S.slot_id[i] = -1.0; S.slot_val[i] = NAN; continue; }
        if (std::isnan(vals[i])) continue;
        bool changed = (S.slot_id[i] != ids[i]) || std::isnan(S.slot_val[i]) ||
                       (S.slot_val[i] != vals[i]);
        if (!changed) continue;
        const ParamInfo *p = find_param(ids[i]);
        if (!p) continue;            /* unknown id: ignored, not corrupted */
        double v = vals[i] < 0.0 ? 0.0 : (vals[i] > 1.0 ? 1.0 : vals[i]);
        int32 qi = 0, pi = 0;
        IParamValueQueue *q = S.in_changes.addParameterData(p->id, qi);
        if (q) q->addPoint(0, v, pi);
        if (S.controller) S.controller->setParamNormalized(p->id, v);
        S.slot_id[i]  = ids[i];
        S.slot_val[i] = vals[i];
    }

    /* Main buses: copy the block in, run, copy the block out. */
    AudioBusBuffers *ib = (S.pd.numInputs > 0) ? &S.pd.inputs[0] : nullptr;
    AudioBusBuffers *ob = &S.pd.outputs[0];
    if (ib) {
        for (int32 c = 0; c < ib->numChannels && c < (int32)S.chan; c++)
            memcpy(ib->channelBuffers32[c], S.in[c], sizeof(float) * (size_t)S.block);
        ib->silenceFlags = 0;
    }
    for (int32 c = 0; c < ob->numChannels; c++)
        memset(ob->channelBuffers32[c], 0, sizeof(float) * (size_t)S.block);
    S.pd.numSamples = (int32)S.block;
    S.ctx.continousTimeSamples = S.steady;
    S.ctx.projectTimeSamples   = S.steady;

    tresult r = S.processor->process(S.pd);
    S.n_process++;
    S.steady += S.block;
    if (r != kResultOk) {
        set_err("class '%s' returned %d from process()", S.plugin_name.c_str(), (int)r);
        S.out_n = 0;
        return NAN;
    }
    for (int32 c = 0; c < ob->numChannels && c < (int32)S.chan; c++)
        memcpy(S.out[c], ob->channelBuffers32[c], sizeof(float) * (size_t)S.block);
    S.out_n = S.block;
    return (double)(++S.out_token);
}

/* ---------------------------------------------------------------- *
 * 9. The output block
 * ---------------------------------------------------------------- */

static int out_ok(double dep) {
    return S.open && S.out_token != 0 && (long)(dep + 0.5) == S.out_token;
}

double vst3_out_sample(double dep, double i, double ch) {
    if (!out_ok(dep)) return NAN;
    long k = (long)(i + 0.5), c = (long)(ch + 0.5);
    if (k < 0 || k >= S.out_n || c < 0 || c >= S.chan) return NAN;
    return (double)S.out[c][k];
}

double vst3_out_rms(double dep) {
    if (!out_ok(dep)) return NAN;
    if (S.out_n <= 0) return 0.0;
    double s = 0.0;
    for (long c = 0; c < S.chan; c++)
        for (long i = 0; i < S.out_n; i++) s += (double)S.out[c][i] * (double)S.out[c][i];
    return sqrt(s / (double)(S.out_n * S.chan));
}

double vst3_out_peak(double dep) {
    if (!out_ok(dep)) return NAN;
    double m = 0.0;
    for (long c = 0; c < S.chan; c++)
        for (long i = 0; i < S.out_n; i++) {
            double a = fabs((double)S.out[c][i]);
            if (a > m) m = a;
        }
    return m;
}

double vst3_out_count(double dep) { return out_ok(dep) ? (double)S.out_n : NAN; }
double vst3_out_valid(double dep) { return out_ok(dep) ? 1.0 : 0.0; }

} // extern "C"
