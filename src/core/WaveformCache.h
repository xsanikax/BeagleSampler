#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>

namespace RollingSampler {

class WaveformCache {
public:
    static constexpr size_t NUM_BINS = 10000;

    WaveformCache() {
        reset();
    }

    void reset() {
        for (size_t i = 0; i < NUM_BINS; ++i) {
            maxPeaks[i].store(0.0f, std::memory_order_relaxed);
            minPeaks[i].store(0.0f, std::memory_order_relaxed);
            binEpoch[i].store(0, std::memory_order_relaxed);
        }
    }

    void rebuild(const float* left, const float* right, size_t cap) {
        reset();
        if (!left || !right || cap == 0) return;
        for (size_t bin = 0; bin < NUM_BINS; ++bin) {
            size_t startIdx = (bin * cap) / NUM_BINS;
            size_t endIdx = ((bin + 1) * cap) / NUM_BINS;
            if (endIdx > cap) endIdx = cap;
            if (startIdx >= endIdx && startIdx < cap) endIdx = startIdx + 1;

            float bMax = 0.0f;
            float bMin = 0.0f;
            for (size_t i = startIdx; i < endIdx; ++i) {
                float sMax = std::max(left[i], right[i]);
                float sMin = std::min(left[i], right[i]);
                if (sMax > bMax) bMax = sMax;
                if (sMin < bMin) bMin = sMin;
            }
            maxPeaks[bin].store(bMax, std::memory_order_relaxed);
            minPeaks[bin].store(bMin, std::memory_order_relaxed);
            binEpoch[bin].store(1, std::memory_order_relaxed);
        }
    }

    // Ingest sample into decimation cache (called from audio ingestion)
    inline void ingestSample(size_t sampleIndex, size_t capacity, float sampleL, float sampleR, uint64_t currentEpoch) {
        if (capacity == 0) return;

        float sMax = std::max(sampleL, sampleR);
        float sMin = std::min(sampleL, sampleR);

        // Bin calculation matching reference: (samplePos / capacity) * (NUM_BINS - 1)
        double normPos = static_cast<double>(sampleIndex % capacity) / static_cast<double>(capacity);
        size_t bin = static_cast<size_t>(std::round(normPos * (NUM_BINS - 1)));
        if (bin >= NUM_BINS) bin = NUM_BINS - 1;

        uint64_t epoch = binEpoch[bin].load(std::memory_order_relaxed);
        if (epoch < currentEpoch) {
            // New wraparound generation: overwrite old bin data
            maxPeaks[bin].store(sMax, std::memory_order_relaxed);
            minPeaks[bin].store(sMin, std::memory_order_relaxed);
            binEpoch[bin].store(currentEpoch, std::memory_order_relaxed);
        } else {
            // Same generation: accumulate min and max
            float curMax = maxPeaks[bin].load(std::memory_order_relaxed);
            if (sMax > curMax) {
                maxPeaks[bin].store(sMax, std::memory_order_relaxed);
            }
            float curMin = minPeaks[bin].load(std::memory_order_relaxed);
            if (sMin < curMin) {
                minPeaks[bin].store(sMin, std::memory_order_relaxed);
            }
        }
    }

    // Extracts rolling waveform envelope for display:
    // Out points: numPixels elements.
    // Recent audio orientation: newest audio at right edge (index numPixels - 1),
    // scrolling smoothly leftwards as writeHead advances.
    void getRollingEnvelope(size_t numPixels, size_t writeHead, size_t capacity,
                            float displayGain, float* outMax, float* outMin) const {
        if (numPixels == 0 || capacity == 0 || !outMax || !outMin) return;

        double writeHeadFraction = static_cast<double>(writeHead % capacity) / static_cast<double>(capacity);
        size_t writeHeadBin = static_cast<size_t>(std::floor(writeHeadFraction * NUM_BINS));

        for (size_t x = 0; x < numPixels; ++x) {
            // Pixel x in [0, numPixels - 1] maps to age:
            // x = numPixels - 1 is writeHead (newest)
            // x = 0 is oldest (writeHead - capacity)
            double xFraction = static_cast<double>(x) / static_cast<double>(numPixels);
            
            // Rolling bin index modulo NUM_BINS
            size_t binOffset = static_cast<size_t>(xFraction * NUM_BINS);
            size_t bin = (writeHeadBin + binOffset) % NUM_BINS;

            float mx = maxPeaks[bin].load(std::memory_order_relaxed) * displayGain;
            float mn = minPeaks[bin].load(std::memory_order_relaxed) * displayGain;

            // Clamping to [-1.0, 1.0] matching reference
            outMax[x] = std::max(-1.0f, std::min(1.0f, mx));
            outMin[x] = std::max(-1.0f, std::min(1.0f, mn));
        }
    }

private:
    std::atomic<float> maxPeaks[NUM_BINS];
    std::atomic<float> minPeaks[NUM_BINS];
    std::atomic<uint64_t> binEpoch[NUM_BINS];
};

} // namespace RollingSampler
