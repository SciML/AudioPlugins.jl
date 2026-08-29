/* ap_test_vst3.cpp -- the VST3 bundle of test plugins, ours, for testing
 * the host. Mirrors the CLAP and LV2 test bundles: a gain, a one-pole and
 * a 16-sample lookahead, so every expectation in the suite is arithmetic
 * rather than a recording. Built against the Steinberg VST3 SDK (>= 3.8,
 * MIT), as a single-component effect each (processor and controller in
 * one object), with no editor.
 *
 *   ap.gain       param 0 "Gain"        plain 0..4, default 1 (normalised = plain/4)
 *   ap.onepole    param 0 "Coefficient" plain 0..1, default 0.5
 *   ap.lookahead  no parameters, reports 16 samples of latency
 *
 * Class ids are fixed so the tests can name them:
 *   gain      41505447-41494e00-00000000-00000001
 *   onepole   41505431-504f4c45-00000000-00000002
 *   lookahead 41504c4f-4f4b4148-00000000-00000003
 */
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "public.sdk/source/vst/vstparameters.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <cstring>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

const int LOOKAHEAD = 16;

/* Common: one stereo-or-mono in, one out; float32 only; the last value of
 * any queued change for a parameter applies to the whole block (the block
 * boundary is where our hosts put changes, so this is exact). */
class ApEffect : public SingleComponentEffect {
public:
    tresult PLUGIN_API initialize(FUnknown *context) SMTG_OVERRIDE {
        tresult r = SingleComponentEffect::initialize(context);
        if (r != kResultOk) return r;
        addAudioInput(STR16("In"), SpeakerArr::kStereo);
        addAudioOutput(STR16("Out"), SpeakerArr::kStereo);
        addParams();
        return kResultOk;
    }
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement *inputs, int32 numIns,
                                          SpeakerArrangement *outputs, int32 numOuts) SMTG_OVERRIDE {
        if (numIns != 1 || numOuts != 1) return kResultFalse;
        if (inputs[0] != outputs[0]) return kResultFalse;
        if (inputs[0] != SpeakerArr::kMono && inputs[0] != SpeakerArr::kStereo) return kResultFalse;
        return SingleComponentEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
    }
    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) SMTG_OVERRIDE {
        return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
    }
    tresult PLUGIN_API setActive(TBool state) SMTG_OVERRIDE {
        if (state) reset();
        return SingleComponentEffect::setActive(state);
    }
    tresult PLUGIN_API process(ProcessData &data) SMTG_OVERRIDE {
        if (data.inputParameterChanges) {
            int32 n = data.inputParameterChanges->getParameterCount();
            for (int32 i = 0; i < n; i++) {
                IParamValueQueue *q = data.inputParameterChanges->getParameterData(i);
                if (!q || q->getPointCount() < 1) continue;
                int32 off; ParamValue v;
                if (q->getPoint(q->getPointCount() - 1, off, v) == kResultOk)
                    onParam(q->getParameterId(), v);
            }
        }
        if (data.numInputs < 1 || data.numOutputs < 1 || data.numSamples < 1) return kResultOk;
        AudioBusBuffers &in = data.inputs[0], &out = data.outputs[0];
        int32 nch = out.numChannels < in.numChannels ? out.numChannels : in.numChannels;
        for (int32 c = 0; c < nch; c++)
            run(c, in.channelBuffers32[c], out.channelBuffers32[c], data.numSamples);
        out.silenceFlags = 0;
        return kResultOk;
    }
    tresult PLUGIN_API setState(IBStream *) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API getState(IBStream *) SMTG_OVERRIDE { return kResultOk; }

protected:
    virtual void addParams() {}
    virtual void onParam(ParamID, ParamValue) {}
    virtual void reset() {}
    virtual void run(int32 ch, const float *x, float *y, int32 n) = 0;
};

class ApGain : public ApEffect {
public:
    static FUnknown *createInstance(void *) { return (IAudioProcessor *)new ApGain; }
    static const FUID cid;
protected:
    void addParams() SMTG_OVERRIDE {
        parameters.addParameter(new RangeParameter(STR16("Gain"), 0, STR16(""), 0.0, 4.0, 1.0,
                                                   0, ParameterInfo::kCanAutomate));
    }
    void onParam(ParamID id, ParamValue v) SMTG_OVERRIDE {
        if (id == 0) gain = (float)(4.0 * v);
        setParamNormalized(id, v);
    }
    void run(int32, const float *x, float *y, int32 n) SMTG_OVERRIDE {
        for (int32 i = 0; i < n; i++) y[i] = x[i] * gain;
    }
    float gain = 1.0f;
};

class ApOnePole : public ApEffect {
public:
    static FUnknown *createInstance(void *) { return (IAudioProcessor *)new ApOnePole; }
    static const FUID cid;
protected:
    void addParams() SMTG_OVERRIDE {
        parameters.addParameter(new RangeParameter(STR16("Coefficient"), 0, STR16(""), 0.0, 1.0, 0.5,
                                                   0, ParameterInfo::kCanAutomate));
    }
    void onParam(ParamID id, ParamValue v) SMTG_OVERRIDE {
        if (id == 0) a = (float)v;
        setParamNormalized(id, v);
    }
    void reset() SMTG_OVERRIDE { state[0] = state[1] = 0.0f; }
    /* y[n] = y[n-1] + a * (x[n] - y[n-1]); step response 1 - (1-a)^n */
    void run(int32 ch, const float *x, float *y, int32 n) SMTG_OVERRIDE {
        float s = state[ch & 1];
        for (int32 i = 0; i < n; i++) { s += a * (x[i] - s); y[i] = s; }
        state[ch & 1] = s;
    }
    float a = 0.5f;
    float state[2] = {0, 0};
};

class ApLookahead : public ApEffect {
public:
    static FUnknown *createInstance(void *) { return (IAudioProcessor *)new ApLookahead; }
    static const FUID cid;
    uint32 PLUGIN_API getLatencySamples() SMTG_OVERRIDE { return LOOKAHEAD; }
protected:
    void reset() SMTG_OVERRIDE { memset(delay, 0, sizeof delay); pos[0] = pos[1] = 0; }
    void run(int32 ch, const float *x, float *y, int32 n) SMTG_OVERRIDE {
        float *d = delay[ch & 1]; int &p = pos[ch & 1];
        for (int32 i = 0; i < n; i++) {
            float v = x[i];
            y[i] = d[p];
            d[p] = v;
            p = (p + 1) % LOOKAHEAD;
        }
    }
    float delay[2][LOOKAHEAD] = {{0}};
    int pos[2] = {0, 0};
};

const FUID ApGain::cid     (0x41505447, 0x41494e00, 0x00000000, 0x00000001);
const FUID ApOnePole::cid  (0x41505431, 0x504f4c45, 0x00000000, 0x00000002);
const FUID ApLookahead::cid(0x41504c4f, 0x4f4b4148, 0x00000000, 0x00000003);

} // namespace

BEGIN_FACTORY_DEF("AudioPlugins.jl", "https://github.com/SciML/AudioPlugins.jl", "mailto:noreply@example.invalid")
    DEF_CLASS2(INLINE_UID_FROM_FUID(ApGain::cid), PClassInfo::kManyInstances, kVstAudioEffectClass,
               "AudioPlugins Test Gain", Vst::kDistributable, "Fx", "1.0.0", kVstVersionString,
               ApGain::createInstance)
    DEF_CLASS2(INLINE_UID_FROM_FUID(ApOnePole::cid), PClassInfo::kManyInstances, kVstAudioEffectClass,
               "AudioPlugins Test One-Pole", Vst::kDistributable, "Fx|Filter", "1.0.0", kVstVersionString,
               ApOnePole::createInstance)
    DEF_CLASS2(INLINE_UID_FROM_FUID(ApLookahead::cid), PClassInfo::kManyInstances, kVstAudioEffectClass,
               "AudioPlugins Test Lookahead", Vst::kDistributable, "Fx|Delay", "1.0.0", kVstVersionString,
               ApLookahead::createInstance)
END_FACTORY

/* The module entry points (ModuleEntry / ModuleExit) come from the SDK's
 * per-platform main file, compiled alongside this one:
 *   public.sdk/source/main/linuxmain.cpp | macmain.cpp | dllmain.cpp */
