#pragma once

#include <vector>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <chrono>
#include "WaveformCache.h"

namespace RollingSampler {

class RollingAudioBuffer {
public:
    RollingAudioBuffer(uint32_t sampleRate = 44100, double durationSeconds = 30.0);
    ~RollingAudioBuffer() = default;

    // Core audio thread ingestion
    void writeSamples(const float* const* channelData, size_t numChannels, size_t numSamples);

    // Read methods for UI/Export/Preview
    // Reads up to count recent samples in chronological order (oldest to newest)
    size_t readRecentSamples(float* destLeft, float* destRight, size_t count) const;

    // Reads absolute sample range from the circular buffer
    void readRange(size_t startSample, size_t count, float* destLeft, float* destRight) const;

    // Configuration
    void setDuration(double durationSeconds, uint32_t sampleRate);
    double getDuration() const;
    uint32_t getSampleRate() const;
    size_t getCapacity() const;

    // State
    void setPaused(bool paused);
    bool isPaused() const;

    void clear();

    uint32_t getWriteHead() const;
    uint64_t getEpoch() const;
    uint64_t getTotalSamplesWritten() const;

    // Peak levels for metering
    float getPeakLeft() const;
    float getPeakRight() const;
    void resetPeaks();

    // Waveform Decimation Cache
    const WaveformCache& getWaveformCache() const { return waveformCache; }
    WaveformCache& getWaveformCache() { return waveformCache; }

    // High-fidelity smooth rolling waveform extraction (zero-flicker sub-sample envelope)
    void getSmoothRollingEnvelope(size_t numPixels, double displayGain, float* outMax, float* outMin) const;

    // High-fidelity sweep waveform extraction (traveling write-head envelope with leading erase gap and smooth easing fade)
    void getBufferSweepEnvelope(size_t numPixels, double displayGain, float* outMax, float* outMin, size_t eraseGapPixels = 0, size_t fadePixels = 0, double headFrac = -1.0) const;

    // Sub-sample time-interpolated write head fraction [0.0, 1.0) for silky 60+ FPS motion
    double getInterpolatedWriteHeadFrac() const;

    // Selection reading with optional silence trimming
    size_t getSelectionSampleCount(double startFrac, double endFrac) const;
    size_t readSelectionSamples(float* destLeft, float* destRight, double startFrac, double endFrac,
                                bool trimSilence = false, float silenceThresholdDb = -90.0f) const;
    size_t readSweepSelectionSamples(float* destLeft, float* destRight, double startFrac, double endFrac,
                                     bool trimSilence = false, float silenceThresholdDb = -90.0f) const;

    // Buffer State Persistence (Save States / Session Recovery)
    bool saveStateToFile(const std::wstring& filePath) const;
    bool loadStateFromFile(const std::wstring& filePath);

    static constexpr size_t PEAK_BLOCK_SIZE = 64;

private:
    void reallocate(size_t newCapacity);

    mutable std::mutex resizeMutex;

    uint32_t sampleRate{44100};
    double durationSeconds{30.0};
    size_t capacity{1323000}; // 44100 * 30

    std::vector<float> bufferLeft;
    std::vector<float> bufferRight;

    std::vector<float> blockMax;
    std::vector<float> blockMin;

    WaveformCache waveformCache;

    std::atomic<uint32_t> writeHead{0};
    std::atomic<uint64_t> epoch{0};
    std::atomic<uint64_t> totalSamplesWritten{0};
    std::atomic<int64_t> lastWriteTimeNs{0};
    std::atomic<bool> paused{false};

    std::atomic<float> peakLeft{0.0f};
    std::atomic<float> peakRight{0.0f};
};

} // namespace RollingSampler
