#include <clap/clap.h>
#include "../core/RollingAudioBuffer.h"
#include "../core/SettingsManager.h"
#include "../ui/WaveformEditor.h"
#include <windows.h>
#include <cstring>
#include <algorithm>

using namespace RollingSampler;

static const char* s_clapPluginFeatures[] = {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
    CLAP_PLUGIN_FEATURE_UTILITY,
    CLAP_PLUGIN_FEATURE_SAMPLER,
    nullptr
};

static const clap_plugin_descriptor_t s_clapDesc = {
    CLAP_VERSION_INIT,
    "com.beagleboss.beaglesampler",
    "BeagleSampler",
    "BeagleBoss",
    "https://github.com/beagleboss/beaglesampler",
    "",
    "",
    "1.0.15",
    "Continuous rolling audio capture and instant drag-and-drop sampler",
    s_clapPluginFeatures
};

class BeagleSamplerClap {
public:
    BeagleSamplerClap(const clap_host_t* host)
        : m_host(host), m_buffer(48000, 30.0) {
        m_plugin.desc = &s_clapDesc;
        m_plugin.plugin_data = this;
        m_plugin.init = clapInit;
        m_plugin.destroy = clapDestroy;
        m_plugin.activate = clapActivate;
        m_plugin.deactivate = clapDeactivate;
        m_plugin.start_processing = clapStartProcessing;
        m_plugin.stop_processing = clapStopProcessing;
        m_plugin.reset = clapReset;
        m_plugin.process = clapProcess;
        m_plugin.get_extension = clapGetExtension;
        m_plugin.on_main_thread = clapOnMainThread;

        // OLE initialization for drag & drop
        OleInitialize(NULL);
    }

    ~BeagleSamplerClap() {
        if (m_editor) {
            delete m_editor;
            m_editor = nullptr;
        }
    }

    const clap_plugin_t* getPlugin() const { return &m_plugin; }

private:
    static bool CLAP_ABI clapInit(const clap_plugin_t*) { return true; }

    static void CLAP_ABI clapDestroy(const clap_plugin_t* plugin) {
        auto* self = static_cast<BeagleSamplerClap*>(plugin->plugin_data);
        delete self;
    }

    static bool CLAP_ABI clapActivate(const clap_plugin_t* plugin, double sample_rate, uint32_t, uint32_t) {
        auto* self = static_cast<BeagleSamplerClap*>(plugin->plugin_data);
        self->m_buffer.setDuration(self->m_buffer.getDuration(), static_cast<uint32_t>(sample_rate));
        return true;
    }

    static void CLAP_ABI clapDeactivate(const clap_plugin_t*) {}
    static bool CLAP_ABI clapStartProcessing(const clap_plugin_t*) { return true; }
    static void CLAP_ABI clapStopProcessing(const clap_plugin_t*) {}

    static void CLAP_ABI clapReset(const clap_plugin_t* plugin) {
        auto* self = static_cast<BeagleSamplerClap*>(plugin->plugin_data);
        self->m_buffer.clear();
    }

    static clap_process_status CLAP_ABI clapProcess(const clap_plugin_t* plugin, const clap_process_t* process) {
        auto* self = static_cast<BeagleSamplerClap*>(plugin->plugin_data);
        if (!process) return CLAP_PROCESS_CONTINUE;

        uint32_t frames = process->frames_count;
        if (frames == 0) return CLAP_PROCESS_CONTINUE;

        // Audio pass-through & rolling buffer ingestion
        if (process->audio_inputs_count > 0 && process->audio_outputs_count > 0) {
            const clap_audio_buffer_t* in = &process->audio_inputs[0];
            clap_audio_buffer_t* out = &process->audio_outputs[0];

            // 1. Pass through input directly to output without coloration
            for (uint32_t c = 0; c < out->channel_count && c < in->channel_count; ++c) {
                if (out->data32[c] != in->data32[c]) {
                    memcpy(out->data32[c], in->data32[c], frames * sizeof(float));
                }
            }

            // 2. Ingest into rolling buffer
            const float* inL = (in->channel_count > 0) ? in->data32[0] : nullptr;
            const float* inR = (in->channel_count > 1) ? in->data32[1] : inL;
            if (inL) {
                const float* channels[2] = { inL, inR };
                self->m_buffer.writeSamples(channels, inR ? 2 : 1, frames);
            }
        }
        return CLAP_PROCESS_CONTINUE;
    }

    static const void* CLAP_ABI clapGetExtension(const clap_plugin_t*, const char* id) {
        if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &s_audioPorts;
        if (strcmp(id, CLAP_EXT_GUI) == 0) return &s_gui;
        if (strcmp(id, CLAP_EXT_STATE) == 0) return &s_state;
        if (strcmp(id, CLAP_EXT_LATENCY) == 0) return &s_latency;
        return nullptr;
    }

    static void CLAP_ABI clapOnMainThread(const clap_plugin_t*) {}

    // Audio Ports Extension
    static uint32_t CLAP_ABI audioPortsCount(const clap_plugin_t*, bool) { return 1; }

    static bool CLAP_ABI audioPortsGet(const clap_plugin_t*, uint32_t index, bool is_input, clap_audio_port_info_t* info) {
        if (index != 0 || !info) return false;
        info->id = is_input ? 0 : 1;
        strncpy(info->name, is_input ? "Main Input" : "Main Output", sizeof(info->name) - 1);
        info->flags = CLAP_AUDIO_PORT_IS_MAIN | CLAP_AUDIO_PORT_SUPPORTS_64BITS;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = is_input ? 1 : 0;
        return true;
    }

    // GUI Extension
    static bool CLAP_ABI guiIsApiSupported(const clap_plugin_t*, const char* api, bool is_floating) {
        return (strcmp(api, CLAP_WINDOW_API_WIN32) == 0) && !is_floating;
    }

    static bool CLAP_ABI guiGetPreferredApi(const clap_plugin_t*, const char** api, bool* is_floating) {
        *api = CLAP_WINDOW_API_WIN32;
        *is_floating = false;
        return true;
    }

    static bool CLAP_ABI guiCreate(const clap_plugin_t* plugin, const char* api, bool is_floating) {
        if (strcmp(api, CLAP_WINDOW_API_WIN32) != 0 || is_floating) return false;
        auto* self = static_cast<BeagleSamplerClap*>(plugin->plugin_data);
        if (!self->m_editor) {
            self->m_editor = new WaveformEditor(&self->m_buffer, nullptr, /*isChild=*/true);
        }
        return true;
    }

    static void CLAP_ABI guiDestroy(const clap_plugin_t* plugin) {
        auto* self = static_cast<BeagleSamplerClap*>(plugin->plugin_data);
        if (self->m_editor) {
            delete self->m_editor;
            self->m_editor = nullptr;
        }
    }

    static bool CLAP_ABI guiSetScale(const clap_plugin_t*, double) { return false; }

    static bool CLAP_ABI guiGetSize(const clap_plugin_t*, uint32_t* width, uint32_t* height) {
        *width = 1223;
        *height = 293;
        return true;
    }

    static bool CLAP_ABI guiCanResize(const clap_plugin_t*) { return true; }

    static bool CLAP_ABI guiGetResizeHints(const clap_plugin_t*, clap_gui_resize_hints_t* hints) {
        hints->can_resize_horizontally = true;
        hints->can_resize_vertically = true;
        hints->preserve_aspect_ratio = false;
        hints->aspect_ratio_width = 1223;
        hints->aspect_ratio_height = 293;
        return true;
    }

    static bool CLAP_ABI guiAdjustSize(const clap_plugin_t*, uint32_t* width, uint32_t* height) {
        *width = std::clamp(*width, 420U, 3840U);
        *height = std::clamp(*height, 140U, 2160U);
        return true;
    }

    static bool CLAP_ABI guiSetSize(const clap_plugin_t* plugin, uint32_t width, uint32_t height) {
        auto* self = static_cast<BeagleSamplerClap*>(plugin->plugin_data);
        if (self->m_editor) {
            self->m_editor->setSize(width, height);
            return true;
        }
        return false;
    }

    static bool CLAP_ABI guiSetParent(const clap_plugin_t* plugin, const clap_window_t* window) {
        if (!window || strcmp(window->api, CLAP_WINDOW_API_WIN32) != 0) return false;
        auto* self = static_cast<BeagleSamplerClap*>(plugin->plugin_data);
        if (self->m_editor) {
            HWND hParent = static_cast<HWND>(window->win32);
            self->m_editor->create(hParent, GetModuleHandleW(NULL), 0, 0, 1223, 293);
            return true;
        }
        return false;
    }

    static bool CLAP_ABI guiSetTransient(const clap_plugin_t*, const clap_window_t*) { return false; }
    static void CLAP_ABI guiSuggestTitle(const clap_plugin_t*, const char*) {}

    static bool CLAP_ABI guiShow(const clap_plugin_t* plugin) {
        auto* self = static_cast<BeagleSamplerClap*>(plugin->plugin_data);
        if (self->m_editor && self->m_editor->getHwnd()) {
            ShowWindow(self->m_editor->getHwnd(), SW_SHOW);
            return true;
        }
        return false;
    }

    static bool CLAP_ABI guiHide(const clap_plugin_t* plugin) {
        auto* self = static_cast<BeagleSamplerClap*>(plugin->plugin_data);
        if (self->m_editor && self->m_editor->getHwnd()) {
            ShowWindow(self->m_editor->getHwnd(), SW_HIDE);
            return true;
        }
        return false;
    }

    // State Extension
    static bool CLAP_ABI stateSave(const clap_plugin_t* plugin, const clap_ostream_t* stream) {
        auto* self = static_cast<BeagleSamplerClap*>(plugin->plugin_data);
        if (!stream) return false;
        double dur = self->m_buffer.getDuration();
        int64_t written = stream->write(stream, &dur, sizeof(dur));
        return written == sizeof(dur);
    }

    static bool CLAP_ABI stateLoad(const clap_plugin_t* plugin, const clap_istream_t* stream) {
        auto* self = static_cast<BeagleSamplerClap*>(plugin->plugin_data);
        if (!stream) return false;
        double dur = 30.0;
        int64_t bytesRead = stream->read(stream, &dur, sizeof(dur));
        if (bytesRead == sizeof(dur) && dur >= 5.0 && dur <= 600.0) {
            self->m_buffer.setDuration(dur, self->m_buffer.getSampleRate());
            return true;
        }
        return false;
    }

    // Latency Extension (0 latency pass-through)
    static uint32_t CLAP_ABI latencyGet(const clap_plugin_t*) { return 0; }

    clap_plugin_t m_plugin = {};
    const clap_host_t* m_host = nullptr;
    RollingAudioBuffer m_buffer;
    WaveformEditor* m_editor = nullptr;

    static const clap_plugin_audio_ports_t s_audioPorts;
    static const clap_plugin_gui_t s_gui;
    static const clap_plugin_state_t s_state;
    static const clap_plugin_latency_t s_latency;
};

const clap_plugin_audio_ports_t BeagleSamplerClap::s_audioPorts = {
    audioPortsCount,
    audioPortsGet
};

const clap_plugin_gui_t BeagleSamplerClap::s_gui = {
    guiIsApiSupported,
    guiGetPreferredApi,
    guiCreate,
    guiDestroy,
    guiSetScale,
    guiGetSize,
    guiCanResize,
    guiGetResizeHints,
    guiAdjustSize,
    guiSetSize,
    guiSetParent,
    guiSetTransient,
    guiSuggestTitle,
    guiShow,
    guiHide
};

const clap_plugin_state_t BeagleSamplerClap::s_state = {
    stateSave,
    stateLoad
};

const clap_plugin_latency_t BeagleSamplerClap::s_latency = {
    latencyGet
};

// Plugin Factory
static uint32_t CLAP_ABI factoryGetPluginCount(const clap_plugin_factory_t*) { return 1; }

static const clap_plugin_descriptor_t* CLAP_ABI factoryGetPluginDescriptor(const clap_plugin_factory_t*, uint32_t index) {
    return (index == 0) ? &s_clapDesc : nullptr;
}

static const clap_plugin_t* CLAP_ABI factoryCreatePlugin(const clap_plugin_factory_t*, const clap_host_t* host, const char* plugin_id) {
    if (strcmp(plugin_id, "com.beagleboss.beaglesampler") == 0) {
        auto* inst = new BeagleSamplerClap(host);
        return inst->getPlugin();
    }
    return nullptr;
}

static const clap_plugin_factory_t s_factory = {
    factoryGetPluginCount,
    factoryGetPluginDescriptor,
    factoryCreatePlugin
};

// Entry point
static bool CLAP_ABI clapEntryInit(const char*) { return true; }
static void CLAP_ABI clapEntryDeinit() {}

static const void* CLAP_ABI clapEntryGetFactory(const char* factory_id) {
    if (strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0) {
        return &s_factory;
    }
    return nullptr;
}

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    clapEntryInit,
    clapEntryDeinit,
    clapEntryGetFactory
};
