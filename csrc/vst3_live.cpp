/* Per-session VST3 adapter. All SDK/controller work stays on control;
 * only setProcessing and process run on the native processing thread. */
#include "live_adapter.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include <array>
#include <cstring>
#include <memory>
#include <vector>
using namespace Steinberg;
using namespace Steinberg::Vst;
namespace {
/* Fixed storage, unlike SDK ParameterChanges whose point vectors can grow. */
#define INTERFACE(Type) \
    tresult PLUGIN_API queryInterface(const TUID id, void **out) override { \
        QUERY_INTERFACE(id, out, FUnknown::iid, Type) \
        QUERY_INTERFACE(id, out, Type::iid, Type) \
        *out = nullptr; return kNoInterface; \
    } \
    uint32 PLUGIN_API addRef() override { return 1; } \
    uint32 PLUGIN_API release() override { return 1; }
struct Queue : IParamValueQueue {
    INTERFACE(IParamValueQueue)
    ParamID id = 0;
    int32 n = 0;
    struct Point { int32 frame; ParamValue value; } points[AP_LIVE_MAX_EVENTS];
    ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return n; }
    tresult PLUGIN_API getPoint(int32 i, int32 &offset, ParamValue &value) override {
        if (i < 0 || i >= n) return kInvalidArgument;
        offset = points[i].frame; value = points[i].value; return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32 offset, ParamValue value, int32 &index) override {
        if (n == AP_LIVE_MAX_EVENTS) return kResultFalse;
        index = n; points[n++] = {offset, value}; return kResultOk;
    }
};
struct Changes : IParameterChanges {
    INTERFACE(IParameterChanges)
    int32 n = 0;
    Queue queues[AP_LIVE_MAX_EVENTS];
    int32 PLUGIN_API getParameterCount() override { return n; }
    IParamValueQueue *PLUGIN_API getParameterData(int32 i) override { return i >= 0 && i < n ? &queues[i] : nullptr; }
    IParamValueQueue *PLUGIN_API addParameterData(const ParamID &id, int32 &index) override {
        for (int32 i = 0; i < n; ++i) if (queues[i].id == id) { index = i; return &queues[i]; }
        if (n == AP_LIVE_MAX_EVENTS) return nullptr;
        index = n++; queues[index].id = id; queues[index].n = 0; return &queues[index];
    }
};
struct Events : IEventList {
    INTERFACE(IEventList)
    int32 n = 0;
    Event events[AP_LIVE_MAX_EVENTS];
    int32 PLUGIN_API getEventCount() override { return n; }
    tresult PLUGIN_API getEvent(int32 i, Event &event) override {
        if (i < 0 || i >= n) return kInvalidArgument;
        event = events[i]; return kResultOk;
    }
    tresult PLUGIN_API addEvent(Event &event) override {
        if (n == AP_LIVE_MAX_EVENTS) return kResultFalse;
        events[n++] = event; return kResultOk;
    }
};
struct Handler : IComponentHandler {
    INTERFACE(IComponentHandler)
    const clap_host_t *host = nullptr;
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID, ParamValue) override { return kNotImplemented; }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) override { host->request_restart(host); return kResultOk; }
};
class Context : public HostApplication {
public:
    const clap_host_t *host = nullptr;
    tresult PLUGIN_API createInstance(TUID cid, TUID iid, void **object) override {
        auto threads = static_cast<const clap_host_thread_check_t *>(host->get_extension(host, CLAP_EXT_THREAD_CHECK));
        if (!threads->is_main_thread(host)) { *object = nullptr; return kNotImplemented; }
        return HostApplication::createInstance(cid, iid, object);
    }
};
class Provider : public PlugProvider {
public:
    using PlugProvider::PlugProvider;
    bool setup(FUnknown *context) { return setupPlugin(context); }
};
struct State {
    clap_plugin_t api{};
    const clap_host_t *host;
    ap_live_config config;
    Context context;
    Handler handler;
    VST3::Hosting::Module::Ptr module;
    std::unique_ptr<Provider> provider;
    IPtr<IComponent> component;
    IPtr<IEditController> controller;
    IPtr<IAudioProcessor> processor;
    std::vector<clap_param_info_t> parameters;
    Changes in_changes, out_changes;
    Events in_events, out_events;
    ProcessContext clock{};
    bool notes = false;
    ~State() {
        if (controller) controller->setComponentHandler(nullptr);
        provider.reset(); processor = nullptr; controller = nullptr; component = nullptr;
    }
};
static State &state(const clap_plugin_t *p) { return *static_cast<State *>(p->plugin_data); }
static bool CLAP_ABI init(const clap_plugin_t *p) { return !!state(p).processor; }
static void CLAP_ABI destroy(const clap_plugin_t *p) { delete &state(p); }
static bool CLAP_ABI activate(const clap_plugin_t *p, double rate, uint32_t lo, uint32_t hi) {
    auto &s = state(p);
    ProcessSetup setup{kRealtime, kSample32, static_cast<int32>(hi), rate};
    return lo == hi && s.processor->setupProcessing(setup) == kResultOk && s.component->setActive(true) == kResultOk;
}
static void CLAP_ABI deactivate(const clap_plugin_t *p) { state(p).component->setActive(false); }
static bool CLAP_ABI start(const clap_plugin_t *p) {
    auto result = state(p).processor->setProcessing(true);
    return result == kResultOk || result == kNotImplemented;
}
static void CLAP_ABI stop(const clap_plugin_t *p) { state(p).processor->setProcessing(false); }
static uint32_t CLAP_ABI latency(const clap_plugin_t *p) { return state(p).processor->getLatencySamples(); }
static clap_process_status CLAP_ABI process(const clap_plugin_t *p, const clap_process_t *pr) {
    auto &s = state(p);
    s.in_changes.n = s.out_changes.n = s.in_events.n = s.out_events.n = 0;
    for (uint32_t i = 0; i < pr->in_events->size(pr->in_events); ++i) {
        auto h = pr->in_events->get(pr->in_events, i);
        if (h->type == CLAP_EVENT_PARAM_VALUE) {
            auto v = reinterpret_cast<const clap_event_param_value_t *>(h);
            int32 index;
            auto q = s.in_changes.addParameterData(v->param_id, index);
            if (!q || q->addPoint(h->time, v->value, index) != kResultOk) return CLAP_PROCESS_ERROR;
        } else if (h->type == CLAP_EVENT_MIDI) {
            auto m = reinterpret_cast<const clap_event_midi_t *>(h);
            Event e{}; e.sampleOffset = h->time;
            auto kind = m->data[0] & 0xf0;
            if (kind == 0x90 && m->data[2]) {
                e.type = Event::kNoteOnEvent; e.noteOn.channel = m->data[0] & 15;
                e.noteOn.pitch = m->data[1]; e.noteOn.velocity = m->data[2] / 127.0f; e.noteOn.noteId = -1;
            } else if (kind == 0x80 || kind == 0x90) {
                e.type = Event::kNoteOffEvent; e.noteOff.channel = m->data[0] & 15;
                e.noteOff.pitch = m->data[1]; e.noteOff.velocity = m->data[2] / 127.0f; e.noteOff.noteId = -1;
            } else return CLAP_PROCESS_ERROR;
            if (s.in_events.addEvent(e) != kResultOk) return CLAP_PROCESS_ERROR;
        }
    }
    AudioBusBuffers input{}, output{};
    input.numChannels = output.numChannels = s.config.channels;
    input.channelBuffers32 = pr->audio_inputs[0].data32;
    output.channelBuffers32 = pr->audio_outputs[0].data32;
    s.clock.sampleRate = s.config.sample_rate; s.clock.projectTimeSamples = pr->steady_time;
    s.clock.continousTimeSamples = pr->steady_time;
    s.clock.state = ProcessContext::kPlaying | ProcessContext::kContTimeValid;
    ProcessData data{};
    data.processMode = kRealtime; data.symbolicSampleSize = kSample32; data.numSamples = pr->frames_count;
    data.numInputs = data.numOutputs = 1; data.inputs = &input; data.outputs = &output;
    data.inputParameterChanges = &s.in_changes; data.outputParameterChanges = &s.out_changes;
    data.inputEvents = &s.in_events; data.outputEvents = &s.out_events; data.processContext = &s.clock;
    if (s.processor->process(data) != kResultOk) return CLAP_PROCESS_ERROR;
    for (int32 i = 0; i < s.out_changes.n; ++i) {
        auto &q = s.out_changes.queues[i];
        for (int32 j = 0; j < q.n; ++j) {
            clap_event_param_value_t event{};
            event.header.size = sizeof(event); event.header.type = CLAP_EVENT_PARAM_VALUE;
            event.header.time = q.points[j].frame; event.param_id = q.id; event.value = q.points[j].value;
            pr->out_events->try_push(pr->out_events, &event.header);
        }
    }
    for (int32 i = 0; i < s.out_events.n; ++i) {
        clap_event_header_t dropped{}; // Count unsupported output events honestly.
        pr->out_events->try_push(pr->out_events, &dropped);
    }
    return CLAP_PROCESS_CONTINUE;
}
static uint32_t CLAP_ABI port_count(const clap_plugin_t *, bool) { return 1; }
static bool CLAP_ABI port_info(const clap_plugin_t *p, uint32_t i, bool, clap_audio_port_info_t *info) {
    if (i) return false;
    *info = {}; info->channel_count = state(p).config.channels; info->flags = CLAP_AUDIO_PORT_IS_MAIN; return true;
}
static uint32_t CLAP_ABI param_count(const clap_plugin_t *p) { return static_cast<uint32_t>(state(p).parameters.size()); }
static bool CLAP_ABI param_info(const clap_plugin_t *p, uint32_t i, clap_param_info_t *info) {
    auto &s = state(p); if (i >= s.parameters.size()) return false; *info = s.parameters[i]; return true;
}
static uint32_t CLAP_ABI note_count(const clap_plugin_t *p, bool in) { return in && state(p).notes ? 1 : 0; }
static bool CLAP_ABI note_info(const clap_plugin_t *p, uint32_t i, bool in, clap_note_port_info_t *info) {
    if (i || !note_count(p, in)) return false;
    *info = {}; info->supported_dialects = CLAP_NOTE_DIALECT_MIDI; return true;
}
static const clap_plugin_audio_ports_t ports{port_count, port_info};
static const clap_plugin_params_t params{param_count, param_info, nullptr, nullptr, nullptr, nullptr};
static const clap_plugin_note_ports_t notes{note_count, note_info};
static const clap_plugin_latency_t latency_ext{latency};
static const void *CLAP_ABI extension(const clap_plugin_t *, const char *id) {
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &ports;
    if (!strcmp(id, CLAP_EXT_PARAMS)) return &params;
    if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &notes;
    if (!strcmp(id, CLAP_EXT_LATENCY)) return &latency_ext;
    if (!strcmp(id, "audioplugins.notes-only")) return &notes;
    return nullptr;
}
static void CLAP_ABI callback(const clap_plugin_t *) {}
static const clap_plugin_t *create(const clap_host_t *host, const char *path, const char *id, const ap_live_config *config) {
    try {
        auto s = std::make_unique<State>();
        volatile unsigned char *memory = reinterpret_cast<volatile unsigned char *>(s.get());
        for (size_t i = 0; i < sizeof(State); i += 4096) memory[i] = memory[i];
        s->host = host; s->config = *config;
        s->context.host = s->handler.host = host;
        std::string error;
        s->module = VST3::Hosting::Module::create(path, error); if (!s->module) return nullptr;
        for (const auto &info : s->module->getFactory().classInfos()) {
            if (info.category() != kVstAudioEffectClass || (*id && info.ID().toString() != id)) continue;
            s->provider = std::make_unique<Provider>(s->module->getFactory(), info, true); break;
        }
        if (!s->provider || !s->provider->setup(&s->context)) return nullptr;
        s->component = s->provider->getComponentPtr(); s->controller = s->provider->getControllerPtr();
        s->processor = FUnknownPtr<IAudioProcessor>(s->component);
        if (!s->component || !s->processor || s->processor->canProcessSampleSize(kSample32) != kResultOk) return nullptr;
        if (s->component->getBusCount(kAudio, kInput) != 1 || s->component->getBusCount(kAudio, kOutput) != 1) return nullptr;
        SpeakerArrangement arrangement = config->channels == 2 ? SpeakerArr::kStereo : SpeakerArr::kMono;
        if (s->processor->setBusArrangements(&arrangement, 1, &arrangement, 1) != kResultOk) return nullptr;
        for (auto direction : {kInput, kOutput}) {
            SpeakerArrangement got = 0;
            if (s->processor->getBusArrangement(direction, 0, got) != kResultOk || got != arrangement) return nullptr;
            if (s->component->activateBus(kAudio, direction, 0, true) != kResultOk) return nullptr;
            int32 count = s->component->getBusCount(kEvent, direction);
            if (count > 1) return nullptr;
            if (count) {
                if (s->component->activateBus(kEvent, direction, 0, true) != kResultOk) return nullptr;
                if (direction == kInput) s->notes = true;
            }
        }
        if (s->controller) {
            s->controller->setComponentHandler(&s->handler);
            int32 count = s->controller->getParameterCount(); if (count < 0 || count > 4096) return nullptr;
            for (int32 i = 0; i < count; ++i) {
                ParameterInfo info{}; if (s->controller->getParameterInfo(i, info) != kResultOk) return nullptr;
                clap_param_info_t cached{}; cached.id = info.id; cached.min_value = 0; cached.max_value = 1;
                cached.default_value = info.defaultNormalizedValue;
                if (info.flags & ParameterInfo::kIsReadOnly) cached.flags |= CLAP_PARAM_IS_READONLY;
                s->parameters.push_back(cached);
            }
        }
        s->api = {nullptr, s.get(), init, destroy, activate, deactivate, start, stop, nullptr, process, extension, callback};
        auto result = &s->api; s.release(); return result;
    } catch (...) { return nullptr; }
}
} // namespace
extern "C" int ap_live_open_vst3(const char *path, const char *id, const ap_live_config *c, ap_live **s) {
    return ap_live_open_adapter(path, id, c, s, create);
}
