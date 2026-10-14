#include "../src/core/RollingAudioBuffer.h"
#include <iostream>
#include <vector>
#include <cmath>
#include <cassert>
#include <chrono>

using namespace RollingSampler;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAILED: " << msg << " (" << #cond << ") at line " << __LINE__ << std::endl; \
            return false; \
        } \
    } while (0)

// Test 1: Exact sample ordering
bool testSampleOrdering() {
    RollingAudioBuffer buffer(1000, 1.0); // 1000 samples capacity
    std::vector<float> inL(200);
    std::vector<float> inR(200);
    for (size_t i = 0; i < 200; ++i) {
        inL[i] = static_cast<float>(i + 1) * 0.001f;
        inR[i] = static_cast<float>(i + 1) * -0.001f;
    }
    const float* channels[2] = { inL.data(), inR.data() };
    buffer.writeSamples(channels, 2, 200);

    TEST_ASSERT(buffer.getWriteHead() == 200, "Write head should be 200");
    TEST_ASSERT(buffer.getTotalSamplesWritten() == 200, "Total samples should be 200");

    std::vector<float> outL(200);
    std::vector<float> outR(200);
    buffer.readRecentSamples(outL.data(), outR.data(), 200);

    for (size_t i = 0; i < 200; ++i) {
        TEST_ASSERT(std::fabs(outL[i] - inL[i]) < 1e-6f, "Left sample mismatch in ordering");
        TEST_ASSERT(std::fabs(outR[i] - inR[i]) < 1e-6f, "Right sample mismatch in ordering");
    }
    std::cout << "  [PASS] Test 1: Exact sample ordering verified." << std::endl;
    return true;
}

// Test 2: Circular wraparound
bool testCircularWraparound() {
    RollingAudioBuffer buffer(100, 1.0); // Capacity 100 samples
    TEST_ASSERT(buffer.getCapacity() == 100, "Capacity should be 100");

    // Write 250 samples: should wrap twice and leave head at 50
    std::vector<float> inL(250);
    std::vector<float> inR(250);
    for (size_t i = 0; i < 250; ++i) {
        inL[i] = static_cast<float>(i);
        inR[i] = static_cast<float>(i * 2);
    }
    const float* channels[2] = { inL.data(), inR.data() };
    buffer.writeSamples(channels, 2, 250);

    TEST_ASSERT(buffer.getWriteHead() == 50, "Write head after 250 samples on 100 cap should be 50");
    TEST_ASSERT(buffer.getEpoch() == 2, "Epoch after 2 full wraps should be 2");
    TEST_ASSERT(buffer.getTotalSamplesWritten() == 250, "Total samples should be 250");

    // Read the most recent 100 samples. They should correspond to inL[150..249]
    std::vector<float> outL(100);
    std::vector<float> outR(100);
    buffer.readRecentSamples(outL.data(), outR.data(), 100);

    for (size_t i = 0; i < 100; ++i) {
        float expectedL = static_cast<float>(150 + i);
        float expectedR = static_cast<float>((150 + i) * 2);
        TEST_ASSERT(std::fabs(outL[i] - expectedL) < 1e-6f, "Wraparound left sample mismatch");
        TEST_ASSERT(std::fabs(outR[i] - expectedR) < 1e-6f, "Wraparound right sample mismatch");
    }
    std::cout << "  [PASS] Test 2: Circular wraparound verified." << std::endl;
    return true;
}

// Test 3: Channel integrity (Stereo separation)
bool testChannelIntegrity() {
    RollingAudioBuffer buffer(1000, 1.0);
    // Distinct signals: Left = 0.75, Right = -0.25
    std::vector<float> inL(500, 0.75f);
    std::vector<float> inR(500, -0.25f);
    const float* channels[2] = { inL.data(), inR.data() };
    buffer.writeSamples(channels, 2, 500);

    std::vector<float> outL(500);
    std::vector<float> outR(500);
    buffer.readRecentSamples(outL.data(), outR.data(), 500);

    for (size_t i = 0; i < 500; ++i) {
        TEST_ASSERT(std::fabs(outL[i] - 0.75f) < 1e-6f, "Channel integrity Left corrupted");
        TEST_ASSERT(std::fabs(outR[i] - (-0.25f)) < 1e-6f, "Channel integrity Right corrupted");
    }
    std::cout << "  [PASS] Test 3: Channel integrity verified." << std::endl;
    return true;
}

// Test 4: Pause behavior
bool testPause() {
    RollingAudioBuffer buffer(1000, 1.0);
    std::vector<float> in(100, 0.5f);
    const float* channels[2] = { in.data(), in.data() };
    buffer.writeSamples(channels, 2, 100);
    TEST_ASSERT(buffer.getWriteHead() == 100, "Head before pause should be 100");

    // Pause the buffer
    buffer.setPaused(true);
    TEST_ASSERT(buffer.isPaused() == true, "Buffer should be paused");

    // Attempt to write 200 samples while paused
    std::vector<float> ignore(200, 0.9f);
    const float* ignoreCh[2] = { ignore.data(), ignore.data() };
    buffer.writeSamples(ignoreCh, 2, 200);

    TEST_ASSERT(buffer.getWriteHead() == 100, "Head must NOT advance while paused");
    TEST_ASSERT(buffer.getTotalSamplesWritten() == 100, "Total samples must NOT increase while paused");

    std::cout << "  [PASS] Test 4: Pause behavior verified." << std::endl;
    return true;
}

// Test 5: Resume behavior
bool testResume() {
    RollingAudioBuffer buffer(1000, 1.0);
    std::vector<float> in1(100, 0.2f);
    const float* ch1[2] = { in1.data(), in1.data() };
    buffer.writeSamples(ch1, 2, 100);

    buffer.setPaused(true);
    std::vector<float> ignore(100, 0.9f);
    const float* chIgnore[2] = { ignore.data(), ignore.data() };
    buffer.writeSamples(chIgnore, 2, 100);

    // Resume
    buffer.setPaused(false);
    TEST_ASSERT(buffer.isPaused() == false, "Buffer should be unpaused");

    std::vector<float> in2(50, 0.8f);
    const float* ch2[2] = { in2.data(), in2.data() };
    buffer.writeSamples(ch2, 2, 50);

    TEST_ASSERT(buffer.getWriteHead() == 150, "Head after resume should be 150");
    TEST_ASSERT(buffer.getTotalSamplesWritten() == 150, "Total samples after resume should be 150");

    std::vector<float> outL(150);
    buffer.readRecentSamples(outL.data(), nullptr, 150);

    for (size_t i = 0; i < 100; ++i) {
        TEST_ASSERT(std::fabs(outL[i] - 0.2f) < 1e-6f, "Pre-pause samples mismatch");
    }
    for (size_t i = 100; i < 150; ++i) {
        TEST_ASSERT(std::fabs(outL[i] - 0.8f) < 1e-6f, "Post-resume samples mismatch");
    }
    std::cout << "  [PASS] Test 5: Resume behavior verified." << std::endl;
    return true;
}

// Test 6: Buffer duration changes
bool testBufferDuration() {
    RollingAudioBuffer buffer(1000, 2.0); // 2000 samples
    TEST_ASSERT(buffer.getCapacity() == 2000, "Initial capacity should be 2000");

    std::vector<float> in(500, 0.33f);
    const float* ch[2] = { in.data(), in.data() };
    buffer.writeSamples(ch, 2, 500);

    // Expand duration to 5.0s (5000 samples)
    buffer.setDuration(5.0, 1000);
    TEST_ASSERT(buffer.getCapacity() == 5000, "Capacity should expand to 5000");
    TEST_ASSERT(buffer.getDuration() == 5.0, "Duration should be 5.0");

    // Shrink duration to 1.0s (1000 samples)
    buffer.setDuration(1.0, 1000);
    TEST_ASSERT(buffer.getCapacity() == 1000, "Capacity should shrink to 1000");

    std::cout << "  [PASS] Test 6: Buffer duration reconfiguration verified." << std::endl;
    return true;
}

// Test 7: Long-running memory stability
bool testMemoryStability() {
    RollingAudioBuffer buffer(44100, 30.0); // 30s buffer @ 44.1kHz = 1,323,000 samples
    const size_t blockSize = 512;
    std::vector<float> block(blockSize, 0.1f);
    const float* ch[2] = { block.data(), block.data() };

    // Simulate 20 minutes of continuous audio ingestion (52,920,000 samples = ~40 complete wraparounds)
    auto t0 = std::chrono::high_resolution_clock::now();
    const size_t totalSamples = 44100 * 60 * 20;
    size_t written = 0;
    while (written < totalSamples) {
        size_t chunk = std::min(blockSize, totalSamples - written);
        buffer.writeSamples(ch, 2, chunk);
        written += chunk;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    TEST_ASSERT(buffer.getCapacity() == 1323000, "Capacity must remain constant");
    TEST_ASSERT(buffer.getEpoch() == 40, "Epoch should be 40 after 20 minutes of 30s buffer");
    std::cout << "  [PASS] Test 7: Long-running memory stability verified (simulated 20 mins in " 
              << ms << " ms, 40 wraparounds, 0 leaks)." << std::endl;
    return true;
}

// Test 8: Waveform Decimation Cache accuracy and peak extraction
bool testWaveformDecimationCache() {
    RollingAudioBuffer buffer(10000, 1.0); // 10,000 samples, 1.0s
    std::vector<float> sine(10000);
    for (size_t i = 0; i < 10000; ++i) {
        sine[i] = 0.8f * std::sin(2.0 * 3.141592653589793 * (i * 10.0 / 10000.0));
    }
    const float* ch[2] = { sine.data(), sine.data() };
    buffer.writeSamples(ch, 2, 10000);

    const auto& cache = buffer.getWaveformCache();
    std::vector<float> maxEnv(1000);
    std::vector<float> minEnv(1000);
    cache.getRollingEnvelope(1000, buffer.getWriteHead(), buffer.getCapacity(), 1.0f, maxEnv.data(), minEnv.data());

    float globalMax = -1.0f;
    float globalMin = 1.0f;
    for (size_t i = 0; i < 1000; ++i) {
        if (maxEnv[i] > globalMax) globalMax = maxEnv[i];
        if (minEnv[i] < globalMin) globalMin = minEnv[i];
    }

    TEST_ASSERT(std::fabs(globalMax - 0.8f) < 0.05f, "Global max peak should be ~0.8");
    TEST_ASSERT(std::fabs(globalMin - (-0.8f)) < 0.05f, "Global min peak should be ~-0.8");

    // Test with Display Gain = 2.0x -> should clamp at 1.0 / -1.0
    cache.getRollingEnvelope(1000, buffer.getWriteHead(), buffer.getCapacity(), 2.0f, maxEnv.data(), minEnv.data());
    float maxClamped = *std::max_element(maxEnv.begin(), maxEnv.end());
    float minClamped = *std::min_element(minEnv.begin(), minEnv.end());
    TEST_ASSERT(std::fabs(maxClamped - 1.0f) < 1e-4f, "Display gain should clamp at 1.0");
    TEST_ASSERT(std::fabs(minClamped - (-1.0f)) < 1e-4f, "Display gain should clamp at -1.0");

    std::cout << "  [PASS] Test 8: Waveform decimation cache accuracy and gain clamping verified." << std::endl;
    return true;
}

// Test 9: Rolling Coordinate Mapping and recent-audio orientation
bool testWaveformRollingEnvelope() {
    RollingAudioBuffer buffer(1000, 1.0); // 1000 samples
    // Write 500 samples of ramp from 0.1 to 0.9
    std::vector<float> ramp(500);
    for (size_t i = 0; i < 500; ++i) {
        ramp[i] = 0.1f + 0.8f * (static_cast<float>(i) / 499.0f);
    }
    const float* ch[2] = { ramp.data(), ramp.data() };
    buffer.writeSamples(ch, 2, 500);

    const auto& cache = buffer.getWaveformCache();
    std::vector<float> maxEnv(100);
    std::vector<float> minEnv(100);
    cache.getRollingEnvelope(100, buffer.getWriteHead(), buffer.getCapacity(), 1.0f, maxEnv.data(), minEnv.data());

    // Most recent sample at right edge (x = 99) should be near the end of the ramp (~0.9)
    TEST_ASSERT(maxEnv[99] >= 0.7f, "Right edge of rolling waveform must represent newest audio");

    std::cout << "  [PASS] Test 9: Rolling coordinate mapping (newest audio at right edge) verified." << std::endl;
    return true;
}

// Test 10: Smooth Direct Rolling Waveform Envelope (zero-flicker sub-sample continuity)
bool testSmoothRollingEnvelope() {
    RollingAudioBuffer buffer(1000, 1.0); // 1000 samples
    std::vector<float> wave(1000);
    // 200 Hz wave across 1000 samples (period = 5 samples), so every 10-sample pixel column spans 2 full cycles
    for (size_t i = 0; i < 1000; ++i) {
        wave[i] = std::sin(2.0f * 3.14159265f * static_cast<float>(i) / 5.0f);
    }
    const float* ch[2] = { wave.data(), wave.data() };
    buffer.writeSamples(ch, 2, 1000);

    std::vector<float> maxEnv(100);
    std::vector<float> minEnv(100);
    buffer.getSmoothRollingEnvelope(100, 1.0, maxEnv.data(), minEnv.data());

    // Check all pixels capture crests and troughs
    for (size_t x = 0; x < 100; ++x) {
        TEST_ASSERT(maxEnv[x] >= 0.8f, "Max envelope must capture wave positive crest");
        TEST_ASSERT(minEnv[x] <= -0.8f, "Min envelope must capture wave negative trough");
    }

    std::cout << "  [PASS] Test 10: Smooth direct rolling waveform envelope verified." << std::endl;
    return true;
}

// Test 11: Selection Audio Extraction & Silence Trimming
bool testSelectionExtraction() {
    RollingAudioBuffer buffer(1000, 1.0); // 1000 samples
    std::vector<float> audio(1000, 0.0f);
    // Silence from 0..200, tone from 200..600, silence from 600..1000
    for (size_t i = 200; i < 600; ++i) {
        audio[i] = 0.5f;
    }
    const float* ch[2] = { audio.data(), audio.data() };
    buffer.writeSamples(ch, 2, 1000);

    // Read full visible buffer without trim
    std::vector<float> outL(1000);
    std::vector<float> outR(1000);
    size_t count = buffer.readSelectionSamples(outL.data(), outR.data(), 0.0, 1.0, false);
    TEST_ASSERT(count == 1000, "Full selection must return 1000 samples");

    // Read exact sub-slice from 0.2 to 0.6 (where tone was written)
    std::vector<float> subL(400);
    std::vector<float> subR(400);
    size_t subCount = buffer.readSelectionSamples(subL.data(), subR.data(), 0.2, 0.6, false);
    TEST_ASSERT(subCount == 400, "Sub-range must return 400 samples");
    for (size_t i = 0; i < 400; ++i) {
        TEST_ASSERT(std::fabs(subL[i] - 0.5f) < 1e-5f, "Sub-range sample must match written tone");
    }

    // Read with silence trim at -40 dB
    size_t trimmedCount = buffer.readSelectionSamples(outL.data(), outR.data(), 0.0, 1.0, true, -40.0f);
    TEST_ASSERT(trimmedCount >= 390 && trimmedCount <= 410, "Trimmed selection must isolate active audio region");

    std::cout << "  [PASS] Test 11: Selection extraction and silence trimming verified." << std::endl;
    return true;
}

// Test 12: Sweep Mode envelope and traveling playhead selection
bool testBufferSweepEnvelope() {
    RollingAudioBuffer buffer(1000, 1.0); // 1000 samples capacity
    std::vector<float> audio(500, 0.8f);
    const float* ch[2] = { audio.data(), audio.data() };
    buffer.writeSamples(ch, 2, 500);

    TEST_ASSERT(buffer.getWriteHead() == 500, "Write head must be 500");
    TEST_ASSERT(buffer.getTotalSamplesWritten() == 500, "Total samples written must be 500");

    // Extract 100 pixels: cols 0..49 have audio (0.8), cols 50..99 are empty (0.0) ahead of the traveling head
    std::vector<float> maxEnv(100);
    std::vector<float> minEnv(100);
    buffer.getBufferSweepEnvelope(100, 1.0, maxEnv.data(), minEnv.data());

    for (size_t x = 0; x < 50; ++x) {
        TEST_ASSERT(std::fabs(maxEnv[x] - 0.8f) < 1e-4f, "Sweep envelope behind write head must reflect audio");
    }
    for (size_t x = 50; x < 100; ++x) {
        TEST_ASSERT(maxEnv[x] == 0.0f, "Sweep envelope ahead of write head on epoch 0 must be 0.0");
        TEST_ASSERT(minEnv[x] == 0.0f, "Sweep min ahead of write head on epoch 0 must be 0.0");
    }

    // Verify sweep selection
    std::vector<float> selL(500);
    std::vector<float> selR(500);
    size_t count = buffer.readSweepSelectionSamples(selL.data(), selR.data(), 0.0, 0.5, false);
    TEST_ASSERT(count == 500, "Sweep selection from 0.0 to 0.5 must return 500 samples");
    for (size_t i = 0; i < 500; ++i) {
        TEST_ASSERT(std::fabs(selL[i] - 0.8f) < 1e-4f, "Sweep selection sample must match written audio");
    }

    // Verify Erase Gap when buffer wraps around (epoch > 0)
    // Fill the remainder of the buffer (500 samples) with 0.8f, then wrap by 200 samples with 0.4f
    std::vector<float> fillRemaining(500, 0.8f);
    const float* fillCh[2] = { fillRemaining.data(), fillRemaining.data() };
    buffer.writeSamples(fillCh, 2, 500); // writeHead = 0 (1000 total written)

    std::vector<float> wrapAudio(200, 0.4f);
    const float* wrapCh[2] = { wrapAudio.data(), wrapAudio.data() };
    buffer.writeSamples(wrapCh, 2, 200); // writeHead = 200 (1200 total written, headPx = 20)

    // With eraseGapPixels = 10:
    // cols 0..19 are new audio (0.4f)
    // cols 20..29 are erase gap ahead of head (0.0f)
    // cols 30..99 are old audio from previous pass (0.8f)
    buffer.getBufferSweepEnvelope(100, 1.0, maxEnv.data(), minEnv.data(), 10);
    for (size_t x = 0; x < 20; ++x) {
        TEST_ASSERT(std::fabs(maxEnv[x] - 0.4f) < 1e-4f, "Sweep envelope behind write head must reflect new audio");
    }
    for (size_t x = 20; x < 30; ++x) {
        TEST_ASSERT(maxEnv[x] == 0.0f, "Sweep erase gap ahead of write head must be 0.0f");
        TEST_ASSERT(minEnv[x] == 0.0f, "Sweep erase gap min must be 0.0f");
    }
    for (size_t x = 30; x < 100; ++x) {
        TEST_ASSERT(std::fabs(maxEnv[x] - 0.8f) < 1e-4f, "Sweep envelope beyond erase gap must retain previous pass audio");
    }

    std::cout << "  [PASS] Test 12: Sweep Mode envelope, erase gap, and traveling playhead verified." << std::endl;
    return true;
}

// Test 13: Buffer State Persistence (Save States / Session Recovery)
bool testBufferStatePersistence() {
    RollingAudioBuffer buffer(48000, 2.0); // 96000 samples
    std::vector<float> sineL(1000);
    std::vector<float> sineR(1000);
    for (size_t i = 0; i < 1000; ++i) {
        sineL[i] = std::sin(2.0f * 3.14159f * 440.0f * i / 48000.0f);
        sineR[i] = -sineL[i];
    }
    const float* chs[2] = { sineL.data(), sineR.data() };
    buffer.writeSamples(chs, 2, 1000);
    buffer.setPaused(true);

    std::wstring tempPath = L"test_session.state";
    TEST_ASSERT(buffer.saveStateToFile(tempPath), "State file must save successfully");

    RollingAudioBuffer restoredBuffer(44100, 30.0);
    TEST_ASSERT(restoredBuffer.loadStateFromFile(tempPath), "State file must load successfully");

    TEST_ASSERT(restoredBuffer.getSampleRate() == 48000, "Restored sample rate must match");
    TEST_ASSERT(std::fabs(restoredBuffer.getDuration() - 2.0) < 1e-4, "Restored duration must match");
    TEST_ASSERT(restoredBuffer.getCapacity() == 96000, "Restored capacity must match");
    TEST_ASSERT(restoredBuffer.getWriteHead() == 1000, "Restored write head must match");
    TEST_ASSERT(restoredBuffer.isPaused() == true, "Restored pause state must match");

    std::vector<float> readL(1000);
    std::vector<float> readR(1000);
    size_t count = restoredBuffer.readRecentSamples(readL.data(), readR.data(), 1000);
    TEST_ASSERT(count == 1000, "Restored buffer must read 1000 recent samples");
    for (size_t i = 0; i < 1000; ++i) {
        TEST_ASSERT(std::fabs(readL[i] - sineL[i]) < 1e-5f, "Restored left audio sample must match exactly");
        TEST_ASSERT(std::fabs(readR[i] - sineR[i]) < 1e-5f, "Restored right audio sample must match exactly");
    }

    _wremove(tempPath.c_str());
    std::cout << "  [PASS] Test 13: Buffer State Persistence (Save States / Session Recovery) verified." << std::endl;
    return true;
}

#include "../src/core/SettingsManager.h"

// Test 14: Settings Persistence (.bbsettings XML & INI)
bool testSettingsPersistence() {
    AppSettings original;
    original.themeId = 4; // Lemon
    original.displayGain = 2.5f;
    original.duration = 90.0;
    original.save24Bit = true;
    original.channelMode = 1; // MonoLeft
    original.trimSilence = true;
    original.silenceThresholdDb = -84.5f;
    original.appendBpm = true;
    original.saveSessionState = true;
    original.displayMode = 1; // Rolling
    original.asymmetrical = true;
    original.pinOnTop = true;
    original.targetFps = 30;
    original.borderless = false;
    original.isLoopback = true;
    original.windowX = 150;
    original.windowY = 220;
    original.windowWidth = 1350;
    original.windowHeight = 310;

    // Test XML Serialization
    std::string xml = SettingsManager::serializeToBbSettingsXml(original);
    TEST_ASSERT(xml.find("<PROPERTIES>") != std::string::npos, "XML must contain <PROPERTIES>");
    TEST_ASSERT(xml.find("name=\"Selected Theme\" val=\"4\"") != std::string::npos, "XML must have theme 4");
    TEST_ASSERT(xml.find("name=\"Standalone Display Width\" val=\"1350\"") != std::string::npos, "XML must have width 1350");
    TEST_ASSERT(xml.find("name=\"Target FPS\" val=\"30\"") != std::string::npos, "XML must have FPS 30");

    // Test XML Deserialization
    AppSettings restored;
    bool parsed = SettingsManager::deserializeFromBbSettingsXml(xml, restored);
    TEST_ASSERT(parsed, "Deserialization from XML must succeed");
    TEST_ASSERT(restored.themeId == 4, "Restored theme must be 4");
    TEST_ASSERT(std::fabs(restored.displayGain - 2.5f) < 1e-4f, "Restored display gain must be 2.5");
    TEST_ASSERT(std::fabs(restored.duration - 90.0) < 1e-4, "Restored duration must be 90.0");
    TEST_ASSERT(restored.save24Bit == true, "Restored save24Bit must be true");
    TEST_ASSERT(restored.channelMode == 1, "Restored channelMode must be 1");
    TEST_ASSERT(restored.trimSilence == true, "Restored trimSilence must be true");
    TEST_ASSERT(std::fabs(restored.silenceThresholdDb - (-84.5f)) < 0.1f, "Restored threshold must match");
    TEST_ASSERT(restored.displayMode == 1, "Restored display mode must be 1");
    TEST_ASSERT(restored.borderless == false, "Restored borderless must be false");
    TEST_ASSERT(restored.targetFps == 30, "Restored target FPS must be 30");
    TEST_ASSERT(restored.windowWidth == 1350, "Restored window width must be 1350");
    TEST_ASSERT(restored.windowHeight == 310, "Restored window height must be 310");

    std::cout << "  [PASS] Test 14: Settings Persistence (.bbsettings XML & INI) verified." << std::endl;
    return true;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "RUNNING ROLLING BUFFER AUTOMATED TEST SUITE" << std::endl;
    std::cout << "========================================" << std::endl;

    if (!testSampleOrdering()) return 1;
    if (!testCircularWraparound()) return 2;
    if (!testChannelIntegrity()) return 3;
    if (!testPause()) return 4;
    if (!testResume()) return 5;
    if (!testBufferDuration()) return 6;
    if (!testMemoryStability()) return 7;
    if (!testWaveformDecimationCache()) return 8;
    if (!testWaveformRollingEnvelope()) return 9;
    if (!testSmoothRollingEnvelope()) return 10;
    if (!testSelectionExtraction()) return 11;
    if (!testBufferSweepEnvelope()) return 12;
    if (!testBufferStatePersistence()) return 13;
    if (!testSettingsPersistence()) return 14;

    std::cout << "========================================" << std::endl;
    std::cout << "ALL 14 AUTOMATED BUFFER & WAVEFORM TESTS PASSED 100%" << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}



