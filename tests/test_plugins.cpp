#include <windows.h>
#include <iostream>
#include <vector>
#include <cassert>
#include <cstring>
#include <cmath>

#include <clap/clap.h>
#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>

namespace Steinberg {
namespace Vst {
DEF_CLASS_IID (IComponent)
DEF_CLASS_IID (IAudioProcessor)
}
}

#define PLUGIN_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAILED: " << msg << " (" << #cond << ") at line " << __LINE__ << std::endl; \
            return false; \
        } \
    } while (0)

bool testClapPlugin() {
    std::cout << "[TEST] Testing CLAP Plugin..." << std::endl;
    HMODULE hMod = LoadLibraryW(L"E:\\AI Made Apps\\rolling-sampler-work\\build\\dist\\CLAP\\BeagleSampler.clap");
    PLUGIN_ASSERT(hMod != NULL, "LoadLibrary failed for BeagleSampler.clap");

    auto* entry = reinterpret_cast<const clap_plugin_entry_t*>(GetProcAddress(hMod, "clap_entry"));
    PLUGIN_ASSERT(entry != nullptr, "Could not find 'clap_entry' export");
    PLUGIN_ASSERT(entry->init(""), "clap_entry.init failed");

    auto* factory = reinterpret_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    PLUGIN_ASSERT(factory != nullptr, "Could not get CLAP_PLUGIN_FACTORY");
    PLUGIN_ASSERT(factory->get_plugin_count(factory) == 1, "Plugin count must be 1");

    const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, 0);
    PLUGIN_ASSERT(desc != nullptr, "Descriptor must not be null");
    std::cout << "  CLAP Plugin ID: " << desc->id << " (" << desc->name << " v" << desc->version << ")" << std::endl;
    PLUGIN_ASSERT(strcmp(desc->id, "com.beagleboss.beaglesampler") == 0, "Descriptor ID mismatch");
    PLUGIN_ASSERT(strcmp(desc->name, "BeagleSampler") == 0, "Descriptor name mismatch");

    // Dummy host
    clap_host_t dummyHost = { CLAP_VERSION_INIT, nullptr, "TestHost", "TestVendor", "http://test", "1.0", nullptr, nullptr, nullptr };
    const clap_plugin_t* plugin = factory->create_plugin(factory, &dummyHost, desc->id);
    PLUGIN_ASSERT(plugin != nullptr, "Failed to create CLAP plugin instance");
    PLUGIN_ASSERT(plugin->init(plugin), "Plugin init failed");
    PLUGIN_ASSERT(plugin->activate(plugin, 48000.0, 32, 512), "Plugin activate failed");

    // Test audio processing: 256 samples stereo
    std::vector<float> inL(256, 0.5f);
    std::vector<float> inR(256, -0.5f);
    std::vector<float> outL(256, 0.0f);
    std::vector<float> outR(256, 0.0f);
    float* inBuffers[2] = { inL.data(), inR.data() };
    float* outBuffers[2] = { outL.data(), outR.data() };

    clap_audio_buffer_t inBuf = { inBuffers, nullptr, 2, 0, 0 };
    clap_audio_buffer_t outBuf = { outBuffers, nullptr, 2, 0, 0 };

    clap_process_t proc = {};
    proc.frames_count = 256;
    proc.steady_time = 0;
    proc.audio_inputs = &inBuf;
    proc.audio_inputs_count = 1;
    proc.audio_outputs = &outBuf;
    proc.audio_outputs_count = 1;

    clap_process_status status = plugin->process(plugin, &proc);
    PLUGIN_ASSERT(status == CLAP_PROCESS_CONTINUE, "Process status should be CLAP_PROCESS_CONTINUE");

    // Verify transparent pass-through
    for (size_t i = 0; i < 256; ++i) {
        PLUGIN_ASSERT(std::fabs(outL[i] - 0.5f) < 1e-6f, "Left output sample must match input pass-through");
        PLUGIN_ASSERT(std::fabs(outR[i] - (-0.5f)) < 1e-6f, "Right output sample must match input pass-through");
    }

    // Verify Audio Ports extension
    auto* audioPorts = reinterpret_cast<const clap_plugin_audio_ports_t*>(plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    PLUGIN_ASSERT(audioPorts != nullptr, "Audio ports extension must be present");
    PLUGIN_ASSERT(audioPorts->count(plugin, true) == 1, "Input port count must be 1");
    PLUGIN_ASSERT(audioPorts->count(plugin, false) == 1, "Output port count must be 1");

    // Verify GUI extension
    auto* gui = reinterpret_cast<const clap_plugin_gui_t*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
    PLUGIN_ASSERT(gui != nullptr, "GUI extension must be present");
    PLUGIN_ASSERT(gui->is_api_supported(plugin, CLAP_WINDOW_API_WIN32, false), "Win32 GUI API must be supported");

    // Verify Latency extension
    auto* latency = reinterpret_cast<const clap_plugin_latency_t*>(plugin->get_extension(plugin, CLAP_EXT_LATENCY));
    PLUGIN_ASSERT(latency != nullptr, "Latency extension must be present");
    PLUGIN_ASSERT(latency->get(plugin) == 0, "Plugin must have 0 latency");

    plugin->deactivate(plugin);
    plugin->destroy(plugin);
    entry->deinit();
    FreeLibrary(hMod);
    std::cout << "  [PASS] CLAP Plugin verification succeeded 100%!" << std::endl;
    return true;
}

typedef bool (*InitDllFunc)();
typedef bool (*ExitDllFunc)();
typedef Steinberg::IPluginFactory* (PLUGIN_API *GetPluginFactoryFunc)();

bool testVst3Plugin() {
    std::cout << "[TEST] Testing VST3 Plugin..." << std::endl;
    HMODULE hMod = LoadLibraryW(L"E:\\AI Made Apps\\rolling-sampler-work\\build\\dist\\VST3\\Contents\\x86_64-win\\BeagleSampler.vst3");
    PLUGIN_ASSERT(hMod != NULL, "LoadLibrary failed for BeagleSampler.vst3");

    auto initDll = reinterpret_cast<InitDllFunc>(GetProcAddress(hMod, "InitDll"));
    auto exitDll = reinterpret_cast<ExitDllFunc>(GetProcAddress(hMod, "ExitDll"));
    auto getFactory = reinterpret_cast<GetPluginFactoryFunc>(GetProcAddress(hMod, "GetPluginFactory"));

    PLUGIN_ASSERT(initDll != nullptr, "InitDll export not found");
    PLUGIN_ASSERT(exitDll != nullptr, "ExitDll export not found");
    PLUGIN_ASSERT(getFactory != nullptr, "GetPluginFactory export not found");

    PLUGIN_ASSERT(initDll(), "InitDll returned false");

    Steinberg::IPluginFactory* factory = getFactory();
    PLUGIN_ASSERT(factory != nullptr, "GetPluginFactory returned null");
    PLUGIN_ASSERT(factory->countClasses() == 1, "Class count must be 1");

    Steinberg::PClassInfo classInfo = {};
    PLUGIN_ASSERT(factory->getClassInfo(0, &classInfo) == Steinberg::kResultOk, "getClassInfo failed");
    std::cout << "  VST3 Class: " << classInfo.name << " (" << classInfo.category << ")" << std::endl;
    PLUGIN_ASSERT(strcmp(classInfo.name, "BeagleSampler") == 0, "Class name must be BeagleSampler");
    PLUGIN_ASSERT(strcmp(classInfo.category, kVstAudioEffectClass) == 0, "Category must be Audio Module Class");

    // Create Instance
    void* obj = nullptr;
    Steinberg::tresult res = factory->createInstance(classInfo.cid, Steinberg::Vst::IComponent::iid, &obj);
    PLUGIN_ASSERT(res == Steinberg::kResultOk && obj != nullptr, "createInstance failed for IComponent");

    auto* component = static_cast<Steinberg::Vst::IComponent*>(obj);
    PLUGIN_ASSERT(component->initialize(nullptr) == Steinberg::kResultOk, "component initialize failed");
    PLUGIN_ASSERT(component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kInput) == 1, "Input bus count must be 1");
    PLUGIN_ASSERT(component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput) == 1, "Output bus count must be 1");

    // Query AudioProcessor
    void* procObj = nullptr;
    res = component->queryInterface(Steinberg::Vst::IAudioProcessor::iid, &procObj);
    PLUGIN_ASSERT(res == Steinberg::kResultOk && procObj != nullptr, "queryInterface failed for IAudioProcessor");
    auto* processor = static_cast<Steinberg::Vst::IAudioProcessor*>(procObj);

    Steinberg::Vst::ProcessSetup setup = { Steinberg::Vst::kSample32, Steinberg::Vst::kRealtime, 512, 48000.0 };
    PLUGIN_ASSERT(processor->setupProcessing(setup) == Steinberg::kResultOk, "setupProcessing failed");
    PLUGIN_ASSERT(processor->getLatencySamples() == 0, "Latency must be 0");

    // Test Audio Processing: 256 samples stereo
    std::vector<float> inL(256, 0.75f);
    std::vector<float> inR(256, -0.75f);
    std::vector<float> outL(256, 0.0f);
    std::vector<float> outR(256, 0.0f);
    float* inBuffers[2] = { inL.data(), inR.data() };
    float* outBuffers[2] = { outL.data(), outR.data() };

    Steinberg::Vst::AudioBusBuffers inBus;
    inBus.numChannels = 2;
    inBus.silenceFlags = 0;
    inBus.channelBuffers32 = inBuffers;

    Steinberg::Vst::AudioBusBuffers outBus;
    outBus.numChannels = 2;
    outBus.silenceFlags = 0;
    outBus.channelBuffers32 = outBuffers;

    Steinberg::Vst::ProcessData procData = {};
    procData.numSamples = 256;
    procData.numInputs = 1;
    procData.inputs = &inBus;
    procData.numOutputs = 1;
    procData.outputs = &outBus;

    PLUGIN_ASSERT(processor->process(procData) == Steinberg::kResultOk, "process failed");

    // Verify transparent pass-through
    for (size_t i = 0; i < 256; ++i) {
        PLUGIN_ASSERT(std::fabs(outL[i] - 0.75f) < 1e-6f, "Left output sample must match input pass-through");
        PLUGIN_ASSERT(std::fabs(outR[i] - (-0.75f)) < 1e-6f, "Right output sample must match input pass-through");
    }

    processor->release();
    component->terminate();
    component->release();

    exitDll();
    FreeLibrary(hMod);
    std::cout << "  [PASS] VST3 Plugin verification succeeded 100%!" << std::endl;
    return true;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "RUNNING CLAP & VST3 HOST INTEGRATION TEST" << std::endl;
    std::cout << "========================================" << std::endl;

    if (!testClapPlugin()) return 1;
    if (!testVst3Plugin()) return 2;

    std::cout << "========================================" << std::endl;
    std::cout << "ALL PLUGIN HOST INTEGRATION TESTS PASSED 100%" << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
