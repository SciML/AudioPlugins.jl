/* Live-only fixture: check lifecycle threads and exact event offsets. */
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include <cassert>
#include <thread>
using namespace Steinberg;
using namespace Steinberg::Vst;
class LiveEffect : public SingleComponentEffect {
    std::thread::id control, audio;
    bool active = false, processing = false;
    float gain = 1, gate = 0;
    void main() { assert(std::this_thread::get_id() == control); }
public:
    static FUnknown *create(void *) { return static_cast<IAudioProcessor *>(new LiveEffect); }
    tresult PLUGIN_API initialize(FUnknown *host) override {
        control = std::this_thread::get_id();
        auto result = SingleComponentEffect::initialize(host);
        addAudioInput(STR16("In"), SpeakerArr::kStereo);
        addAudioOutput(STR16("Out"), SpeakerArr::kStereo);
        addEventInput(STR16("Notes"), 16); addEventOutput(STR16("Notes"), 16);
        parameters.addParameter(STR16("Gain"), nullptr, 0, 1, ParameterInfo::kCanAutomate, 0);
        return result;
    }
    tresult PLUGIN_API terminate() override { main(); assert(!active && !processing); return SingleComponentEffect::terminate(); }
    tresult PLUGIN_API setupProcessing(ProcessSetup &setup) override {
        main(); assert(!active && setup.processMode == kRealtime);
        return SingleComponentEffect::setupProcessing(setup);
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override { return size == kSample32 ? kResultOk : kResultFalse; }
    tresult PLUGIN_API setActive(TBool state) override {
        main(); assert(!processing); active = state;
        if (state) gate = 0;
        return SingleComponentEffect::setActive(state);
    }
    tresult PLUGIN_API setProcessing(TBool state) override {
        assert(std::this_thread::get_id() != control && active);
        if (state) { assert(!processing); audio = std::this_thread::get_id(); }
        else assert(processing && audio == std::this_thread::get_id());
        processing = state; return kResultOk;
    }
    uint32 PLUGIN_API getLatencySamples() override { main(); return 0; }
    tresult PLUGIN_API process(ProcessData &data) override {
        assert(active && processing && audio == std::this_thread::get_id());
        assert(data.processMode == kRealtime && data.symbolicSampleSize == kSample32);
        assert(data.numInputs == 1 && data.numOutputs == 1);
        for (int32 frame = 0; frame < data.numSamples; ++frame) {
            for (int32 i = 0; i < data.inputParameterChanges->getParameterCount(); ++i) {
                auto queue = data.inputParameterChanges->getParameterData(i);
                for (int32 j = 0; j < queue->getPointCount(); ++j) {
                    int32 offset; ParamValue value;
                    assert(queue->getPoint(j, offset, value) == kResultOk);
                    if (offset == frame) gain = static_cast<float>(value);
                }
            }
            for (int32 i = 0; i < data.inputEvents->getEventCount(); ++i) {
                Event event{}; assert(data.inputEvents->getEvent(i, event) == kResultOk);
                if (event.sampleOffset != frame) continue;
                if (event.type == Event::kNoteOnEvent) gate = event.noteOn.velocity;
                else if (event.type == Event::kNoteOffEvent) gate = 0;
                else assert(false);
                data.outputEvents->addEvent(event);
            }
            for (int32 ch = 0; ch < data.outputs[0].numChannels; ++ch)
                data.outputs[0].channelBuffers32[ch][frame] = data.inputs[0].channelBuffers32[ch][frame] * gain * gate;
        }
        return kResultOk;
    }
};
static const FUID cid(0x41504C49, 0x56450000, 0x00000000, 0x00000001);
BEGIN_FACTORY_DEF("AudioPlugins", "https://github.com/SciML/AudioPlugins.jl", "mailto:noreply@example.invalid")
    DEF_CLASS2(INLINE_UID_FROM_FUID(cid), PClassInfo::kManyInstances, kVstAudioEffectClass,
        "Live fixture", Vst::kDistributable, "Fx", "1", kVstVersionString, LiveEffect::create)
END_FACTORY
