#pragma once

#include "../core/RollingAudioBuffer.h"
#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include <mutex>

namespace RollingSampler {

struct AudioDeviceInfo {
    std::wstring id;
    std::wstring name;
    bool isDefault{false};
    bool isLoopback{false}; // true if this is an output (speakers/desktop) device captured via loopback
};

class AudioCapture {
public:
    AudioCapture(RollingAudioBuffer& buffer);
    ~AudioCapture();

    bool start();
    void stop();
    bool isRunning() const;

    std::vector<AudioDeviceInfo> getAvailableDevices();
    bool setDevice(const std::wstring& deviceId, bool isLoopback = false);

    std::wstring getDeviceName() const;
    std::wstring getSelectedDeviceId() const {
        std::lock_guard<std::mutex> lock(deviceMutex);
        return selectedDeviceId;
    }
    bool isLoopback() const;
    uint32_t getSampleRate() const;       // Internal buffer rate (48000 Hz)
    uint32_t getDeviceSampleRate() const; // Hardware device rate (e.g. 192000 Hz)
    uint32_t getChannels() const;

    static constexpr uint32_t INTERNAL_SAMPLE_RATE = 48000;

private:
    void captureLoop();
    void runGeneratorFallback();

    RollingAudioBuffer& audioBuffer;
    std::atomic<bool> running{false};
    std::thread workerThread;

    std::wstring selectedDeviceId{L"default_loopback"};
    bool selectedIsLoopback{true};
    std::wstring activeDeviceName{L"Desktop Audio"};
    bool activeIsLoopback{true};
    uint32_t deviceSampleRate{48000};
    uint32_t channels{2};
    bool usingFallbackGenerator{false};
    void* hStopEvent{nullptr};
    mutable std::mutex deviceMutex;
};

} // namespace RollingSampler
