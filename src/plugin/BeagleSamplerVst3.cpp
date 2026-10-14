#include "../core/RollingAudioBuffer.h"
#include "../core/SettingsManager.h"
#include "../ui/WaveformEditor.h"

#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/gui/iplugview.h>

#include <windows.h>
#include <atomic>
#include <cstring>
#include <algorithm>

using namespace Steinberg;
using namespace Steinberg::Vst;
using namespace RollingSampler;

namespace Steinberg {
namespace Vst {
DEF_CLASS_IID (IComponent)
DEF_CLASS_IID (IAudioProcessor)
DEF_CLASS_IID (IEditController)
}
DEF_CLASS_IID (IPlugView)
}

// Unique Class ID for BeagleSampler: {B3A61E01-54C7-4D92-82A1-B1C2D3E4F5A6}
static const FUID kBeagleSamplerProcessorUID(0xB3A61E01, 0x54C74D92, 0x82A1B1C2, 0xD3E4F5A6);

class BeagleSamplerProcessor;

class BeagleSamplerPlugView : public IPlugView {
public:
    BeagleSamplerPlugView(BeagleSamplerProcessor* processor);
    virtual ~BeagleSamplerPlugView();

    // FUnknown
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override;
    uint32 PLUGIN_API addRef() override { return ++m_refCount; }
    uint32 PLUGIN_API release() override {
        uint32 r = --m_refCount;
        if (r == 0) delete this;
        return r;
    }

    // IPlugView
    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {
        if (strcmp(type, kPlatformTypeHWND) == 0) return kResultTrue;
        return kResultFalse;
    }

    tresult PLUGIN_API attached(void* parent, FIDString type) override;
    tresult PLUGIN_API removed() override;
    tresult PLUGIN_API onWheel(float) override { return kResultOk; }
    tresult PLUGIN_API onKeyDown(char16, int16, int16) override { return kResultOk; }
    tresult PLUGIN_API onKeyUp(char16, int16, int16) override { return kResultOk; }
    tresult PLUGIN_API getSize(ViewRect* size) override;
    tresult PLUGIN_API onSize(ViewRect* newSize) override;
    tresult PLUGIN_API onFocus(TBool) override { return kResultOk; }
    tresult PLUGIN_API setFrame(IPlugFrame*) override { return kResultOk; }
    tresult PLUGIN_API canResize() override { return kResultTrue; }
    tresult PLUGIN_API checkSizeConstraint(ViewRect* rect) override;

private:
    std::atomic<uint32> m_refCount{ 1 };
    BeagleSamplerProcessor* m_processor = nullptr;
    WaveformEditor* m_editor = nullptr;
};

class BeagleSamplerProcessor : public IComponent, public IAudioProcessor, public IEditController {
public:
    BeagleSamplerProcessor() : m_buffer(48000, 30.0) {
        OleInitialize(NULL);
    }
    virtual ~BeagleSamplerProcessor() {}

    RollingAudioBuffer* getBuffer() { return &m_buffer; }

    // FUnknown
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(_iid, FUnknown::iid) ||
            FUnknownPrivate::iidEqual(_iid, IPluginBase::iid) ||
            FUnknownPrivate::iidEqual(_iid, IComponent::iid)) {
            *obj = static_cast<IComponent*>(this);
            addRef();
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(_iid, IAudioProcessor::iid)) {
            *obj = static_cast<IAudioProcessor*>(this);
            addRef();
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(_iid, IEditController::iid)) {
            *obj = static_cast<IEditController*>(this);
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() override { return ++m_refCount; }
    uint32 PLUGIN_API release() override {
        uint32 r = --m_refCount;
        if (r == 0) delete this;
        return r;
    }

    // IPluginBase
    tresult PLUGIN_API initialize(FUnknown*) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { return kResultOk; }

    // IComponent
    tresult PLUGIN_API getControllerClassId(TUID) override { return kResultFalse; }
    tresult PLUGIN_API setIoMode(IoMode) override { return kResultOk; }

    int32 PLUGIN_API getBusCount(MediaType type, BusDirection) override {
        return (type == kAudio) ? 1 : 0;
    }

    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) override {
        if (type != kAudio || index != 0) return kResultFalse;
        bus.mediaType = kAudio;
        bus.direction = dir;
        bus.channelCount = 2;
        const char* src = (dir == kInput) ? "Stereo In" : "Stereo Out";
        size_t len = strlen(src);
        for (size_t i = 0; i < 127 && i < len; ++i) bus.name[i] = static_cast<TChar>(src[i]);
        bus.name[std::min<size_t>(127, len)] = 0;
        bus.busType = kMain;
        bus.flags = BusInfo::kDefaultActive;
        return kResultOk;
    }

    tresult PLUGIN_API getRoutingInfo(RoutingInfo&, RoutingInfo&) override { return kResultFalse; }
    tresult PLUGIN_API activateBus(MediaType, BusDirection, int32, TBool) override { return kResultOk; }
    tresult PLUGIN_API setActive(TBool) override { return kResultOk; }

    tresult PLUGIN_API setState(IBStream* state) override {
        if (!state) return kResultFalse;
        double dur = 30.0;
        int32 bytesRead = 0;
        if (state->read(&dur, sizeof(dur), &bytesRead) == kResultOk && bytesRead == sizeof(dur)) {
            if (dur >= 5.0 && dur <= 600.0) m_buffer.setDuration(dur, m_buffer.getSampleRate());
            return kResultOk;
        }
        return kResultFalse;
    }

    tresult PLUGIN_API getState(IBStream* state) override {
        if (!state) return kResultFalse;
        double dur = m_buffer.getDuration();
        int32 bytesWritten = 0;
        if (state->write(&dur, sizeof(dur), &bytesWritten) == kResultOk && bytesWritten == sizeof(dur)) {
            return kResultOk;
        }
        return kResultFalse;
    }

    // IAudioProcessor
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* inputs, int32 numIns,
                                          SpeakerArrangement* outputs, int32 numOuts) override {
        if (numIns == 1 && numOuts == 1) {
            if (inputs[0] == SpeakerArr::kStereo && outputs[0] == SpeakerArr::kStereo) {
                return kResultOk;
            }
        }
        return kResultFalse;
    }

    tresult PLUGIN_API getBusArrangement(BusDirection, int32 index, SpeakerArrangement& arr) override {
        if (index == 0) {
            arr = SpeakerArr::kStereo;
            return kResultOk;
        }
        return kResultFalse;
    }

    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) override {
        return (symbolicSampleSize == kSample32) ? kResultTrue : kResultFalse;
    }

    uint32 PLUGIN_API getLatencySamples() override { return 0; }

    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override {
        m_buffer.setDuration(m_buffer.getDuration(), static_cast<uint32_t>(setup.sampleRate));
        return kResultOk;
    }

    tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }

    tresult PLUGIN_API process(ProcessData& data) override {
        if (data.numInputs < 1 || data.numOutputs < 1 || data.numSamples == 0) {
            return kResultOk;
        }

        AudioBusBuffers& in = data.inputs[0];
        AudioBusBuffers& out = data.outputs[0];

        // 1. Transparent audio pass-through to output bus
        for (int32 ch = 0; ch < out.numChannels && ch < in.numChannels; ++ch) {
            if (out.channelBuffers32[ch] != in.channelBuffers32[ch]) {
                memcpy(out.channelBuffers32[ch], in.channelBuffers32[ch], data.numSamples * sizeof(float));
            }
        }

        // 2. Continuous rolling buffer ingestion
        const float* inL = (in.numChannels > 0) ? in.channelBuffers32[0] : nullptr;
        const float* inR = (in.numChannels > 1) ? in.channelBuffers32[1] : inL;
        if (inL) {
            const float* channels[2] = { inL, inR };
            m_buffer.writeSamples(channels, (in.numChannels > 1) ? 2 : 1, data.numSamples);
        }

        return kResultOk;
    }

    uint32 PLUGIN_API getTailSamples() override { return kNoTail; }

    // IEditController
    tresult PLUGIN_API setComponentState(IBStream* state) override { return setState(state); }
    int32 PLUGIN_API getParameterCount() override { return 0; }
    tresult PLUGIN_API getParameterInfo(int32, ParameterInfo&) override { return kResultFalse; }
    tresult PLUGIN_API getParamStringByValue(ParamID, ParamValue, String128) override { return kResultFalse; }
    tresult PLUGIN_API getParamValueByString(ParamID, TChar*, ParamValue&) override { return kResultFalse; }
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID, ParamValue val) override { return val; }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID, ParamValue val) override { return val; }
    ParamValue PLUGIN_API getParamNormalized(ParamID) override { return 0.0; }
    tresult PLUGIN_API setParamNormalized(ParamID, ParamValue) override { return kResultOk; }
    tresult PLUGIN_API setComponentHandler(IComponentHandler*) override { return kResultOk; }

    IPlugView* PLUGIN_API createView(FIDString name) override {
        if (name && strcmp(name, ViewType::kEditor) == 0) {
            return new BeagleSamplerPlugView(this);
        }
        return nullptr;
    }

private:
    std::atomic<uint32> m_refCount{ 1 };
    RollingAudioBuffer m_buffer;
};

BeagleSamplerPlugView::BeagleSamplerPlugView(BeagleSamplerProcessor* processor)
    : m_processor(processor) {
    if (m_processor) m_processor->addRef();
}

BeagleSamplerPlugView::~BeagleSamplerPlugView() {
    removed();
    if (m_processor) {
        m_processor->release();
        m_processor = nullptr;
    }
}

tresult PLUGIN_API BeagleSamplerPlugView::queryInterface(const TUID _iid, void** obj) {
    if (FUnknownPrivate::iidEqual(_iid, FUnknown::iid) ||
        FUnknownPrivate::iidEqual(_iid, IPlugView::iid)) {
        *obj = static_cast<IPlugView*>(this);
        addRef();
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

tresult PLUGIN_API BeagleSamplerPlugView::attached(void* parent, FIDString type) {
    if (strcmp(type, kPlatformTypeHWND) != 0 || !parent) return kResultFalse;
    if (!m_editor && m_processor) {
        m_editor = new WaveformEditor(m_processor->getBuffer(), nullptr, /*isChild=*/true);
        m_editor->create((HWND)parent, GetModuleHandleW(NULL), 0, 0, 1223, 293);
        return kResultOk;
    }
    return kResultFalse;
}

tresult PLUGIN_API BeagleSamplerPlugView::removed() {
    if (m_editor) {
        delete m_editor;
        m_editor = nullptr;
    }
    return kResultOk;
}

tresult PLUGIN_API BeagleSamplerPlugView::getSize(ViewRect* size) {
    if (!size) return kResultFalse;
    size->left = 0;
    size->top = 0;
    size->right = 1223;
    size->bottom = 293;
    return kResultOk;
}

tresult PLUGIN_API BeagleSamplerPlugView::onSize(ViewRect* newSize) {
    if (m_editor && newSize) {
        m_editor->setSize(newSize->getWidth(), newSize->getHeight());
        return kResultOk;
    }
    return kResultFalse;
}

tresult PLUGIN_API BeagleSamplerPlugView::checkSizeConstraint(ViewRect* rect) {
    if (!rect) return kResultFalse;
    int32 w = std::clamp(rect->getWidth(), 420, 3840);
    int32 h = std::clamp(rect->getHeight(), 140, 2160);
    rect->right = rect->left + w;
    rect->bottom = rect->top + h;
    return kResultOk;
}

// Steinberg Plugin Factory
class BeagleSamplerFactory : public IPluginFactory2 {
public:
    BeagleSamplerFactory() {}
    virtual ~BeagleSamplerFactory() {}

    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(_iid, FUnknown::iid) ||
            FUnknownPrivate::iidEqual(_iid, IPluginFactory::iid) ||
            FUnknownPrivate::iidEqual(_iid, IPluginFactory2::iid)) {
            *obj = static_cast<IPluginFactory2*>(this);
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() override { return ++m_refCount; }
    uint32 PLUGIN_API release() override {
        uint32 r = --m_refCount;
        if (r == 0) delete this;
        return r;
    }

    tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override {
        if (!info) return kResultFalse;
        strncpy(info->vendor, "BeagleBoss", sizeof(info->vendor));
        strncpy(info->url, "https://github.com/beagleboss/beaglesampler", sizeof(info->url));
        strncpy(info->email, "info@beagleboss.com", sizeof(info->email));
        info->flags = PFactoryInfo::kNoFlags;
        return kResultOk;
    }

    int32 PLUGIN_API countClasses() override { return 1; }

    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override {
        if (index != 0 || !info) return kResultFalse;
        memcpy(info->cid, kBeagleSamplerProcessorUID.toTUID(), sizeof(TUID));
        info->cardinality = PClassInfo::kManyInstances;
        strncpy(info->category, kVstAudioEffectClass, sizeof(info->category));
        strncpy(info->name, "BeagleSampler", sizeof(info->name));
        return kResultOk;
    }

    tresult PLUGIN_API createInstance(FIDString cid, FIDString _iid, void** obj) override {
        if (FUID::fromTUID(cid) == kBeagleSamplerProcessorUID) {
            auto* proc = new BeagleSamplerProcessor();
            if (proc->queryInterface(_iid, obj) == kResultOk) {
                return kResultOk;
            }
            delete proc;
        }
        *obj = nullptr;
        return kNoInterface;
    }

    tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2* info) override {
        if (index != 0 || !info) return kResultFalse;
        memcpy(info->cid, kBeagleSamplerProcessorUID.toTUID(), sizeof(TUID));
        info->cardinality = PClassInfo::kManyInstances;
        strncpy(info->category, kVstAudioEffectClass, sizeof(info->category));
        strncpy(info->name, "BeagleSampler", sizeof(info->name));
        info->classFlags = 0;
        strncpy(info->subCategories, "Fx|Sampler|Utility", sizeof(info->subCategories));
        strncpy(info->vendor, "BeagleBoss", sizeof(info->vendor));
        strncpy(info->version, "1.0.15", sizeof(info->version));
        strncpy(info->sdkVersion, kVstVersionString, sizeof(info->sdkVersion));
        return kResultOk;
    }

private:
    std::atomic<uint32> m_refCount{ 1 };
};

static BeagleSamplerFactory* s_factoryInstance = nullptr;

extern "C" {

SMTG_EXPORT_SYMBOL bool InitDll() { return true; }
SMTG_EXPORT_SYMBOL bool ExitDll() {
    if (s_factoryInstance) {
        s_factoryInstance->release();
        s_factoryInstance = nullptr;
    }
    return true;
}

SMTG_EXPORT_SYMBOL IPluginFactory* PLUGIN_API GetPluginFactory() {
    if (!s_factoryInstance) {
        s_factoryInstance = new BeagleSamplerFactory();
    } else {
        s_factoryInstance->addRef();
    }
    return s_factoryInstance;
}

} // extern "C"
