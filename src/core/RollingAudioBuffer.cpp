#include "RollingAudioBuffer.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstdio>

namespace RollingSampler {

RollingAudioBuffer::RollingAudioBuffer(uint32_t sr, double dur)
    : sampleRate(sr), durationSeconds(dur)
{
    capacity = static_cast<size_t>(sampleRate * durationSeconds);
    if (capacity == 0) capacity = 44100;
    bufferLeft.resize(capacity, 0.0f);
    bufferRight.resize(capacity, 0.0f);

    size_t numBlocks = (capacity + PEAK_BLOCK_SIZE - 1) / PEAK_BLOCK_SIZE;
    blockMax.resize(numBlocks, 0.0f);
    blockMin.resize(numBlocks, 0.0f);
}

void RollingAudioBuffer::writeSamples(const float* const* channelData, size_t numChannels, size_t numSamples) {
    if (numSamples == 0 || !channelData) return;
    if (paused.load(std::memory_order_relaxed)) return;

    const float* inL = (numChannels > 0 && channelData[0]) ? channelData[0] : nullptr;
    const float* inR = (numChannels > 1 && channelData[1]) ? channelData[1] : inL;

    uint32_t currentHead = writeHead.load(std::memory_order_relaxed);
    const size_t cap = capacity;
    if (cap == 0) return;

    float maxL = 0.0f;
    float maxR = 0.0f;
    uint64_t curEpoch = epoch.load(std::memory_order_relaxed);

    for (size_t i = 0; i < numSamples; ++i) {
        float sL = inL ? inL[i] : 0.0f;
        float sR = inR ? inR[i] : sL;

        float absL = std::fabs(sL);
        float absR = std::fabs(sR);
        if (absL > maxL) maxL = absL;
        if (absR > maxR) maxR = absR;

        bufferLeft[currentHead] = sL;
        bufferRight[currentHead] = sR;

        // Maintain exact real-time peak decimation pyramid using stereo peak bounds
        float sMax = std::max(sL, sR);
        float sMin = std::min(sL, sR);
        size_t bIdx = currentHead / PEAK_BLOCK_SIZE;
        size_t bOffset = currentHead % PEAK_BLOCK_SIZE;

        if (bIdx < blockMax.size()) {
            if (bOffset == 0) {
                blockMax[bIdx] = sMax;
                blockMin[bIdx] = sMin;
            } else {
                if (sMax > blockMax[bIdx]) blockMax[bIdx] = sMax;
                if (sMin < blockMin[bIdx]) blockMin[bIdx] = sMin;
            }
        }

        waveformCache.ingestSample(currentHead, cap, sL, sR, curEpoch);

        currentHead++;
        if (currentHead >= cap) {
            currentHead = 0;
            curEpoch = epoch.fetch_add(1, std::memory_order_relaxed) + 1;
        }
    }

    writeHead.store(currentHead, std::memory_order_release);
    totalSamplesWritten.fetch_add(numSamples, std::memory_order_relaxed);

    auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    lastWriteTimeNs.store(nowNs, std::memory_order_relaxed);

    // Update live peaks
    float curPeakL = peakLeft.load(std::memory_order_relaxed);
    if (maxL > curPeakL) peakLeft.store(maxL, std::memory_order_relaxed);

    float curPeakR = peakRight.load(std::memory_order_relaxed);
    if (maxR > curPeakR) peakRight.store(maxR, std::memory_order_relaxed);
}

size_t RollingAudioBuffer::readRecentSamples(float* destLeft, float* destRight, size_t count) const {
    const size_t cap = capacity;
    if (cap == 0 || count == 0) return 0;
    if (count > cap) count = cap;

    uint32_t head = writeHead.load(std::memory_order_acquire);
    size_t startPos = (head + cap - (count % cap)) % cap;

    readRange(startPos, count, destLeft, destRight);
    return count;
}

void RollingAudioBuffer::readRange(size_t startSample, size_t count, float* destLeft, float* destRight) const {
    const size_t cap = capacity;
    if (cap == 0 || count == 0) return;

    size_t pos = startSample % cap;
    size_t copied = 0;

    while (copied < count) {
        size_t chunk = std::min(count - copied, cap - pos);
        if (destLeft) {
            std::memcpy(destLeft + copied, bufferLeft.data() + pos, chunk * sizeof(float));
        }
        if (destRight) {
            std::memcpy(destRight + copied, bufferRight.data() + pos, chunk * sizeof(float));
        }
        copied += chunk;
        pos = (pos + chunk) % cap;
    }
}

size_t RollingAudioBuffer::getSelectionSampleCount(double startFrac, double endFrac) const {
    const size_t cap = capacity;
    if (cap == 0) return 0;
    double s = std::clamp(std::min(startFrac, endFrac), 0.0, 1.0);
    double e = std::clamp(std::max(startFrac, endFrac), 0.0, 1.0);
    return static_cast<size_t>(std::round((e - s) * cap));
}

size_t RollingAudioBuffer::readSelectionSamples(float* destLeft, float* destRight, double startFrac, double endFrac,
                                                bool trimSilence, float silenceThresholdDb) const {
    const size_t cap = capacity;
    if (cap == 0) return 0;

    double s = std::clamp(std::min(startFrac, endFrac), 0.0, 1.0);
    double e = std::clamp(std::max(startFrac, endFrac), 0.0, 1.0);
    size_t count = static_cast<size_t>(std::round((e - s) * cap));
    if (count == 0) return 0;

    uint32_t head = writeHead.load(std::memory_order_acquire);
    // startFrac = 0.0 is oldest (age = cap), 1.0 is newest (age = 0)
    // s is the earlier/older edge, so we start reading at s and copy count samples forward
    double ageStart = std::round((1.0 - s) * cap);
    size_t startPos = (head + cap - (static_cast<size_t>(ageStart) % cap)) % cap;

    // Read raw selection
    std::vector<float> tempL(count);
    std::vector<float> tempR(count);
    readRange(startPos, count, tempL.data(), tempR.data());

    if (!trimSilence) {
        if (destLeft) std::memcpy(destLeft, tempL.data(), count * sizeof(float));
        if (destRight) std::memcpy(destRight, tempR.data(), count * sizeof(float));
        return count;
    }

    // Automatically trim silence below threshold
    float ampThresh = std::pow(10.0f, silenceThresholdDb / 20.0f);
    size_t firstActive = count;
    size_t lastActive = 0;

    for (size_t i = 0; i < count; ++i) {
        float peak = std::max(std::fabs(tempL[i]), std::fabs(tempR[i]));
        if (peak >= ampThresh) {
            if (firstActive == count) firstActive = i;
            lastActive = i;
        }
    }

    if (firstActive > lastActive) {
        // Complete silence, return full buffer
        if (destLeft) std::memcpy(destLeft, tempL.data(), count * sizeof(float));
        if (destRight) std::memcpy(destRight, tempR.data(), count * sizeof(float));
        return count;
    }

    size_t trimmedCount = (lastActive - firstActive) + 1;
    if (destLeft) std::memcpy(destLeft, tempL.data() + firstActive, trimmedCount * sizeof(float));
    if (destRight) std::memcpy(destRight, tempR.data() + firstActive, trimmedCount * sizeof(float));
    return trimmedCount;
}

void RollingAudioBuffer::setDuration(double newDurationSeconds, uint32_t newSampleRate) {
    std::lock_guard<std::mutex> lock(resizeMutex);
    durationSeconds = newDurationSeconds;
    sampleRate = newSampleRate;
    size_t newCap = static_cast<size_t>(sampleRate * durationSeconds);
    if (newCap == 0) newCap = 44100;
    reallocate(newCap);
}

void RollingAudioBuffer::reallocate(size_t newCap) {
    if (newCap == capacity) return;

    std::vector<float> newLeft(newCap, 0.0f);
    std::vector<float> newRight(newCap, 0.0f);

    size_t samplesToCopy = std::min(capacity, newCap);
    uint32_t head = writeHead.load(std::memory_order_relaxed);

    if (samplesToCopy > 0 && capacity > 0) {
        size_t startPos = (head + capacity - (samplesToCopy % capacity)) % capacity;
        for (size_t i = 0; i < samplesToCopy; ++i) {
            size_t idx = (startPos + i) % capacity;
            newLeft[i] = bufferLeft[idx];
            newRight[i] = bufferRight[idx];
        }
        writeHead.store(static_cast<uint32_t>(samplesToCopy % newCap), std::memory_order_relaxed);
    } else {
        writeHead.store(0, std::memory_order_relaxed);
    }

    bufferLeft = std::move(newLeft);
    bufferRight = std::move(newRight);
    capacity = newCap;

    size_t numBlocks = (capacity + PEAK_BLOCK_SIZE - 1) / PEAK_BLOCK_SIZE;
    blockMax.assign(numBlocks, 0.0f);
    blockMin.assign(numBlocks, 0.0f);

    waveformCache.reset();
    epoch.fetch_add(1, std::memory_order_release);
}

double RollingAudioBuffer::getDuration() const {
    return durationSeconds;
}

uint32_t RollingAudioBuffer::getSampleRate() const {
    return sampleRate;
}

size_t RollingAudioBuffer::getCapacity() const {
    return capacity;
}

void RollingAudioBuffer::setPaused(bool isPause) {
    paused.store(isPause, std::memory_order_release);
}

bool RollingAudioBuffer::isPaused() const {
    return paused.load(std::memory_order_acquire);
}

void RollingAudioBuffer::clear() {
    std::lock_guard<std::mutex> lock(resizeMutex);
    std::fill(bufferLeft.begin(), bufferLeft.end(), 0.0f);
    std::fill(bufferRight.begin(), bufferRight.end(), 0.0f);
    std::fill(blockMax.begin(), blockMax.end(), 0.0f);
    std::fill(blockMin.begin(), blockMin.end(), 0.0f);
    writeHead.store(0, std::memory_order_release);
    waveformCache.reset();
    epoch.fetch_add(1, std::memory_order_release);
    totalSamplesWritten.store(0, std::memory_order_release);
    peakLeft.store(0.0f, std::memory_order_relaxed);
    peakRight.store(0.0f, std::memory_order_relaxed);
}

uint32_t RollingAudioBuffer::getWriteHead() const {
    return writeHead.load(std::memory_order_acquire);
}

double RollingAudioBuffer::getInterpolatedWriteHeadFrac() const {
    const size_t cap = capacity;
    if (cap == 0) return 0.0;

    uint32_t head = writeHead.load(std::memory_order_acquire);
    if (paused.load(std::memory_order_relaxed)) {
        return static_cast<double>(head) / static_cast<double>(cap);
    }

    int64_t lastNs = lastWriteTimeNs.load(std::memory_order_relaxed);
    if (lastNs == 0) {
        return static_cast<double>(head) / static_cast<double>(cap);
    }

    auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    double elapsedSec = static_cast<double>(nowNs - lastNs) * 1e-9;
    if (elapsedSec < 0.0) elapsedSec = 0.0;
    if (elapsedSec > 0.05) elapsedSec = 0.05; // clamp to 50ms to prevent overshoot during pause/audio glitches

    double extraSamples = elapsedSec * static_cast<double>(sampleRate);
    double continuousHead = std::fmod(static_cast<double>(head) + extraSamples, static_cast<double>(cap));
    return continuousHead / static_cast<double>(cap);
}

uint64_t RollingAudioBuffer::getEpoch() const {
    return epoch.load(std::memory_order_acquire);
}

uint64_t RollingAudioBuffer::getTotalSamplesWritten() const {
    return totalSamplesWritten.load(std::memory_order_acquire);
}

float RollingAudioBuffer::getPeakLeft() const {
    return peakLeft.load(std::memory_order_relaxed);
}

float RollingAudioBuffer::getPeakRight() const {
    return peakRight.load(std::memory_order_relaxed);
}

void RollingAudioBuffer::resetPeaks() {
    peakLeft.store(0.0f, std::memory_order_relaxed);
    peakRight.store(0.0f, std::memory_order_relaxed);
}

void RollingAudioBuffer::getSmoothRollingEnvelope(size_t numPixels, double displayGain, float* outMax, float* outMin) const {
    if (!outMax || !outMin || numPixels == 0) return;

    const size_t cap = capacity;
    if (cap == 0) {
        std::fill(outMax, outMax + numPixels, 0.0f);
        std::fill(outMin, outMin + numPixels, 0.0f);
        return;
    }

    const uint32_t head = writeHead.load(std::memory_order_acquire);
    const uint64_t totalWritten = totalSamplesWritten.load(std::memory_order_acquire);
    const double samplesPerPixel = static_cast<double>(cap) / static_cast<double>(numPixels);

    const size_t numBlocks = blockMax.size();

    for (size_t x = 0; x < numPixels; ++x) {
        // Pixel x=0 is oldest (age = cap), x=numPixels-1 is newest (age = 0)
        double ageStart = static_cast<double>(numPixels - 1 - x) * samplesPerPixel;
        double ageEnd = ageStart + samplesPerPixel;

        size_t startSample = static_cast<size_t>(ageStart);
        size_t endSample = static_cast<size_t>(ageEnd);
        if (endSample <= startSample) endSample = startSample + 1;

        if (startSample >= totalWritten) {
            outMax[x] = 0.0f;
            outMin[x] = 0.0f;
            continue;
        }

        float maxVal = 0.0f;
        float minVal = 0.0f;

        size_t span = endSample - startSample;
        for (size_t s = 0; s < span; ++s) {
            size_t age = startSample + s;
            if (age >= cap) age = cap - 1;
            if (age >= totalWritten) continue;

            size_t idx = (head + cap - 1 - (age % cap)) % cap;
            float hi = std::max(bufferLeft[idx], bufferRight[idx]);
            float lo = std::min(bufferLeft[idx], bufferRight[idx]);
            if (hi > maxVal) maxVal = hi;
            if (lo < minVal) minVal = lo;
        }

        outMax[x] = std::clamp(static_cast<float>(maxVal * displayGain), 0.0f, 1.0f);
        outMin[x] = std::clamp(static_cast<float>(minVal * displayGain), -1.0f, 0.0f);
    }
}

void RollingAudioBuffer::getBufferSweepEnvelope(size_t numPixels, double displayGain, float* outMax, float* outMin, size_t eraseGapPixels, size_t fadePixels, double headFrac) const {
    if (!outMax || !outMin || numPixels == 0) return;

    const size_t cap = capacity;
    if (cap == 0) {
        std::fill(outMax, outMax + numPixels, 0.0f);
        std::fill(outMin, outMin + numPixels, 0.0f);
        return;
    }

    const uint32_t head = writeHead.load(std::memory_order_acquire);
    const uint64_t totalWritten = totalSamplesWritten.load(std::memory_order_acquire);
    const double samplesPerPixel = static_cast<double>(cap) / static_cast<double>(numPixels);

    // Use high-resolution time-interpolated playhead position for silky synchronization
    double frac = (headFrac >= 0.0) ? headFrac : getInterpolatedWriteHeadFrac();
    double headPx = frac * static_cast<double>(numPixels);
    size_t headCol = (numPixels > 0) ? (static_cast<size_t>(headPx) % numPixels) : 0;

    for (size_t x = 0; x < numPixels; ++x) {
        size_t distAhead = (x >= headCol) ? (x - headCol) : (numPixels - headCol + x);

        // Leading Erase Bar: actively clear silence zone ahead of traveling write head (if erase gap enabled)
        if (eraseGapPixels > 0 && distAhead < eraseGapPixels) {
            outMax[x] = 0.0f;
            outMin[x] = 0.0f;
            continue;
        }

        // Screen pixel x maps linearly to buffer sample space [startSample, endSample)
        double sampleStart = static_cast<double>(x) * samplesPerPixel;
        double sampleEnd = sampleStart + samplesPerPixel;

        size_t startSample = static_cast<size_t>(sampleStart);
        size_t endSample = static_cast<size_t>(sampleEnd);
        if (endSample <= startSample) endSample = startSample + 1;
        if (endSample > cap) endSample = cap;

        // If total written < capacity and this column hasn't been written to yet in epoch 0:
        if (totalWritten < cap && startSample >= head) {
            outMax[x] = 0.0f;
            outMin[x] = 0.0f;
            continue;
        }

        float maxVal = 0.0f;
        float minVal = 0.0f;

        // At the write head column (x == headCol):
        // Old audio (s >= head) MUST NEVER leak into or past the yellow playhead!
        // We strictly only scan new samples [startSample, head) for this column.
        size_t scanEnd = (totalWritten >= cap && x == headCol)
            ? std::min(endSample, static_cast<size_t>(head))
            : endSample;

        if (scanEnd > startSample) {
            // Block-accelerated envelope scan
            size_t startBlock = (startSample + PEAK_BLOCK_SIZE - 1) / PEAK_BLOCK_SIZE;
            size_t endBlock = scanEnd / PEAK_BLOCK_SIZE;

            if (startBlock < endBlock && !blockMax.empty()) {
                size_t prefixEnd = startBlock * PEAK_BLOCK_SIZE;
                for (size_t s = startSample; s < prefixEnd; ++s) {
                    float hi = std::max(bufferLeft[s], bufferRight[s]);
                    float lo = std::min(bufferLeft[s], bufferRight[s]);
                    if (hi > maxVal) maxVal = hi;
                    if (lo < minVal) minVal = lo;
                }
                for (size_t b = startBlock; b < endBlock; ++b) {
                    if (blockMax[b] > maxVal) maxVal = blockMax[b];
                    if (blockMin[b] < minVal) minVal = blockMin[b];
                }
                size_t postfixStart = endBlock * PEAK_BLOCK_SIZE;
                for (size_t s = postfixStart; s < scanEnd; ++s) {
                    float hi = std::max(bufferLeft[s], bufferRight[s]);
                    float lo = std::min(bufferLeft[s], bufferRight[s]);
                    if (hi > maxVal) maxVal = hi;
                    if (lo < minVal) minVal = lo;
                }
            } else {
                for (size_t s = startSample; s < scanEnd; ++s) {
                    float hi = std::max(bufferLeft[s], bufferRight[s]);
                    float lo = std::min(bufferLeft[s], bufferRight[s]);
                    if (hi > maxVal) maxVal = hi;
                    if (lo < minVal) minVal = lo;
                }
            }
        }

        float scaledMax = std::clamp(static_cast<float>(maxVal * displayGain), 0.0f, 1.0f);
        float scaledMin = std::clamp(static_cast<float>(minVal * displayGain), -1.0f, 0.0f);

        // Gobble animation: smoothly ease old audio down ahead of the traveling playhead
        // It shrinks to 0 right at the yellow line so it disappears directly underneath
        if (fadePixels > 0 && distAhead >= eraseGapPixels && distAhead < eraseGapPixels + fadePixels) {
            float t = static_cast<float>(distAhead - eraseGapPixels) / static_cast<float>(fadePixels);
            float fade = t * t * (3.0f - 2.0f * t); // Smooth Hermite S-curve
            scaledMax *= fade;
            scaledMin *= fade;
        }

        outMax[x] = scaledMax;
        outMin[x] = scaledMin;
    }
}

size_t RollingAudioBuffer::readSweepSelectionSamples(float* destLeft, float* destRight, double startFrac, double endFrac,
                                                     bool trimSilence, float silenceThresholdDb) const {
    const size_t cap = capacity;
    if (cap == 0) return 0;

    double s = std::clamp(std::min(startFrac, endFrac), 0.0, 1.0);
    double e = std::clamp(std::max(startFrac, endFrac), 0.0, 1.0);
    size_t count = static_cast<size_t>(std::round((e - s) * cap));
    if (count == 0) return 0;

    size_t startPos = static_cast<size_t>(std::round(s * cap)) % cap;

    std::vector<float> tempL(count);
    std::vector<float> tempR(count);
    readRange(startPos, count, tempL.data(), tempR.data());

    if (!trimSilence) {
        if (destLeft) std::memcpy(destLeft, tempL.data(), count * sizeof(float));
        if (destRight) std::memcpy(destRight, tempR.data(), count * sizeof(float));
        return count;
    }

    float ampThresh = std::pow(10.0f, silenceThresholdDb / 20.0f);
    size_t firstActive = count;
    size_t lastActive = 0;

    for (size_t i = 0; i < count; ++i) {
        float peak = std::max(std::fabs(tempL[i]), std::fabs(tempR[i]));
        if (peak >= ampThresh) {
            if (firstActive == count) firstActive = i;
            lastActive = i;
        }
    }

    if (firstActive > lastActive) {
        if (destLeft) std::memcpy(destLeft, tempL.data(), count * sizeof(float));
        if (destRight) std::memcpy(destRight, tempR.data(), count * sizeof(float));
        return count;
    }

    size_t trimmedCount = (lastActive - firstActive) + 1;
    if (destLeft) std::memcpy(destLeft, tempL.data() + firstActive, trimmedCount * sizeof(float));
    if (destRight) std::memcpy(destRight, tempR.data() + firstActive, trimmedCount * sizeof(float));
    return trimmedCount;
}

bool RollingAudioBuffer::saveStateToFile(const std::wstring& filePath) const {
    std::lock_guard<std::mutex> lock(resizeMutex);
    FILE* f = _wfopen(filePath.c_str(), L"wb");
    if (!f) return false;

    uint32_t magic = 0x42475354; // 'BGST'
    uint32_t version = 1;
    uint32_t sr = sampleRate;
    double dur = durationSeconds;
    uint64_t cap = capacity;
    uint32_t head = writeHead.load(std::memory_order_relaxed);
    uint64_t ep = epoch.load(std::memory_order_relaxed);
    uint64_t totalWritten = totalSamplesWritten.load(std::memory_order_relaxed);
    uint8_t isP = paused.load(std::memory_order_relaxed) ? 1 : 0;

    fwrite(&magic, sizeof(magic), 1, f);
    fwrite(&version, sizeof(version), 1, f);
    fwrite(&sr, sizeof(sr), 1, f);
    fwrite(&dur, sizeof(dur), 1, f);
    fwrite(&cap, sizeof(cap), 1, f);
    fwrite(&head, sizeof(head), 1, f);
    fwrite(&ep, sizeof(ep), 1, f);
    fwrite(&totalWritten, sizeof(totalWritten), 1, f);
    fwrite(&isP, sizeof(isP), 1, f);

    if (cap > 0 && bufferLeft.size() >= cap && bufferRight.size() >= cap) {
        fwrite(bufferLeft.data(), sizeof(float), cap, f);
        fwrite(bufferRight.data(), sizeof(float), cap, f);
    }

    fclose(f);
    return true;
}

bool RollingAudioBuffer::loadStateFromFile(const std::wstring& filePath) {
    std::lock_guard<std::mutex> lock(resizeMutex);
    FILE* f = _wfopen(filePath.c_str(), L"rb");
    if (!f) return false;

    uint32_t magic = 0, version = 0, sr = 0;
    double dur = 0;
    uint64_t cap = 0;
    uint32_t head = 0;
    uint64_t ep = 0, totalWritten = 0;
    uint8_t isP = 0;

    if (fread(&magic, sizeof(magic), 1, f) != 1 || magic != 0x42475354) { fclose(f); return false; }
    if (fread(&version, sizeof(version), 1, f) != 1 || version != 1) { fclose(f); return false; }
    if (fread(&sr, sizeof(sr), 1, f) != 1) { fclose(f); return false; }
    if (fread(&dur, sizeof(dur), 1, f) != 1) { fclose(f); return false; }
    if (fread(&cap, sizeof(cap), 1, f) != 1 || cap == 0 || cap > 48000 * 600) { fclose(f); return false; }
    if (fread(&head, sizeof(head), 1, f) != 1) { fclose(f); return false; }
    if (fread(&ep, sizeof(ep), 1, f) != 1) { fclose(f); return false; }
    if (fread(&totalWritten, sizeof(totalWritten), 1, f) != 1) { fclose(f); return false; }
    if (fread(&isP, sizeof(isP), 1, f) != 1) { fclose(f); return false; }

    sampleRate = sr;
    durationSeconds = dur;
    capacity = static_cast<size_t>(cap);
    bufferLeft.resize(capacity, 0.0f);
    bufferRight.resize(capacity, 0.0f);

    if (fread(bufferLeft.data(), sizeof(float), capacity, f) != capacity ||
        fread(bufferRight.data(), sizeof(float), capacity, f) != capacity) {
        fclose(f);
        return false;
    }
    fclose(f);

    writeHead.store(std::min(head, static_cast<uint32_t>(capacity - 1)), std::memory_order_relaxed);
    epoch.store(ep, std::memory_order_relaxed);
    totalSamplesWritten.store(totalWritten, std::memory_order_relaxed);
    paused.store(isP != 0, std::memory_order_relaxed);

    size_t numBlocks = (capacity + PEAK_BLOCK_SIZE - 1) / PEAK_BLOCK_SIZE;
    blockMax.assign(numBlocks, 0.0f);
    blockMin.assign(numBlocks, 0.0f);
    for (size_t b = 0; b < numBlocks; ++b) {
        size_t bStart = b * PEAK_BLOCK_SIZE;
        size_t bEnd = std::min(bStart + PEAK_BLOCK_SIZE, capacity);
        float bMax = 0.0f, bMin = 0.0f;
        for (size_t i = bStart; i < bEnd; ++i) {
            float s = 0.5f * (bufferLeft[i] + bufferRight[i]);
            if (s > bMax) bMax = s;
            if (s < bMin) bMin = s;
        }
        blockMax[b] = bMax;
        blockMin[b] = bMin;
    }
    waveformCache.rebuild(bufferLeft.data(), bufferRight.data(), capacity);
    return true;
}

} // namespace RollingSampler
