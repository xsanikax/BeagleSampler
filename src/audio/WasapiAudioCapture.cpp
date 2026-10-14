#include "WasapiAudioCapture.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <timeapi.h>
#include <avrt.h>
#include <iostream>
#include <vector>
#include <cmath>
#include <chrono>

namespace RollingSampler {

static const GUID GUID_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID GUID_KSDATAFORMAT_SUBTYPE_PCM =
    { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

// High-fidelity streaming resampler maintaining phase continuity across WASAPI packet boundaries
class StreamingResampler {
public:
    StreamingResampler() = default;

    void reset(uint32_t inRate, uint32_t outRate) {
        inputRate = inRate;
        outputRate = outRate;
        ratio = static_cast<double>(inRate) / static_cast<double>(outRate);
        phase = 0.0;
        prevL = 0.0f;
        prevR = 0.0f;
        hasPrev = false;
    }

    void process(const float* inL, const float* inR, size_t inFrames,
                 std::vector<float>& outL, std::vector<float>& outR) {
        if (inFrames == 0) {
            outL.clear();
            outR.clear();
            return;
        }

        // Direct 1:1 pass-through if sample rates match (e.g. 48kHz Desktop Audio)
        if (inputRate == outputRate) {
            outL.assign(inL, inL + inFrames);
            outR.assign(inR, inR + inFrames);
            return;
        }

        // Exact integer decimation: 4:1 (e.g. 192,000 Hz Realtek microphone -> 48,000 Hz)
        if (inputRate == outputRate * 4) {
            size_t outFrames = inFrames / 4;
            outL.resize(outFrames);
            outR.resize(outFrames);
            for (size_t o = 0; o < outFrames; ++o) {
                size_t base = o * 4;
                outL[o] = 0.25f * (inL[base] + inL[base + 1] + inL[base + 2] + inL[base + 3]);
                outR[o] = 0.25f * (inR[base] + inR[base + 1] + inR[base + 2] + inR[base + 3]);
            }
            return;
        }

        // Exact integer decimation: 2:1 (e.g. 96,000 Hz -> 48,000 Hz)
        if (inputRate == outputRate * 2) {
            size_t outFrames = inFrames / 2;
            outL.resize(outFrames);
            outR.resize(outFrames);
            for (size_t o = 0; o < outFrames; ++o) {
                size_t base = o * 2;
                outL[o] = 0.5f * (inL[base] + inL[base + 1]);
                outR[o] = 0.5f * (inR[base] + inR[base + 1]);
            }
            return;
        }

        // Continuous fractional resampler with phase tracking across buffer boundaries
        outL.clear();
        outR.clear();
        while (phase < static_cast<double>(inFrames)) {
            size_t idx0 = static_cast<size_t>(phase);
            float frac = static_cast<float>(phase - idx0);

            float sL0 = (idx0 == 0 && hasPrev) ? prevL : inL[idx0];
            float sR0 = (idx0 == 0 && hasPrev) ? prevR : inR[idx0];

            float sL1 = (idx0 + 1 < inFrames) ? inL[idx0 + 1] : inL[idx0];
            float sR1 = (idx0 + 1 < inFrames) ? inR[idx0 + 1] : inR[idx0];

            outL.push_back(sL0 * (1.0f - frac) + sL1 * frac);
            outR.push_back(sR0 * (1.0f - frac) + sR1 * frac);

            phase += ratio;
        }

        phase -= static_cast<double>(inFrames);
        prevL = inL[inFrames - 1];
        prevR = inR[inFrames - 1];
        hasPrev = true;
    }

private:
    uint32_t inputRate{48000};
    uint32_t outputRate{48000};
    double ratio{1.0};
    double phase{0.0};
    float prevL{0.0f};
    float prevR{0.0f};
    bool hasPrev{false};
};

AudioCapture::AudioCapture(RollingAudioBuffer& buffer)
    : audioBuffer(buffer),
      selectedDeviceId(L"default_loopback"),
      selectedIsLoopback(true),
      activeDeviceName(L"Desktop Audio"),
      activeIsLoopback(true)
{
    hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

AudioCapture::~AudioCapture() {
    stop();
    if (hStopEvent) {
        CloseHandle(static_cast<HANDLE>(hStopEvent));
        hStopEvent = nullptr;
    }
}

bool AudioCapture::start() {
    if (running.load()) return true;
    running.store(true);
    if (hStopEvent) ResetEvent(static_cast<HANDLE>(hStopEvent));
    workerThread = std::thread(&AudioCapture::captureLoop, this);
    return true;
}

void AudioCapture::stop() {
    if (!running.load()) return;
    running.store(false);
    if (hStopEvent) SetEvent(static_cast<HANDLE>(hStopEvent));
    if (workerThread.joinable()) {
        workerThread.join();
    }
    if (hStopEvent) ResetEvent(static_cast<HANDLE>(hStopEvent));
}

bool AudioCapture::isRunning() const {
    return running.load();
}

std::wstring AudioCapture::getDeviceName() const {
    std::lock_guard<std::mutex> lock(deviceMutex);
    return activeDeviceName;
}

bool AudioCapture::isLoopback() const {
    std::lock_guard<std::mutex> lock(deviceMutex);
    return activeIsLoopback;
}

uint32_t AudioCapture::getSampleRate() const {
    return INTERNAL_SAMPLE_RATE;
}

uint32_t AudioCapture::getDeviceSampleRate() const {
    return deviceSampleRate;
}

uint32_t AudioCapture::getChannels() const {
    return channels;
}

std::vector<AudioDeviceInfo> AudioCapture::getAvailableDevices() {
    std::vector<AudioDeviceInfo> devices;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool coInit = SUCCEEDED(hr);

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&enumerator);

    if (SUCCEEDED(hr) && enumerator) {
        // 1. Enumerate Capture Devices (Microphones & Inputs)
        IMMDevice* defaultCaptureDev = nullptr;
        LPWSTR defaultCaptureId = nullptr;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &defaultCaptureDev)) && defaultCaptureDev) {
            defaultCaptureDev->GetId(&defaultCaptureId);
        }

        IMMDeviceCollection* captureCollection = nullptr;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &captureCollection)) && captureCollection) {
            UINT count = 0;
            captureCollection->GetCount(&count);
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* pDev = nullptr;
                if (SUCCEEDED(captureCollection->Item(i, &pDev)) && pDev) {
                    LPWSTR pId = nullptr;
                    pDev->GetId(&pId);

                    IPropertyStore* pProps = nullptr;
                    std::wstring devName = L"Audio Input " + std::to_wstring(i + 1);

                    if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
                        PROPVARIANT pv;
                        PropVariantInit(&pv);
                        if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &pv))) {
                            if (pv.vt == VT_LPWSTR && pv.pwszVal) {
                                devName = pv.pwszVal;
                            }
                            PropVariantClear(&pv);
                        }
                        pProps->Release();
                    }

                    if (pId) {
                        bool isDef = (defaultCaptureId && wcscmp(defaultCaptureId, pId) == 0);
                        devices.push_back({ pId, devName, isDef, false });
                        CoTaskMemFree(pId);
                    }
                    pDev->Release();
                }
            }
            captureCollection->Release();
        }

        if (defaultCaptureId) CoTaskMemFree(defaultCaptureId);
        if (defaultCaptureDev) defaultCaptureDev->Release();

        // 2. Enumerate Render Devices (Speakers, Headphones, Desktop Audio Loopback)
        IMMDevice* defaultRenderDev = nullptr;
        LPWSTR defaultRenderId = nullptr;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultRenderDev)) && defaultRenderDev) {
            defaultRenderDev->GetId(&defaultRenderId);
        }

        IMMDeviceCollection* renderCollection = nullptr;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &renderCollection)) && renderCollection) {
            UINT count = 0;
            renderCollection->GetCount(&count);
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* pDev = nullptr;
                if (SUCCEEDED(renderCollection->Item(i, &pDev)) && pDev) {
                    LPWSTR pId = nullptr;
                    pDev->GetId(&pId);

                    IPropertyStore* pProps = nullptr;
                    std::wstring devName = L"Audio Output " + std::to_wstring(i + 1);

                    if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
                        PROPVARIANT pv;
                        PropVariantInit(&pv);
                        if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &pv))) {
                            if (pv.vt == VT_LPWSTR && pv.pwszVal) {
                                devName = pv.pwszVal;
                            }
                            PropVariantClear(&pv);
                        }
                        pProps->Release();
                    }

                    if (pId) {
                        bool isDef = (defaultRenderId && wcscmp(defaultRenderId, pId) == 0);
                        devices.push_back({ pId, devName, isDef, true });
                        CoTaskMemFree(pId);
                    }
                    pDev->Release();
                }
            }
            renderCollection->Release();
        }

        if (defaultRenderId) CoTaskMemFree(defaultRenderId);
        if (defaultRenderDev) defaultRenderDev->Release();

        enumerator->Release();
    }

    if (coInit) CoUninitialize();
    return devices;
}

bool AudioCapture::setDevice(const std::wstring& deviceId, bool isLoopback) {
    stop();
    {
        std::lock_guard<std::mutex> lock(deviceMutex);
        selectedDeviceId = deviceId;
        selectedIsLoopback = isLoopback;
    }
    return start();
}

void AudioCapture::captureLoop() {
    timeBeginPeriod(1);

    // Elevate audio thread priority to real-time MMCSS
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    DWORD mmcssTaskIndex = 0;
    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIndex);
    if (!hMmcss) {
        hMmcss = AvSetMmThreadCharacteristicsW(L"Audio", &mmcssTaskIndex);
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool coInitialized = SUCCEEDED(hr);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    WAVEFORMATEX* waveFormat = nullptr;
    HANDLE hAudioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    bool wasapiSuccess = false;
    bool isEventDriven = false;
    bool isLoopbackMode = false;

    do {
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
        if (FAILED(hr)) break;

        std::wstring targetId;
        bool targetIsLoopback = false;
        {
            std::lock_guard<std::mutex> lock(deviceMutex);
            targetId = selectedDeviceId;
            targetIsLoopback = selectedIsLoopback;
        }

        if (targetId == L"default_loopback") {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
            isLoopbackMode = true;
        } else if (targetId == L"default_capture") {
            hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
            isLoopbackMode = false;
        } else if (!targetId.empty()) {
            hr = enumerator->GetDevice(targetId.c_str(), &device);
        }

        if (FAILED(hr) || !device) {
            hr = enumerator->GetDefaultAudioEndpoint(targetIsLoopback ? eRender : eCapture, eConsole, &device);
            if (FAILED(hr) || !device) {
                hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
            }
        }
        if (FAILED(hr) || !device) break;

        // Auto-detect whether device is render (loopback) or capture
        IMMEndpoint* pEndpoint = nullptr;
        if (SUCCEEDED(device->QueryInterface(__uuidof(IMMEndpoint), (void**)&pEndpoint)) && pEndpoint) {
            EDataFlow flow;
            if (SUCCEEDED(pEndpoint->GetDataFlow(&flow))) {
                if (flow == eRender) {
                    isLoopbackMode = true;
                }
            }
            pEndpoint->Release();
        }

        // Retrieve friendly name
        IPropertyStore* pProps = nullptr;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &pv))) {
                if (pv.vt == VT_LPWSTR && pv.pwszVal) {
                    std::lock_guard<std::mutex> lock(deviceMutex);
                    activeDeviceName = pv.pwszVal;
                    activeIsLoopback = isLoopbackMode;
                }
                PropVariantClear(&pv);
            }
            pProps->Release();
        }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient);
        if (FAILED(hr)) break;

        hr = audioClient->GetMixFormat(&waveFormat);
        if (FAILED(hr) || !waveFormat) break;

        deviceSampleRate = waveFormat->nSamplesPerSec;
        channels = waveFormat->nChannels;

        DWORD streamFlags = AUDCLNT_STREAMFLAGS_NOPERSIST;
        if (isLoopbackMode) {
            streamFlags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        }

        // Event-driven mode with 100ms shared buffer
        REFERENCE_TIME bufferDuration = 1000000; // 100 ms
        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                     streamFlags | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     bufferDuration, 0, waveFormat, nullptr);
        if (SUCCEEDED(hr) && hAudioEvent) {
            hr = audioClient->SetEventHandle(hAudioEvent);
            if (SUCCEEDED(hr)) {
                isEventDriven = true;
            }
        }

        if (!isEventDriven) {
            hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                         streamFlags,
                                         bufferDuration, 0, waveFormat, nullptr);
            if (FAILED(hr)) break;
        }

        hr = audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClient);
        if (FAILED(hr)) break;

        hr = audioClient->Start();
        if (FAILED(hr)) break;

        wasapiSuccess = true;
    } while (false);

    if (!wasapiSuccess) {
        usingFallbackGenerator = true;
        {
            std::lock_guard<std::mutex> lock(deviceMutex);
            activeDeviceName = L"Virtual Audio Test Generator";
            activeIsLoopback = false;
        }
        deviceSampleRate = 48000;
        channels = 2;
        audioBuffer.setDuration(audioBuffer.getDuration(), INTERNAL_SAMPLE_RATE);
        runGeneratorFallback();
    } else {
        usingFallbackGenerator = false;
        // The rolling audio buffer always operates at pristine studio-standard 48,000 Hz
        audioBuffer.setDuration(audioBuffer.getDuration(), INTERNAL_SAMPLE_RATE);

        StreamingResampler resampler;
        resampler.reset(deviceSampleRate, INTERNAL_SAMPLE_RATE);

        std::vector<float> rawLeft;
        std::vector<float> rawRight;
        std::vector<float> resampledLeft;
        std::vector<float> resampledRight;

        bool isFloat = false;
        bool isPcm = false;
        WORD bitsPerSample = waveFormat->wBitsPerSample;

        if (waveFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            isFloat = true;
        } else if (waveFormat->wFormatTag == WAVE_FORMAT_PCM) {
            isPcm = true;
        } else if (waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            WAVEFORMATEXTENSIBLE* pExt = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(waveFormat);
            if (IsEqualGUID(pExt->SubFormat, GUID_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
                isFloat = true;
            } else if (IsEqualGUID(pExt->SubFormat, GUID_KSDATAFORMAT_SUBTYPE_PCM)) {
                isPcm = true;
            }
        }

        // For desktop loopback capture, activate a silent background playback stream
        // on the same audio render device so WASAPI's audio engine is kept active
        // and delivering packets continuously even when the Windows desktop is quiet.
        IAudioClient* silentRenderClient = nullptr;
        IAudioRenderClient* silentRenderService = nullptr;
        if (isLoopbackMode && device) {
            if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&silentRenderClient))) {
                REFERENCE_TIME silentDuration = 1000000; // 100 ms
                if (SUCCEEDED(silentRenderClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, silentDuration, 0, waveFormat, nullptr))) {
                    if (SUCCEEDED(silentRenderClient->GetService(__uuidof(IAudioRenderClient), (void**)&silentRenderService))) {
                        UINT32 bufFrames = 0;
                        if (SUCCEEDED(silentRenderClient->GetBufferSize(&bufFrames)) && bufFrames > 0) {
                            BYTE* pSilentData = nullptr;
                            if (SUCCEEDED(silentRenderService->GetBuffer(bufFrames, &pSilentData)) && pSilentData) {
                                silentRenderService->ReleaseBuffer(bufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
                            }
                        }
                        silentRenderClient->Start();
                    }
                }
            }
        }

        HANDLE waitHandles[2] = { hAudioEvent, static_cast<HANDLE>(hStopEvent) };
        auto lastPacketTime = std::chrono::steady_clock::now();

        while (running.load()) {
            if (isEventDriven) {
                // Wait up to 20ms for incoming audio packet or stop event
                DWORD waitRes = WaitForMultipleObjects(2, waitHandles, FALSE, 20);
                if (!running.load() || waitRes == (WAIT_OBJECT_0 + 1)) break;
            } else {
                Sleep(1);
            }

            size_t packetsRead = 0;

            // Drain ALL available packets in a tight loop to guarantee zero underruns
            while (running.load()) {
                UINT32 packetLength = 0;
                hr = captureClient->GetNextPacketSize(&packetLength);
                if (FAILED(hr) || packetLength == 0) break;

                BYTE* pData = nullptr;
                UINT32 numFrames = 0;
                DWORD flags = 0;

                hr = captureClient->GetBuffer(&pData, &numFrames, &flags, nullptr, nullptr);
                if (SUCCEEDED(hr)) {
                    if (numFrames > 0 && pData) {
                        packetsRead++;
                        lastPacketTime = std::chrono::steady_clock::now();
                        rawLeft.resize(numFrames);
                        rawRight.resize(numFrames);

                        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                            std::fill(rawLeft.begin(), rawLeft.end(), 0.0f);
                            std::fill(rawRight.begin(), rawRight.end(), 0.0f);
                        } else if (isFloat && bitsPerSample == 32) {
                            const float* pFloats = reinterpret_cast<const float*>(pData);
                            for (UINT32 i = 0; i < numFrames; ++i) {
                                rawLeft[i] = pFloats[i * channels];
                                rawRight[i] = (channels > 1) ? pFloats[i * channels + 1] : rawLeft[i];
                            }
                        } else if (isPcm && bitsPerSample == 16) {
                            const int16_t* pShorts = reinterpret_cast<const int16_t*>(pData);
                            for (UINT32 i = 0; i < numFrames; ++i) {
                                rawLeft[i] = pShorts[i * channels] / 32768.0f;
                                rawRight[i] = (channels > 1) ? (pShorts[i * channels + 1] / 32768.0f) : rawLeft[i];
                            }
                        } else if (isPcm && bitsPerSample == 24) {
                            for (UINT32 i = 0; i < numFrames; ++i) {
                                size_t offL = (i * channels) * 3;
                                int32_t valL = static_cast<int32_t>(pData[offL] | (pData[offL + 1] << 8) | (pData[offL + 2] << 16));
                                if (valL & 0x800000) valL |= 0xFF000000;
                                rawLeft[i] = valL / 8388608.0f;

                                if (channels > 1) {
                                    size_t offR = (i * channels + 1) * 3;
                                    int32_t valR = static_cast<int32_t>(pData[offR] | (pData[offR + 1] << 8) | (pData[offR + 2] << 16));
                                    if (valR & 0x800000) valR |= 0xFF000000;
                                    rawRight[i] = valR / 8388608.0f;
                                } else {
                                    rawRight[i] = rawLeft[i];
                                }
                            }
                        } else if (isPcm && bitsPerSample == 32) {
                            const int32_t* pInts = reinterpret_cast<const int32_t*>(pData);
                            for (UINT32 i = 0; i < numFrames; ++i) {
                                rawLeft[i] = pInts[i * channels] / 2147483648.0f;
                                rawRight[i] = (channels > 1) ? (pInts[i * channels + 1] / 2147483648.0f) : rawLeft[i];
                            }
                        }

                        // Resample / decimate to standard 48,000 Hz float stereo
                        resampler.process(rawLeft.data(), rawRight.data(), numFrames, resampledLeft, resampledRight);

                        if (!resampledLeft.empty()) {
                            const float* chs[2] = { resampledLeft.data(), resampledRight.data() };
                            audioBuffer.writeSamples(chs, 2, resampledLeft.size());
                        }
                    }
                    captureClient->ReleaseBuffer(numFrames);
                }
            }

            // In loopback mode, if no audio packets arrived within 15ms (e.g. system quiet),
            // synthesize silent frames matching elapsed wall-clock time so recording timeline
            // advances with zero pause/stutter.
            if (isLoopbackMode && packetsRead == 0) {
                auto nowTime = std::chrono::steady_clock::now();
                auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(nowTime - lastPacketTime).count();
                if (elapsedMs >= 15) {
                    size_t silentFrames = static_cast<size_t>((INTERNAL_SAMPLE_RATE * elapsedMs) / 1000);
                    if (silentFrames > 0 && silentFrames < INTERNAL_SAMPLE_RATE) {
                        std::vector<float> zeros(silentFrames, 0.0f);
                        const float* chs[2] = { zeros.data(), zeros.data() };
                        audioBuffer.writeSamples(chs, 2, silentFrames);
                        lastPacketTime = nowTime;
                    }
                }
            }
        }

        if (silentRenderClient) {
            silentRenderClient->Stop();
            if (silentRenderService) silentRenderService->Release();
            silentRenderClient->Release();
        }
        if (audioClient) audioClient->Stop();
    }

    if (hAudioEvent) CloseHandle(hAudioEvent);
    if (waveFormat) CoTaskMemFree(waveFormat);
    if (captureClient) captureClient->Release();
    if (audioClient) audioClient->Release();
    if (device) device->Release();
    if (enumerator) enumerator->Release();
    if (coInitialized) CoUninitialize();

    if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
    timeEndPeriod(1);
}

void AudioCapture::runGeneratorFallback() {
    const size_t blockSize = 256;
    std::vector<float> left(blockSize);
    std::vector<float> right(blockSize);
    const float* chs[2] = { left.data(), right.data() };

    double phase = 0.0;
    const double freq = 440.0;
    const double dt = 1.0 / INTERNAL_SAMPLE_RATE;

    while (running.load()) {
        auto t0 = std::chrono::high_resolution_clock::now();
        for (size_t i = 0; i < blockSize; ++i) {
            float s = 0.35f * static_cast<float>(std::sin(2.0 * 3.141592653589793 * phase));
            left[i] = s;
            right[i] = s;
            phase += freq * dt;
            if (phase >= 1.0) phase -= 1.0;
        }

        audioBuffer.writeSamples(chs, 2, blockSize);

        auto t1 = std::chrono::high_resolution_clock::now();
        double elapsedMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double sleepMs = (static_cast<double>(blockSize) / INTERNAL_SAMPLE_RATE) * 1000.0 - elapsedMs;
        if (sleepMs > 1.0) {
            Sleep(static_cast<DWORD>(sleepMs));
        }
    }
}

} // namespace RollingSampler
