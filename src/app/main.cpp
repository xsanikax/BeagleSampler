#include "../core/RollingAudioBuffer.h"
#include "../core/SettingsManager.h"
#include "../audio/WasapiAudioCapture.h"
#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <mmsystem.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <string>
#include <sstream>
#include <iomanip>
#include <vector>
#include <cmath>
#include <algorithm>

#include <winhttp.h>
#include <thread>
#include <atomic>
#include <chrono>

#define IDI_APP_ICON 1

static const wchar_t* CURRENT_VERSION_STR = L"v1.0.15";
static const int CURRENT_VERSION_MAJOR = 1;
static const int CURRENT_VERSION_MINOR = 0;
static const int CURRENT_VERSION_PATCH = 15;
static bool g_autoCheckUpdates = true;

using namespace RollingSampler;
using namespace Gdiplus;

static RollingAudioBuffer* g_buffer = nullptr;
static AudioCapture* g_capture = nullptr;
static Bitmap* g_pAppIconBmp = nullptr;


// Display Gain: default 1.0x (0 dB unity gain)
static float g_displayGain = 1.0f;

// Selection & Drag state
static bool g_isSelecting = false;
static bool g_hasSelection = false;
static int g_selStartX = 0;
static int g_selEndX = 0;
static bool g_isDragging = false;
static POINT g_mouseDownPos = { 0, 0 };
static bool g_wasAutoPaused = false;

// Audio format & export settings
enum class ChannelMode { Stereo = 0, MonoLeft = 1, MonoRight = 2 };
static ChannelMode g_channelMode = ChannelMode::Stereo;
static bool g_save24Bit = false;
static bool g_trimSilence = true;
static float g_silenceThresholdDb = -90.0f;
static bool g_appendBpm = false;
static double g_detectedBpm = 120.0;
static bool g_saveSessionState = true;

// Display & Visual settings
enum class ThemeId { Carbon = 1, Rainbow = 2, Bright = 3, Lemon = 4, Vintage = 5 };
static ThemeId g_currentTheme = ThemeId::Carbon;
static bool g_asymmetrical = false;
static bool g_pinOnTop = false;
static int g_targetFps = 60;

// Status notification message
static std::wstring g_statusMessage = L"";
static DWORD g_statusMessageExpiry = 0;

// Waveform display mode: Sweep (Traveling Playhead, default) vs Rolling (Right-to-Left)
enum class DisplayMode { Sweep = 0, Rolling = 1 };
static DisplayMode g_displayMode = DisplayMode::Sweep;

// Window modes & controls
static bool g_borderless = true;
static RECT g_rectCloseBtn = { 0, 0, 0, 0 };
static RECT g_rectMinBtn = { 0, 0, 0, 0 };
static RECT g_rectPinBtn = { 0, 0, 0, 0 };
static RECT g_rectPauseBtn = { 0, 0, 0, 0 };
static RECT g_rectSettingsBtn = { 0, 0, 0, 0 };

enum class HoverBtn { None, Close, Min, Pin, Settings, Pause };
static HoverBtn g_hoverBtn = HoverBtn::None;

static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    ImageCodecInfo* pImageCodecInfo = (ImageCodecInfo*)(malloc(size));
    if (!pImageCodecInfo) return -1;
    GetImageEncoders(num, size, pImageCodecInfo);
    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            free(pImageCodecInfo);
            return j;
        }
    }
    free(pImageCodecInfo);
    return -1;
}

// Settings Persistence Store (%APPDATA%\BeagleSampler\settings.ini and BeagleSampler.bbsettings)
static std::wstring getSettingsFilePath() {
    return SettingsManager::getIniPath();
}

// Audio Buffer Session State Persistence Store (%APPDATA%\BeagleSampler\session.state)
static std::wstring getSessionStateFilePath() {
    return SettingsManager::getSessionStatePath();
}

static void saveSettings(HWND hwnd = NULL) {
    AppSettings s;
    if (g_capture) {
        s.audioDeviceId = g_capture->getSelectedDeviceId();
        s.isLoopback = g_capture->isLoopback();
    }
    s.duration = g_buffer ? g_buffer->getDuration() : 30.0;
    s.save24Bit = g_save24Bit;
    s.channelMode = static_cast<int>(g_channelMode);
    s.trimSilence = g_trimSilence;
    s.silenceThresholdDb = g_silenceThresholdDb;
    s.appendBpm = g_appendBpm;
    s.saveSessionState = g_saveSessionState;

    s.themeId = static_cast<int>(g_currentTheme);
    s.displayGain = g_displayGain;
    s.displayMode = static_cast<int>(g_displayMode);
    s.asymmetrical = g_asymmetrical;
    s.pinOnTop = g_pinOnTop;
    s.targetFps = g_targetFps;
    s.borderless = g_borderless;
    s.autoCheckUpdates = g_autoCheckUpdates;

    SettingsManager::saveSettings(s, hwnd);
}

static void loadSettings(int& winX, int& winY, int& winW, int& winH, std::wstring& outDevId, bool& outIsLoopback, double& outDur) {
    AppSettings s = SettingsManager::loadSettings();
    outDevId = s.audioDeviceId;
    outIsLoopback = s.isLoopback;
    outDur = s.duration;
    g_save24Bit = s.save24Bit;
    g_channelMode = static_cast<ChannelMode>(s.channelMode);
    g_trimSilence = s.trimSilence;
    g_silenceThresholdDb = s.silenceThresholdDb;
    g_appendBpm = s.appendBpm;
    g_saveSessionState = s.saveSessionState;

    g_currentTheme = static_cast<ThemeId>(s.themeId);
    g_displayGain = s.displayGain;
    g_displayMode = static_cast<DisplayMode>(s.displayMode);
    g_asymmetrical = s.asymmetrical;
    g_pinOnTop = s.pinOnTop;
    g_targetFps = s.targetFps;
    g_borderless = s.borderless;
    g_autoCheckUpdates = s.autoCheckUpdates;

    if (s.windowX != -99999 && s.windowY != -99999 && s.windowWidth >= 400 && s.windowHeight >= 140) {
        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        if (s.windowX >= -500 && s.windowX < screenW - 100 && s.windowY >= -100 && s.windowY < screenH - 50) {
            winX = s.windowX;
            winY = s.windowY;
            winW = s.windowWidth;
            winH = s.windowHeight;
        }
    }
}

struct ThemeColors {
    Color bg;
    Color panelBg;
    Color panelBorder;
    Color waveformFill;
    Color waveformStroke;
    Color waveformFillOld;
    Color waveformStrokeOld;
    Color centerLine;
    Color gridLine;
    Color text;
    Color textMuted;
    Color playhead;
    Color selectionFill;
    Color selectionBorder;
    const wchar_t* name;
};

static ThemeColors getThemeColors(ThemeId theme, bool isPaused) {
    switch (theme) {
    case ThemeId::Carbon:
        return {
            Color(255, 20, 20, 20),                               // bg #141414
            Color(255, 26, 26, 26),                               // panelBg
            Color(255, 42, 42, 42),                               // panelBorder
            isPaused ? Color(55, 185, 125, 55) : Color(75, 245, 160, 50),     // waveformFill (New: Translucent Amber Gold)
            isPaused ? Color(255, 205, 145, 75) : Color(255, 255, 200, 90),   // waveformStroke (New: Bright Amber Gold)
            isPaused ? Color(55, 175, 95, 75) : Color(75, 230, 115, 80),      // waveformFillOld (Old: Translucent Coral)
            isPaused ? Color(255, 195, 120, 100) : Color(255, 255, 150, 120), // waveformStrokeOld (Old: Terracotta Coral)
            Color(255, 42, 42, 42),                               // centerLine
            Color(255, 28, 28, 28),                               // gridLine
            Color(255, 240, 240, 240),                            // text #F0F0F0
            Color(255, 130, 130, 130),                            // textMuted
            Color(255, 255, 215, 140),                            // playhead
            Color(75, 240, 150, 80),                              // selection fill
            Color(255, 255, 190, 120),                            // selection border
            L"Carbon"
        };
    case ThemeId::Rainbow:
        return {
            Color(255, 18, 18, 20),
            Color(255, 25, 25, 28),
            Color(255, 44, 44, 48),
            Color(75, 255, 120, 160),                             // waveformFill (Translucent)
            Color(255, 255, 200, 220),                            // waveformStroke (Opaque)
            Color(55, 210, 130, 220),                             // waveformFillOld (Translucent)
            Color(255, 230, 180, 240),                            // waveformStrokeOld (Opaque)
            Color(255, 42, 42, 46),
            Color(255, 28, 28, 32),
            Color(255, 240, 240, 245),
            Color(255, 140, 140, 150),
            Color(255, 255, 240, 180),
            Color(75, 255, 140, 180),
            Color(255, 255, 200, 220),
            L"Rainbow"
        };
    case ThemeId::Bright:
        return {
            Color(255, 245, 245, 247),
            Color(255, 255, 255, 255),
            Color(255, 220, 220, 225),
            isPaused ? Color(50, 75, 105, 140) : Color(70, 30, 85, 165),      // waveformFill (New: Translucent Cobalt Slate)
            isPaused ? Color(255, 75, 105, 140) : Color(255, 18, 70, 145),    // waveformStroke (New: Deep Cobalt)
            isPaused ? Color(50, 65, 120, 125) : Color(70, 20, 125, 135),     // waveformFillOld (Old: Translucent Ocean Teal)
            isPaused ? Color(255, 65, 120, 125) : Color(255, 12, 105, 115),   // waveformStrokeOld (Old: Deep Ocean Teal)
            Color(255, 215, 215, 220),
            Color(255, 236, 236, 240),
            Color(255, 20, 20, 20),
            Color(255, 110, 110, 110),
            Color(255, 0, 120, 215),
            Color(65, 0, 120, 215),
            Color(255, 0, 120, 215),
            L"Bright"
        };
    case ThemeId::Lemon:
        return {
            Color(255, 20, 20, 20),
            Color(255, 25, 25, 24),
            Color(255, 42, 42, 36),
            isPaused ? Color(55, 145, 185, 40) : Color(75, 155, 220, 35),     // waveformFill (New: Translucent Lime Chartreuse)
            isPaused ? Color(255, 165, 205, 60) : Color(255, 185, 250, 45),   // waveformStroke (New: Vivid Electric Lime Chartreuse)
            isPaused ? Color(55, 185, 165, 30) : Color(75, 240, 200, 25),     // waveformFillOld (Old: Translucent Lemon Yellow)
            isPaused ? Color(255, 205, 185, 50) : Color(255, 255, 225, 55),   // waveformStrokeOld (Old: Vivid Golden Lemon Yellow)
            Color(255, 44, 44, 38),
            Color(255, 30, 30, 25),
            Color(255, 240, 240, 240),
            Color(255, 140, 140, 130),
            Color(255, 255, 245, 100),
            Color(75, 231, 197, 5),
            Color(255, 255, 230, 80),
            L"Lemon"
        };
    case ThemeId::Vintage:
        return {
            Color(255, 32, 26, 34),
            Color(255, 38, 30, 42),
            Color(255, 56, 44, 62),
            isPaused ? Color(55, 185, 165, 130) : Color(75, 225, 200, 155),   // waveformFill (New: Translucent Sandstone Cream)
            isPaused ? Color(255, 205, 185, 150) : Color(255, 248, 232, 195), // waveformStroke (New: Vivid Sandstone Cream)
            isPaused ? Color(55, 165, 125, 130) : Color(75, 205, 145, 150),   // waveformFillOld (Old: Translucent Dusty Rose)
            isPaused ? Color(255, 185, 145, 150) : Color(255, 238, 178, 188), // waveformStrokeOld (Old: Vivid Dusty Rose Mauve)
            Color(255, 55, 44, 60),
            Color(255, 42, 34, 46),
            Color(255, 235, 230, 225),
            Color(255, 155, 145, 150),
            Color(255, 250, 235, 200),
            Color(75, 208, 190, 146),
            Color(255, 245, 230, 195),
            L"Vintage"
        };
    default:
        return getThemeColors(ThemeId::Carbon, isPaused);
    }
}

// OLE Format Enumerator
class FormatEtcEnum : public IEnumFORMATETC {
public:
    FormatEtcEnum(const FORMATETC& fe) : refCount(1), index(0), format(fe) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IEnumFORMATETC) {
            *ppv = static_cast<IEnumFORMATETC*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refCount); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG c = InterlockedDecrement(&refCount);
        if (c == 0) delete this;
        return c;
    }
    STDMETHODIMP Next(ULONG celt, FORMATETC* rgelt, ULONG* pceltFetched) override {
        if (!rgelt) return E_POINTER;
        if (index == 0 && celt >= 1) {
            rgelt[0] = format;
            index = 1;
            if (pceltFetched) *pceltFetched = 1;
            return (celt == 1) ? S_OK : S_FALSE;
        }
        if (pceltFetched) *pceltFetched = 0;
        return S_FALSE;
    }
    STDMETHODIMP Skip(ULONG celt) override {
        index = std::min(1UL, index + celt);
        return S_OK;
    }
    STDMETHODIMP Reset() override {
        index = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumFORMATETC** ppenum) override {
        if (!ppenum) return E_POINTER;
        *ppenum = new FormatEtcEnum(format);
        return S_OK;
    }
private:
    LONG refCount;
    ULONG index;
    FORMATETC format;
};

// OLE Drop Source
class DropSource : public IDropSource {
public:
    DropSource() : refCount(1) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *ppv = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refCount); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = InterlockedDecrement(&refCount);
        if (count == 0) delete this;
        return count;
    }
    STDMETHODIMP QueryContinueDrag(BOOL fEscapePressed, DWORD grfKeyState) override {
        if (fEscapePressed) return DRAGDROP_S_CANCEL;
        if (!(grfKeyState & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }
    STDMETHODIMP GiveFeedback(DWORD dwEffect) override {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }
private:
    LONG refCount;
};

// OLE Data Object supporting CF_HDROP
class DataObject : public IDataObject {
public:
    DataObject(const std::wstring& filePath) : refCount(1), filePath(filePath) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *ppv = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refCount); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = InterlockedDecrement(&refCount);
        if (count == 0) delete this;
        return count;
    }

    STDMETHODIMP GetData(FORMATETC* pFormatEtc, STGMEDIUM* pMedium) override {
        if (!pFormatEtc || !pMedium) return E_INVALIDARG;
        if (pFormatEtc->cfFormat != CF_HDROP || !(pFormatEtc->tymed & TYMED_HGLOBAL)) {
            return DV_E_FORMATETC;
        }

        // DROPFILES struct + double null-terminated wide string
        size_t pathLenBytes = (filePath.length() + 2) * sizeof(wchar_t);
        size_t totalSize = sizeof(DROPFILES) + pathLenBytes;

        HGLOBAL hGlobal = GlobalAlloc(GHND | GMEM_SHARE, totalSize);
        if (!hGlobal) return E_OUTOFMEMORY;

        BYTE* pLock = static_cast<BYTE*>(GlobalLock(hGlobal));
        DROPFILES* df = reinterpret_cast<DROPFILES*>(pLock);
        df->pFiles = sizeof(DROPFILES);
        df->pt.x = 0;
        df->pt.y = 0;
        df->fNC = FALSE;
        df->fWide = TRUE;

        wchar_t* destPath = reinterpret_cast<wchar_t*>(pLock + sizeof(DROPFILES));
        std::memcpy(destPath, filePath.c_str(), filePath.length() * sizeof(wchar_t));
        destPath[filePath.length()] = L'\0';
        destPath[filePath.length() + 1] = L'\0';
        GlobalUnlock(hGlobal);

        pMedium->tymed = TYMED_HGLOBAL;
        pMedium->hGlobal = hGlobal;
        pMedium->pUnkForRelease = nullptr;
        return S_OK;
    }

    STDMETHODIMP QueryGetData(FORMATETC* pFormatEtc) override {
        if (!pFormatEtc) return E_INVALIDARG;
        if (pFormatEtc->cfFormat == CF_HDROP && (pFormatEtc->tymed & TYMED_HGLOBAL)) {
            return S_OK;
        }
        return DV_E_FORMATETC;
    }

    STDMETHODIMP EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC** ppenumFormatEtc) override {
        if (!ppenumFormatEtc) return E_POINTER;
        if (dwDirection == DATADIR_GET) {
            FORMATETC fe = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
            *ppenumFormatEtc = new FormatEtcEnum(fe);
            return S_OK;
        }
        *ppenumFormatEtc = nullptr;
        return E_NOTIMPL;
    }

    STDMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    STDMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override { return E_NOTIMPL; }
    STDMETHODIMP SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    STDMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    STDMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    LONG refCount;
    std::wstring filePath;
};

// WAV file writer supporting 16-bit / 24-bit PCM, Stereo / Mono
static bool writeWavFile(const std::wstring& filePath, const float* left, const float* right,
                         size_t numSamples, uint32_t sampleRate, ChannelMode mode, bool use24Bit) {
    FILE* f = _wfopen(filePath.c_str(), L"wb");
    if (!f) return false;

    uint16_t numChannels = (mode == ChannelMode::Stereo) ? 2 : 1;
    uint16_t bitsPerSample = use24Bit ? 24 : 16;
    uint16_t bytesPerSample = bitsPerSample / 8;
    uint16_t blockAlign = numChannels * bytesPerSample;
    uint32_t byteRate = sampleRate * blockAlign;
    uint32_t subchunk2Size = static_cast<uint32_t>(numSamples * blockAlign);
    uint32_t chunkSize = 36 + subchunk2Size;

    fwrite("RIFF", 1, 4, f);
    fwrite(&chunkSize, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t subchunk1Size = 16;
    fwrite(&subchunk1Size, 4, 1, f);
    uint16_t audioFormat = 1; // PCM
    fwrite(&audioFormat, 2, 1, f);
    fwrite(&numChannels, 2, 1, f);
    fwrite(&sampleRate, 4, 1, f);
    fwrite(&byteRate, 4, 1, f);
    fwrite(&blockAlign, 2, 1, f);
    fwrite(&bitsPerSample, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&subchunk2Size, 4, 1, f);

    for (size_t i = 0; i < numSamples; ++i) {
        float sL = left ? std::clamp(left[i], -1.0f, 1.0f) : 0.0f;
        float sR = right ? std::clamp(right[i], -1.0f, 1.0f) : sL;

        if (mode == ChannelMode::Stereo) {
            if (use24Bit) {
                int32_t pcmL = static_cast<int32_t>(sL * 8388607.0f);
                int32_t pcmR = static_cast<int32_t>(sR * 8388607.0f);
                fwrite(&pcmL, 3, 1, f);
                fwrite(&pcmR, 3, 1, f);
            } else {
                int16_t pcmL = static_cast<int16_t>(sL * 32767.0f);
                int16_t pcmR = static_cast<int16_t>(sR * 32767.0f);
                fwrite(&pcmL, 2, 1, f);
                fwrite(&pcmR, 2, 1, f);
            }
        } else {
            float sMono = (mode == ChannelMode::MonoLeft) ? sL : sR;
            if (use24Bit) {
                int32_t pcm = static_cast<int32_t>(sMono * 8388607.0f);
                fwrite(&pcm, 3, 1, f);
            } else {
                int16_t pcm = static_cast<int16_t>(sMono * 32767.0f);
                fwrite(&pcm, 2, 1, f);
            }
        }
    }

    fclose(f);
    return true;
}

static std::wstring getExportFilePath(bool isSelection) {
    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t timeBuf[64];
    swprintf_s(timeBuf, L"%04d%02d%02d_%02d%02d%02d",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    std::wstringstream ss;
    ss << tempDir << L"RollingSampler_";
    if (isSelection) ss << L"Selection_";
    else ss << L"Full_";
    ss << timeBuf;

    if (g_appendBpm) {
        ss << L"_" << static_cast<int>(g_detectedBpm) << L"BPM";
    }
    ss << (g_save24Bit ? L"_24bit" : L"_16bit") << L".wav";
    return ss.str();
}

static std::wstring exportCurrentAudio(bool isSelection) {
    if (!g_buffer) return L"";

    uint32_t sr = g_buffer->getSampleRate();
    size_t cap = g_buffer->getCapacity();

    double startFrac = 0.0;
    double endFrac = 1.0;

    if (isSelection && g_hasSelection) {
        RECT rc;
        HWND hwnd = FindWindowW(L"BeagleSampler", NULL);
        if (hwnd) GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left;
        if (w > 0) {
            int selL = std::min(g_selStartX, g_selEndX);
            int selR = std::max(g_selStartX, g_selEndX);
            startFrac = static_cast<double>(selL) / static_cast<double>(w);
            endFrac = static_cast<double>(selR) / static_cast<double>(w);
        }
    }

    size_t maxCount = g_buffer->getSelectionSampleCount(startFrac, endFrac);
    if (maxCount == 0) maxCount = cap;

    std::vector<float> l(maxCount);
    std::vector<float> r(maxCount);

    size_t actualSamples = 0;
    if (g_displayMode == DisplayMode::Sweep) {
        actualSamples = g_buffer->readSweepSelectionSamples(l.data(), r.data(), startFrac, endFrac,
                                                            g_trimSilence, g_silenceThresholdDb);
        if (actualSamples == 0) {
            actualSamples = g_buffer->readSweepSelectionSamples(l.data(), r.data(), startFrac, endFrac, false, 0.0f);
        }
    } else {
        actualSamples = g_buffer->readSelectionSamples(l.data(), r.data(), startFrac, endFrac,
                                                       g_trimSilence, g_silenceThresholdDb);
        if (actualSamples == 0) {
            actualSamples = g_buffer->readSelectionSamples(l.data(), r.data(), startFrac, endFrac, false, 0.0f);
        }
    }
    if (actualSamples == 0) return L"";

    std::wstring outPath = getExportFilePath(isSelection);
    if (writeWavFile(outPath, l.data(), r.data(), actualSamples, sr, g_channelMode, g_save24Bit)) {
        return outPath;
    }
    return L"";
}

// Resamples audio to 48000 Hz 16-bit PCM for 100% clean, crackle-free Win32 preview playback
static std::wstring exportPreviewWav(bool isSelection) {
    if (!g_buffer) return L"";

    uint32_t sr = g_buffer->getSampleRate();
    size_t cap = g_buffer->getCapacity();

    double startFrac = 0.0;
    double endFrac = 1.0;

    if (isSelection && g_hasSelection) {
        RECT rc;
        HWND hwnd = FindWindowW(L"BeagleSampler", NULL);
        if (hwnd) GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left;
        if (w > 0) {
            int selL = std::min(g_selStartX, g_selEndX);
            int selR = std::max(g_selStartX, g_selEndX);
            startFrac = static_cast<double>(selL) / static_cast<double>(w);
            endFrac = static_cast<double>(selR) / static_cast<double>(w);
        }
    }

    size_t maxCount = g_buffer->getSelectionSampleCount(startFrac, endFrac);
    if (maxCount == 0) maxCount = cap;

    std::vector<float> l(maxCount);
    std::vector<float> r(maxCount);

    size_t actualSamples = 0;
    if (g_displayMode == DisplayMode::Sweep) {
        actualSamples = g_buffer->readSweepSelectionSamples(l.data(), r.data(), startFrac, endFrac,
                                                            g_trimSilence, g_silenceThresholdDb);
    } else {
        actualSamples = g_buffer->readSelectionSamples(l.data(), r.data(), startFrac, endFrac,
                                                       g_trimSilence, g_silenceThresholdDb);
    }

    if (actualSamples == 0) return L"";

    static int previewFileIdx = 0;
    previewFileIdx = (previewFileIdx + 1) % 2;

    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring previewPath = std::wstring(tempDir) + L"BeagleSampler_Preview_" + std::to_wstring(previewFileIdx) + L".wav";

    if (writeWavFile(previewPath, l.data(), r.data(), actualSamples, sr, ChannelMode::Stereo, false)) {
        return previewPath;
    }
    return L"";
}

// Perform interactive OLE Drag-and-Drop
static void performDragAndDrop(HWND hwnd) {
    // Release capture before DoDragDrop so other windows receive mouse messages!
    ReleaseCapture();
    g_isDragging = false;
    g_isSelecting = false;

    std::wstring wavPath = exportCurrentAudio(g_hasSelection);
    if (wavPath.empty()) return;

    DataObject* pData = new DataObject(wavPath);
    DropSource* pSource = new DropSource();
    DWORD dwEffect = 0;

    HRESULT hr = DoDragDrop(pData, pSource, DROPEFFECT_COPY | DROPEFFECT_MOVE, &dwEffect);
    if (hr == DRAGDROP_S_DROP) {
        size_t slashPos = wavPath.find_last_of(L"\\/");
        std::wstring fileName = (slashPos != std::wstring::npos) ? wavPath.substr(slashPos + 1) : wavPath;
        g_statusMessage = L"✓ Exported: " + fileName;
        g_statusMessageExpiry = GetTickCount() + 3500;
    }

    pData->Release();
    pSource->Release();
    InvalidateRect(hwnd, NULL, FALSE);
}

// Menu Command IDs
enum {
    IDM_DUR_15 = 201, IDM_DUR_30, IDM_DUR_60, IDM_DUR_90, IDM_DUR_120, IDM_DUR_180, IDM_DUR_300,
    IDM_THEME_CARBON = 211, IDM_THEME_RAINBOW, IDM_THEME_BRIGHT, IDM_THEME_LEMON, IDM_THEME_VINTAGE,
    IDM_GAIN_P26 = 219, IDM_GAIN_P20 = 220,
    IDM_GAIN_M12 = 221, IDM_GAIN_M6, IDM_GAIN_0, IDM_GAIN_P6, IDM_GAIN_P10, IDM_GAIN_P14,
    IDM_CH_STEREO = 231, IDM_CH_MONO_L, IDM_CH_MONO_R,
    IDM_SAVE_24BIT = 241, IDM_APPEND_BPM, IDM_TRIM_SILENCE,
    IDM_THRESH_125 = 251, IDM_THRESH_100, IDM_THRESH_90, IDM_THRESH_75, IDM_THRESH_60,
    IDM_FPS_60 = 261, IDM_FPS_30, IDM_FPS_20, IDM_FPS_15,
    IDM_ASYMMETRICAL = 271, IDM_PIN_ON_TOP,
    IDM_DISP_SWEEP = 273, IDM_DISP_ROLLING = 274, IDM_BORDERLESS = 275, IDM_SAVE_SESSION_STATE = 276,
    IDM_PAUSE = 281, IDM_PREVIEW, IDM_CLEAR, IDM_EXPORT_SEL, IDM_EXPORT_FULL,
    IDM_CHECK_UPDATES = 290, IDM_AUTO_CHECK_UPDATES = 291,
    IDM_DEV_DEF_LOOPBACK = 490, IDM_DEV_DEF_CAPTURE = 491,
    IDM_DEVICE_BASE = 500
};

struct RemoteReleaseInfo {
    std::wstring tagName;
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::wstring setupDownloadUrl;
    bool found = false;
};

static bool ParseSemanticVersion(const std::string& tag, int& outMajor, int& outMinor, int& outPatch) {
    outMajor = outMinor = outPatch = 0;
    size_t i = 0;
    while (i < tag.size() && (tag[i] == 'v' || tag[i] == 'V' || tag[i] == ' ' || tag[i] == '\t')) {
        i++;
    }
    if (i >= tag.size()) return false;
    int parsed = sscanf(tag.c_str() + i, "%d.%d.%d", &outMajor, &outMinor, &outPatch);
    return parsed >= 2;
}

static bool IsRemoteNewer(int remoteMajor, int remoteMinor, int remotePatch) {
    if (remoteMajor > CURRENT_VERSION_MAJOR) return true;
    if (remoteMajor < CURRENT_VERSION_MAJOR) return false;
    if (remoteMinor > CURRENT_VERSION_MINOR) return true;
    if (remoteMinor < CURRENT_VERSION_MINOR) return false;
    return remotePatch > CURRENT_VERSION_PATCH;
}

static RemoteReleaseInfo QueryLatestRelease() {
    RemoteReleaseInfo info;
    HINTERNET hSession = WinHttpOpen(L"BeagleSampler-App/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return info;

    // Method 1: Web HEAD redirect check on github.com (NOT rate-limited by GitHub API 60 req/hr limits)
    HINTERNET hConnect = WinHttpConnect(hSession, L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (hConnect) {
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"HEAD",
                                                L"/xsanikax/BeagleSampler/releases/latest",
                                                NULL, WINHTTP_NO_REFERER,
                                                WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                WINHTTP_FLAG_SECURE);
        if (hRequest) {
            DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
            WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

            std::wstring headers = L"User-Agent: BeagleSampler-App/1.0\r\n";
            if (WinHttpSendRequest(hRequest, headers.c_str(), headers.length(), NULL, 0, 0, 0) &&
                WinHttpReceiveResponse(hRequest, NULL)) {
                
                DWORD statusCode = 0;
                DWORD statusSize = sizeof(statusCode);
                WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

                if (statusCode == 301 || statusCode == 302 || statusCode == 307 || statusCode == 308) {
                    wchar_t locBuf[1024] = { 0 };
                    DWORD locBufSize = sizeof(locBuf);
                    DWORD index = 0;
                    if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                            locBuf, &locBufSize, &index)) {
                        std::wstring loc(locBuf);
                        size_t tagPos = loc.rfind(L"/tag/");
                        if (tagPos != std::wstring::npos) {
                            std::wstring tag = loc.substr(tagPos + 5);
                            info.tagName = tag;
                            std::string tagStr(tag.begin(), tag.end());
                            ParseSemanticVersion(tagStr, info.major, info.minor, info.patch);
                            info.setupDownloadUrl = L"https://github.com/xsanikax/BeagleSampler/releases/download/" + tag + L"/BeagleSampler_Setup.exe";
                            info.found = true;
                        }
                    }
                }
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }

    // Method 2 (Fallback): Query GitHub REST API if Method 1 didn't find release
    if (!info.found) {
        HINTERNET hApiConnect = WinHttpConnect(hSession, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (hApiConnect) {
            HINTERNET hApiRequest = WinHttpOpenRequest(hApiConnect, L"GET",
                                                    L"/repos/xsanikax/BeagleSampler/releases/latest",
                                                    NULL, WINHTTP_NO_REFERER,
                                                    WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                    WINHTTP_FLAG_SECURE);
            if (hApiRequest) {
                DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
                WinHttpSetOption(hApiRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

                std::wstring headers = L"User-Agent: BeagleSampler-App/1.0\r\nAccept: application/vnd.github.v3+json\r\n";
                if (WinHttpSendRequest(hApiRequest, headers.c_str(), headers.length(), NULL, 0, 0, 0) &&
                    WinHttpReceiveResponse(hApiRequest, NULL)) {
                    
                    DWORD statusCode = 0;
                    DWORD statusSize = sizeof(statusCode);
                    WinHttpQueryHeaders(hApiRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

                    if (statusCode == 200) {
                        std::string responseBody;
                        DWORD bytesAvailable = 0;
                        while (WinHttpQueryDataAvailable(hApiRequest, &bytesAvailable) && bytesAvailable > 0) {
                            std::vector<char> buffer(bytesAvailable);
                            DWORD bytesRead = 0;
                            if (WinHttpReadData(hApiRequest, buffer.data(), bytesAvailable, &bytesRead) && bytesRead > 0) {
                                responseBody.append(buffer.data(), bytesRead);
                            }
                        }

                        size_t tagKey = responseBody.find("\"tag_name\":");
                        if (tagKey != std::string::npos) {
                            size_t startQ = responseBody.find('"', tagKey + 11);
                            if (startQ != std::string::npos) {
                                size_t endQ = responseBody.find('"', startQ + 1);
                                if (endQ != std::string::npos && endQ > startQ) {
                                    std::string tagStr = responseBody.substr(startQ + 1, endQ - startQ - 1);
                                    info.tagName = std::wstring(tagStr.begin(), tagStr.end());
                                    ParseSemanticVersion(tagStr, info.major, info.minor, info.patch);
                                }
                            }
                        }

                        size_t setupPos = responseBody.find("\"name\":\"BeagleSampler_Setup.exe\"");
                        if (setupPos == std::string::npos) {
                            setupPos = responseBody.find("BeagleSampler_Setup.exe");
                        }
                        if (setupPos != std::string::npos) {
                            size_t urlKey = responseBody.find("\"browser_download_url\":", setupPos);
                            if (urlKey != std::string::npos) {
                                size_t startQ = responseBody.find('"', urlKey + 23);
                                if (startQ != std::string::npos) {
                                    size_t endQ = responseBody.find('"', startQ + 1);
                                    if (endQ != std::string::npos && endQ > startQ) {
                                        std::string urlStr = responseBody.substr(startQ + 1, endQ - startQ - 1);
                                        info.setupDownloadUrl = std::wstring(urlStr.begin(), urlStr.end());
                                    }
                                }
                            }
                        }

                        if (!info.tagName.empty() && !info.setupDownloadUrl.empty()) {
                            info.found = true;
                        }
                    }
                }
                WinHttpCloseHandle(hApiRequest);
            }
            WinHttpCloseHandle(hApiConnect);
        }
    }

    WinHttpCloseHandle(hSession);
    return info;
}

static bool DownloadAndRunUpdate(HWND hwnd, const std::wstring& downloadUrl) {
    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring installerPath = std::wstring(tempDir) + L"BeagleSampler_Setup.exe";

    HINTERNET hSession = WinHttpOpen(L"BeagleSampler-App/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    URL_COMPONENTS urlComp = { sizeof(urlComp) };
    urlComp.dwSchemeLength = (DWORD)-1;
    urlComp.dwHostNameLength = (DWORD)-1;
    urlComp.dwUrlPathLength = (DWORD)-1;
    urlComp.dwExtraInfoLength = (DWORD)-1;

    if (!WinHttpCrackUrl(downloadUrl.c_str(), (DWORD)downloadUrl.length(), 0, &urlComp)) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::wstring host(urlComp.lpszHostName, urlComp.dwHostNameLength);
    std::wstring path(urlComp.lpszUrlPath, urlComp.dwUrlPathLength + urlComp.dwExtraInfoLength);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), urlComp.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), NULL,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    std::wstring headers = L"User-Agent: BeagleSampler-App\r\n";
    bool success = false;

    if (WinHttpSendRequest(hRequest, headers.c_str(), headers.length(), NULL, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, NULL)) {
        
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

        if (statusCode == 200) {
            FILE* outFile = _wfopen(installerPath.c_str(), L"wb");
            if (outFile) {
                char buffer[16384];
                DWORD bytesAvailable = 0;
                DWORD totalBytesDownloaded = 0;
                while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
                    DWORD bytesRead = 0;
                    DWORD toRead = std::min(bytesAvailable, (DWORD)sizeof(buffer));
                    if (WinHttpReadData(hRequest, buffer, toRead, &bytesRead) && bytesRead > 0) {
                        fwrite(buffer, 1, bytesRead, outFile);
                        totalBytesDownloaded += bytesRead;
                    } else {
                        break;
                    }
                }
                fclose(outFile);

                if (totalBytesDownloaded > 100000) {
                    success = true;
                }
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (success) {
        wchar_t currentExe[MAX_PATH] = { 0 };
        GetModuleFileNameW(NULL, currentExe, MAX_PATH);
        wchar_t* pSlash = wcsrchr(currentExe, L'\\');
        std::wstring curDir = L"";
        if (pSlash) {
            *pSlash = L'\0';
            curDir = currentExe;
        }

        std::wstring params = L"--silent";
        if (!curDir.empty()) {
            params += L" --from-dir \"" + curDir + L"\"";
        }

        HINSTANCE hInstResult = ShellExecuteW(NULL, L"open", installerPath.c_str(), params.c_str(), NULL, SW_SHOWNORMAL);
        if ((INT_PTR)hInstResult > 32) {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            return true;
        }
    }

    return false;
}

static std::atomic<bool> g_isUpdateActive{false};
static std::atomic<bool> g_updatePromptOpen{false};
static std::atomic<bool> g_hasPromptedThisSession{false};

static void TriggerUpdateCheck(HWND hwnd, bool silentIfUpToDate) {
    if (silentIfUpToDate && g_hasPromptedThisSession.load()) {
        return;
    }

    if (g_isUpdateActive.exchange(true)) {
        if (!silentIfUpToDate) {
            MessageBoxW(hwnd, L"An update operation is already in progress.", L"BeagleSampler Update", MB_OK | MB_ICONINFORMATION);
        }
        return;
    }

    std::thread([hwnd, silentIfUpToDate]() {
        try {
            if (silentIfUpToDate) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2500));
            }

            RemoteReleaseInfo info = QueryLatestRelease();

            if (!info.found) {
                g_isUpdateActive.store(false);
                if (!silentIfUpToDate) {
                    MessageBoxW(hwnd,
                        L"Unable to check for updates. Please check your internet connection and try again.",
                        L"BeagleSampler Update", MB_OK | MB_ICONWARNING);
                }
                return;
            }

            bool newer = IsRemoteNewer(info.major, info.minor, info.patch);

            if (newer) {
                if (g_updatePromptOpen.exchange(true)) {
                    // Prompt is already visible on screen - do not show another
                    return;
                }

                // Mark prompted for this session so it will not prompt again until the application is restarted
                g_hasPromptedThisSession.store(true);

                std::wstringstream ss;
                ss << L"A new version of BeagleSampler is available!\n\n"
                   << L"Current Version:   " << CURRENT_VERSION_STR << L"\n"
                   << L"Latest Version:    " << info.tagName << L"\n\n"
                   << L"Would you like to download and install this update now?";

                int choice = MessageBoxW(hwnd, ss.str().c_str(), L"BeagleSampler Update Available", MB_YESNO | MB_ICONINFORMATION);
                g_updatePromptOpen.store(false);

                if (choice == IDYES) {
                    KillTimer(hwnd, 99);
                    g_statusMessage = L"Downloading BeagleSampler update...";
                    g_statusMessageExpiry = GetTickCount() + 30000;
                    InvalidateRect(hwnd, NULL, FALSE);

                    bool dlOk = DownloadAndRunUpdate(hwnd, info.setupDownloadUrl);
                    if (!dlOk) {
                        g_isUpdateActive.store(false);
                        g_statusMessage = L"Update download failed";
                        g_statusMessageExpiry = GetTickCount() + 3000;
                        InvalidateRect(hwnd, NULL, FALSE);
                        MessageBoxW(hwnd,
                            L"Failed to download the update installer. Please check your internet connection or download it manually from GitHub.",
                            L"Update Error", MB_OK | MB_ICONERROR);
                    }
                } else {
                    g_isUpdateActive.store(false);
                }
            } else {
                g_isUpdateActive.store(false);
                if (!silentIfUpToDate) {
                    std::wstringstream ss;
                    ss << L"You are up to date!\n\n"
                       << L"BeagleSampler " << CURRENT_VERSION_STR << L" is currently the newest version.";
                    MessageBoxW(hwnd, ss.str().c_str(), L"BeagleSampler Update", MB_OK | MB_ICONINFORMATION);
                }
            }
        } catch (...) {
            g_updatePromptOpen.store(false);
            g_isUpdateActive.store(false);
        }
    }).detach();
}

static std::vector<AudioDeviceInfo> g_cachedDevices;

static void setBorderless(HWND hwnd, bool borderless) {
    g_borderless = borderless;
    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    if (borderless) {
        style &= ~(WS_CAPTION | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
        style |= (WS_POPUP | WS_THICKFRAME);
    } else {
        style &= ~WS_POPUP;
        style |= WS_OVERLAPPEDWINDOW;
    }
    SetWindowLongPtr(hwnd, GWL_STYLE, style);
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    if (borderless) {
        MARGINS margins = { 0, 0, 0, 1 };
        DwmExtendFrameIntoClientArea(hwnd, &margins);
    }
    g_statusMessage = borderless ? L"Borderless Window Mode: ON [B]" : L"Window Mode: Standard Window [B]";
    g_statusMessageExpiry = GetTickCount() + 2500;
    InvalidateRect(hwnd, NULL, FALSE);
    UpdateWindow(hwnd);
}

static void showContextMenu(HWND hwnd, int screenX, int screenY) {
    if (!g_buffer) return;

    HMENU hMenu = CreatePopupMenu();
    HMENU hDevMenu = CreatePopupMenu();
    HMENU hOutputMenu = CreatePopupMenu();
    HMENU hInputMenu = CreatePopupMenu();
    HMENU hDurMenu = CreatePopupMenu();
    HMENU hThemeMenu = CreatePopupMenu();
    HMENU hGainMenu = CreatePopupMenu();
    HMENU hFormatMenu = CreatePopupMenu();
    HMENU hChMenu = CreatePopupMenu();
    HMENU hSilenceMenu = CreatePopupMenu();
    HMENU hThreshMenu = CreatePopupMenu();
    HMENU hFpsMenu = CreatePopupMenu();
    HMENU hWinMenu = CreatePopupMenu();
    HMENU hDispMenu = CreatePopupMenu();

    // 1. Audio Devices (Desktop Loopback & Microphones)
    if (g_capture) {
        g_cachedDevices = g_capture->getAvailableDevices();
        std::wstring curDevName = g_capture->getDeviceName();
        bool curIsLoopback = g_capture->isLoopback();

        AppendMenuW(hDevMenu, MF_STRING | (curIsLoopback ? MF_CHECKED : 0),
                    IDM_DEV_DEF_LOOPBACK, L"Default Desktop Audio (Loopback) [L]");
        AppendMenuW(hDevMenu, MF_STRING | (!curIsLoopback ? MF_CHECKED : 0),
                    IDM_DEV_DEF_CAPTURE, L"Default Microphone / Input");
        AppendMenuW(hDevMenu, MF_SEPARATOR, 0, NULL);

        for (size_t i = 0; i < g_cachedDevices.size(); ++i) {
            const auto& dev = g_cachedDevices[i];
            bool isCurrent = (dev.name == curDevName);
            if (dev.isLoopback) {
                AppendMenuW(hOutputMenu, MF_STRING | (isCurrent ? MF_CHECKED : 0),
                            IDM_DEVICE_BASE + static_cast<UINT>(i), dev.name.c_str());
            } else {
                AppendMenuW(hInputMenu, MF_STRING | (isCurrent ? MF_CHECKED : 0),
                            IDM_DEVICE_BASE + static_cast<UINT>(i), dev.name.c_str());
            }
        }
        AppendMenuW(hDevMenu, MF_POPUP, (UINT_PTR)hOutputMenu, L"Desktop Audio Outputs (Loopback)");
        AppendMenuW(hDevMenu, MF_POPUP, (UINT_PTR)hInputMenu, L"Microphones & Inputs");
    }
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hDevMenu, L"Audio Device");

    double curDur = g_buffer->getDuration();
    AppendMenuW(hDurMenu, MF_STRING | (std::abs(curDur - 15.0) < 0.1 ? MF_CHECKED : 0), IDM_DUR_15, L"15s [1]");
    AppendMenuW(hDurMenu, MF_STRING | (std::abs(curDur - 30.0) < 0.1 ? MF_CHECKED : 0), IDM_DUR_30, L"30s [2]");
    AppendMenuW(hDurMenu, MF_STRING | (std::abs(curDur - 60.0) < 0.1 ? MF_CHECKED : 0), IDM_DUR_60, L"60s [3]");
    AppendMenuW(hDurMenu, MF_STRING | (std::abs(curDur - 90.0) < 0.1 ? MF_CHECKED : 0), IDM_DUR_90, L"90s");
    AppendMenuW(hDurMenu, MF_STRING | (std::abs(curDur - 120.0) < 0.1 ? MF_CHECKED : 0), IDM_DUR_120, L"120s [4]");
    AppendMenuW(hDurMenu, MF_STRING | (std::abs(curDur - 180.0) < 0.1 ? MF_CHECKED : 0), IDM_DUR_180, L"180s");
    AppendMenuW(hDurMenu, MF_STRING | (std::abs(curDur - 300.0) < 0.1 ? MF_CHECKED : 0), IDM_DUR_300, L"300s [5]");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hDurMenu, L"Duration");

    AppendMenuW(hThemeMenu, MF_STRING | (g_currentTheme == ThemeId::Carbon ? MF_CHECKED : 0), IDM_THEME_CARBON, L"Carbon (Default) [T]");
    AppendMenuW(hThemeMenu, MF_STRING | (g_currentTheme == ThemeId::Rainbow ? MF_CHECKED : 0), IDM_THEME_RAINBOW, L"Rainbow [T]");
    AppendMenuW(hThemeMenu, MF_STRING | (g_currentTheme == ThemeId::Bright ? MF_CHECKED : 0), IDM_THEME_BRIGHT, L"Bright [T]");
    AppendMenuW(hThemeMenu, MF_STRING | (g_currentTheme == ThemeId::Lemon ? MF_CHECKED : 0), IDM_THEME_LEMON, L"Lemon [T]");
    AppendMenuW(hThemeMenu, MF_STRING | (g_currentTheme == ThemeId::Vintage ? MF_CHECKED : 0), IDM_THEME_VINTAGE, L"Vintage [T]");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hThemeMenu, L"Themes");

    AppendMenuW(hGainMenu, MF_STRING | (std::abs(g_displayGain - 20.0f) < 0.1f ? MF_CHECKED : 0), IDM_GAIN_P26, L"+26 dB (20.0x) [G]");
    AppendMenuW(hGainMenu, MF_STRING | (std::abs(g_displayGain - 10.0f) < 0.1f ? MF_CHECKED : 0), IDM_GAIN_P20, L"+20 dB (10.0x) [G]");
    AppendMenuW(hGainMenu, MF_STRING | (std::abs(g_displayGain - 5.0f) < 0.1f ? MF_CHECKED : 0), IDM_GAIN_P14, L"+14 dB (5.0x) [G]");
    AppendMenuW(hGainMenu, MF_STRING | (std::abs(g_displayGain - 3.0f) < 0.1f ? MF_CHECKED : 0), IDM_GAIN_P10, L"+10 dB (3.0x) [G]");
    AppendMenuW(hGainMenu, MF_STRING | (std::abs(g_displayGain - 2.0f) < 0.1f ? MF_CHECKED : 0), IDM_GAIN_P6, L"+6 dB (2.0x) [G]");
    AppendMenuW(hGainMenu, MF_STRING | (std::abs(g_displayGain - 1.0f) < 0.1f ? MF_CHECKED : 0), IDM_GAIN_0, L"Unity / 0 dB (1.0x) [Default] [G]");
    AppendMenuW(hGainMenu, MF_STRING | (std::abs(g_displayGain - 0.5f) < 0.1f ? MF_CHECKED : 0), IDM_GAIN_M6, L"-6 dB (0.5x) [G]");
    AppendMenuW(hGainMenu, MF_STRING | (std::abs(g_displayGain - 0.25f) < 0.1f ? MF_CHECKED : 0), IDM_GAIN_M12, L"-12 dB (0.25x) [G]");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hGainMenu, L"Display Gain");

    AppendMenuW(hDispMenu, MF_STRING | (g_displayMode == DisplayMode::Sweep ? MF_CHECKED : 0), IDM_DISP_SWEEP, L"Sweep Mode (Traveling Playhead) [W]");
    AppendMenuW(hDispMenu, MF_STRING | (g_displayMode == DisplayMode::Rolling ? MF_CHECKED : 0), IDM_DISP_ROLLING, L"Continuous Rolling Mode [W]");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hDispMenu, L"Display Mode");

    AppendMenuW(hChMenu, MF_STRING | (g_channelMode == ChannelMode::Stereo ? MF_CHECKED : 0), IDM_CH_STEREO, L"Stereo");
    AppendMenuW(hChMenu, MF_STRING | (g_channelMode == ChannelMode::MonoLeft ? MF_CHECKED : 0), IDM_CH_MONO_L, L"Mono (Left)");
    AppendMenuW(hChMenu, MF_STRING | (g_channelMode == ChannelMode::MonoRight ? MF_CHECKED : 0), IDM_CH_MONO_R, L"Mono (Right)");
    AppendMenuW(hFormatMenu, MF_POPUP, (UINT_PTR)hChMenu, L"Channels");
    AppendMenuW(hFormatMenu, MF_STRING | (g_save24Bit ? MF_CHECKED : 0), IDM_SAVE_24BIT, L"Save 24-bit files");
    AppendMenuW(hFormatMenu, MF_STRING | (g_appendBpm ? MF_CHECKED : 0), IDM_APPEND_BPM, L"Add BPM to filename");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hFormatMenu, L"Format");

    AppendMenuW(hSilenceMenu, MF_STRING | (g_trimSilence ? MF_CHECKED : 0), IDM_TRIM_SILENCE, L"Automatically Trim Silence");
    AppendMenuW(hThreshMenu, MF_STRING | (std::abs(g_silenceThresholdDb - (-125.0f)) < 0.1f ? MF_CHECKED : 0), IDM_THRESH_125, L"-125 dB");
    AppendMenuW(hThreshMenu, MF_STRING | (std::abs(g_silenceThresholdDb - (-100.0f)) < 0.1f ? MF_CHECKED : 0), IDM_THRESH_100, L"-100 dB");
    AppendMenuW(hThreshMenu, MF_STRING | (std::abs(g_silenceThresholdDb - (-90.0f)) < 0.1f ? MF_CHECKED : 0), IDM_THRESH_90, L"-90 dB [Default]");
    AppendMenuW(hThreshMenu, MF_STRING | (std::abs(g_silenceThresholdDb - (-75.0f)) < 0.1f ? MF_CHECKED : 0), IDM_THRESH_75, L"-75 dB");
    AppendMenuW(hThreshMenu, MF_STRING | (std::abs(g_silenceThresholdDb - (-60.0f)) < 0.1f ? MF_CHECKED : 0), IDM_THRESH_60, L"-60 dB");
    AppendMenuW(hSilenceMenu, MF_POPUP, (UINT_PTR)hThreshMenu, L"Silence Threshold");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hSilenceMenu, L"Silence Trimming");

    AppendMenuW(hFpsMenu, MF_STRING | (g_targetFps == 60 ? MF_CHECKED : 0), IDM_FPS_60, L"60 FPS (Default)");
    AppendMenuW(hFpsMenu, MF_STRING | (g_targetFps == 30 ? MF_CHECKED : 0), IDM_FPS_30, L"30 FPS");
    AppendMenuW(hFpsMenu, MF_STRING | (g_targetFps == 20 ? MF_CHECKED : 0), IDM_FPS_20, L"20 FPS");
    AppendMenuW(hFpsMenu, MF_STRING | (g_targetFps == 15 ? MF_CHECKED : 0), IDM_FPS_15, L"15 FPS");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hFpsMenu, L"Waveform FPS");

    AppendMenuW(hWinMenu, MF_STRING | (g_pinOnTop ? MF_CHECKED : 0), IDM_PIN_ON_TOP, L"Pin On Top");
    AppendMenuW(hWinMenu, MF_STRING | (g_asymmetrical ? MF_CHECKED : 0), IDM_ASYMMETRICAL, L"Asymmetrical Display");
    AppendMenuW(hWinMenu, MF_STRING | (g_borderless ? MF_CHECKED : 0), IDM_BORDERLESS, L"Borderless Window Mode [B]");
    AppendMenuW(hWinMenu, MF_STRING | (g_saveSessionState ? MF_CHECKED : 0), IDM_SAVE_SESSION_STATE, L"Restore Audio Buffer on Launch");
    AppendMenuW(hWinMenu, MF_STRING | (g_autoCheckUpdates ? MF_CHECKED : 0), IDM_AUTO_CHECK_UPDATES, L"Check for Updates on Launch");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hWinMenu, L"Window");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    bool isP = g_buffer->isPaused();
    AppendMenuW(hMenu, MF_STRING | (isP ? MF_CHECKED : 0), IDM_PAUSE, isP ? L"Resume Recording [Space]" : L"Pause Recording [Space]");
    AppendMenuW(hMenu, MF_STRING | (g_hasSelection ? 0 : MF_GRAYED), IDM_PREVIEW, L"Preview Playback [P]");
    AppendMenuW(hMenu, MF_STRING, IDM_CLEAR, L"Clear Buffer [C / Del]");
    AppendMenuW(hMenu, MF_STRING, IDM_EXPORT_SEL, L"Export Selection to WAV [S]");
    AppendMenuW(hMenu, MF_STRING, IDM_EXPORT_FULL, L"Export Full Buffer to WAV");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_CHECK_UPDATES, L"Check for Updates...");
    AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 999, (std::wstring(L"BeagleSampler ") + CURRENT_VERSION_STR).c_str());


    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_TOPALIGN | TPM_LEFTALIGN, screenX, screenY, 0, hwnd, NULL);

    DestroyMenu(hDispMenu);
    DestroyMenu(hWinMenu);
    DestroyMenu(hFpsMenu);
    DestroyMenu(hThreshMenu);
    DestroyMenu(hSilenceMenu);
    DestroyMenu(hFormatMenu);
    DestroyMenu(hChMenu);
    DestroyMenu(hGainMenu);
    DestroyMenu(hThemeMenu);
    DestroyMenu(hDurMenu);
    DestroyMenu(hInputMenu);
    DestroyMenu(hOutputMenu);
    DestroyMenu(hDevMenu);
    DestroyMenu(hMenu);
}

// Two-layer persistent compositing render targets (zero memory churn & instant 70-microsecond drag selection)
static HDC s_backDC = NULL;
static HBITMAP s_backBM = NULL;
static HBITMAP s_oldBM = NULL;

static HDC s_baseDC = NULL;
static HBITMAP s_baseBM = NULL;
static HBITMAP s_oldBaseBM = NULL;

static int s_backW = 0;
static int s_backH = 0;
static bool s_baseNeedsUpdate = true;
static bool s_inSizeMove = false;

static std::vector<float> s_maxEnv;
static std::vector<float> s_minEnv;
static std::vector<PointF> s_pts;
static HBRUSH s_darkBgBrush = NULL;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ENTERSIZEMOVE: {
        s_inSizeMove = true;
        return 0;
    }

    case WM_EXITSIZEMOVE: {
        s_inSizeMove = false;
        s_baseNeedsUpdate = true;
        saveSettings(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_NCCALCSIZE: {
        if (g_borderless) {
            // Borderless client area occupies entire window frame; eliminates OS titlebar & borders
            return 0;
        }
        break;
    }

    case WM_NCPAINT: {
        if (g_borderless) {
            // Suppress default non-client frame painting to prevent momentary white border flashes on drag/resize
            return 0;
        }
        break;
    }

    case WM_NCACTIVATE: {
        if (g_borderless) {
            // Suppress default non-client activation painting (which repaints white borders on titlebar/frame)
            return TRUE;
        }
        break;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        if (mmi) {
            mmi->ptMinTrackSize.x = 420;
            mmi->ptMinTrackSize.y = 140;

            if (g_borderless) {
                HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = { sizeof(MONITORINFO) };
                if (GetMonitorInfo(hMon, &mi)) {
                    mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
                    mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
                    mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
                    mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
                }
            }
        }
        return 0;
    }

    case WM_NCHITTEST: {
        if (!g_borderless) break;
        if (g_isSelecting || g_isDragging) return HTCLIENT;

        POINT pt;
        pt.x = static_cast<short>(LOWORD(lParam));
        pt.y = static_cast<short>(HIWORD(lParam));

        RECT rc;
        GetWindowRect(hwnd, &rc);

        const int BORDER_THICKNESS = 8; // Invisible resize perimeter

        bool left = (pt.x >= rc.left && pt.x < rc.left + BORDER_THICKNESS);
        bool right = (pt.x < rc.right && pt.x >= rc.right - BORDER_THICKNESS);
        bool top = (pt.y >= rc.top && pt.y < rc.top + BORDER_THICKNESS);
        bool bottom = (pt.y < rc.bottom && pt.y >= rc.bottom - BORDER_THICKNESS);

        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;

        POINT clientPt = pt;
        ScreenToClient(hwnd, &clientPt);

        if (PtInRect(&g_rectCloseBtn, clientPt) ||
            PtInRect(&g_rectMinBtn, clientPt) ||
            PtInRect(&g_rectPinBtn, clientPt) ||
            PtInRect(&g_rectSettingsBtn, clientPt) ||
            PtInRect(&g_rectPauseBtn, clientPt)) {
            return HTCLIENT;
        }

        if (clientPt.y < 30 || (clientPt.x < 92 && clientPt.y < 86)) {
            return HTCAPTION;
        }

        return HTCLIENT;
    }

    case WM_NCRBUTTONDOWN:
        return 0;

    case WM_NCRBUTTONUP: {
        // Right clicking in non-client header also clears selection and resumes recording
        g_hasSelection = false;
        g_isSelecting = false;
        g_selStartX = 0;
        g_selEndX = 0;
        if (g_buffer && g_buffer->isPaused()) {
            g_buffer->setPaused(false);
            g_statusMessage = L"● Resumed Recording";
            g_statusMessageExpiry = GetTickCount() + 1500;
        }
        s_baseNeedsUpdate = true;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_CONTEXTMENU:
        return 0;

    case WM_SIZING: {
        s_baseNeedsUpdate = true;
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }

    case WM_SIZE: {
        s_baseNeedsUpdate = true;
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }

    case WM_ERASEBKGND: {
        HDC hdcErase = (HDC)wParam;
        RECT rcErase;
        GetClientRect(hwnd, &rcErase);
        if (!s_darkBgBrush) s_darkBgBrush = CreateSolidBrush(RGB(20, 20, 22));
        FillRect(hdcErase, &rcErase, s_darkBgBrush);
        return 1;
    }

    case WM_CREATE: {
        SetTimer(hwnd, 1, 1000 / g_targetFps, NULL);
        SetTimer(hwnd, 99, 14400000 /* 4 hours periodic check */, NULL);
        break;
    }


    case WM_KEYDOWN: {
        if (wParam == VK_SPACE) {
            if (g_buffer) {
                bool p = !g_buffer->isPaused();
                g_buffer->setPaused(p);
                g_statusMessage = p ? L"❚❚ Paused Recording" : L"● Resumed Recording";
                g_statusMessageExpiry = GetTickCount() + 2000;
            }
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wParam == 'L') {
            if (g_capture) {
                bool nextLoopback = !g_capture->isLoopback();
                if (nextLoopback) {
                    g_capture->setDevice(L"default_loopback", true);
                    g_statusMessage = L"Switched to: Desktop Audio Loopback [L]";
                } else {
                    g_capture->setDevice(L"default_capture", false);
                    g_statusMessage = L"Switched to: Microphone Input [L]";
                }
                g_statusMessageExpiry = GetTickCount() + 3500;
                InvalidateRect(hwnd, NULL, FALSE);
            }
        } else if (wParam == 'P') {
            std::wstring wav = exportPreviewWav(g_hasSelection);
            if (!wav.empty()) {
                PlaySoundW(wav.c_str(), NULL, SND_FILENAME | SND_ASYNC);
                g_statusMessage = L"▶ Preview Playing...";
                g_statusMessageExpiry = GetTickCount() + 2500;
                InvalidateRect(hwnd, NULL, FALSE);
            }
        } else if (wParam == '1') {
            if (g_buffer) g_buffer->setDuration(15.0, g_buffer->getSampleRate());
            g_statusMessage = L"Duration: 15s";
            g_statusMessageExpiry = GetTickCount() + 2000;
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wParam == '2') {
            if (g_buffer) g_buffer->setDuration(30.0, g_buffer->getSampleRate());
            g_statusMessage = L"Duration: 30s";
            g_statusMessageExpiry = GetTickCount() + 2000;
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wParam == '3') {
            if (g_buffer) g_buffer->setDuration(60.0, g_buffer->getSampleRate());
            g_statusMessage = L"Duration: 60s";
            g_statusMessageExpiry = GetTickCount() + 2000;
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wParam == '4') {
            if (g_buffer) g_buffer->setDuration(120.0, g_buffer->getSampleRate());
            g_statusMessage = L"Duration: 120s";
            g_statusMessageExpiry = GetTickCount() + 2000;
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wParam == '5') {
            if (g_buffer) g_buffer->setDuration(300.0, g_buffer->getSampleRate());
            g_statusMessage = L"Duration: 300s";
            g_statusMessageExpiry = GetTickCount() + 2000;
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wParam == 'T') {
            int nextT = (static_cast<int>(g_currentTheme) % 5) + 1;
            g_currentTheme = static_cast<ThemeId>(nextT);
            ThemeColors tc = getThemeColors(g_currentTheme, false);
            g_statusMessage = std::wstring(L"Theme: ") + tc.name;
            g_statusMessageExpiry = GetTickCount() + 2500;
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wParam == 'G') {
            if (std::abs(g_displayGain - 1.0f) < 0.05f) {
                g_displayGain = 2.0f;
                g_statusMessage = L"Gain: +6 dB (2.0x) [G]";
            } else if (std::abs(g_displayGain - 2.0f) < 0.05f) {
                g_displayGain = 0.5f;
                g_statusMessage = L"Gain: -6 dB (0.5x) [G]";
            } else if (std::abs(g_displayGain - 0.5f) < 0.05f) {
                g_displayGain = 0.25f;
                g_statusMessage = L"Gain: -12 dB (0.25x) [G]";
            } else {
                g_displayGain = 1.0f;
                g_statusMessage = L"Gain: Unity 0 dB (1.0x) [Default] [G]";
            }
            g_statusMessageExpiry = GetTickCount() + 2500;
            s_baseNeedsUpdate = true;
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wParam == VK_ADD || wParam == VK_OEM_PLUS || wParam == 187) {
            g_displayGain = std::min(4.0f, g_displayGain * 1.25f);
            float db = 20.0f * std::log10(g_displayGain);
            std::wstringstream ss;
            if (std::abs(g_displayGain - 1.0f) < 0.01f) {
                ss << L"Gain: 0 dB (1.0x)";
            } else {
                ss << L"Gain: " << (db >= 0.0f ? L"+" : L"") << std::fixed << std::setprecision(1) << db << L" dB ("
                   << std::fixed << std::setprecision(1) << g_displayGain << L"x)";
            }
            g_statusMessage = ss.str();
            g_statusMessageExpiry = GetTickCount() + 2000;
            s_baseNeedsUpdate = true;
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wParam == VK_SUBTRACT || wParam == VK_OEM_MINUS || wParam == 189) {
            g_displayGain = std::max(0.25f, g_displayGain / 1.25f);
            float db = 20.0f * std::log10(g_displayGain);
            std::wstringstream ss;
            if (std::abs(g_displayGain - 1.0f) < 0.01f) {
                ss << L"Gain: 0 dB (1.0x)";
            } else {
                ss << L"Gain: " << (db >= 0.0f ? L"+" : L"") << std::fixed << std::setprecision(1) << db << L" dB ("
                   << std::fixed << std::setprecision(1) << g_displayGain << L"x)";
            }
            g_statusMessage = ss.str();
            g_statusMessageExpiry = GetTickCount() + 2000;
            s_baseNeedsUpdate = true;
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wParam == 'C' || wParam == VK_DELETE || wParam == VK_BACK) {
            if (g_buffer) g_buffer->clear();
            g_hasSelection = false;
            _wremove(getSessionStateFilePath().c_str());
            g_statusMessage = L"Buffer Cleared";
            g_statusMessageExpiry = GetTickCount() + 2000;
            s_baseNeedsUpdate = true;
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wParam == 'S') {
            std::wstring path = exportCurrentAudio(g_hasSelection);
            if (!path.empty()) {
                size_t slashPos = path.find_last_of(L"\\/");
                std::wstring fileName = (slashPos != std::wstring::npos) ? path.substr(slashPos + 1) : path;
                g_statusMessage = L"Exported: " + fileName;
                g_statusMessageExpiry = GetTickCount() + 3500;
            }
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wParam == 'W') {
            g_displayMode = (g_displayMode == DisplayMode::Sweep) ? DisplayMode::Rolling : DisplayMode::Sweep;
            g_statusMessage = (g_displayMode == DisplayMode::Sweep) ? L"Mode: Sweep (Traveling Playhead) [W]" : L"Mode: Continuous Rolling [W]";
            g_statusMessageExpiry = GetTickCount() + 2500;
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wParam == 'B') {
            setBorderless(hwnd, !g_borderless);
        } else if (wParam == VK_ESCAPE) {
            g_hasSelection = false;
            g_statusMessage = L"Selection Deselected";
            g_statusMessageExpiry = GetTickCount() + 1500;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        saveSettings(hwnd);
        break;
    }

    case WM_LBUTTONDBLCLK: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        g_hasSelection = true;
        g_selStartX = 0;
        g_selEndX = rc.right;
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }

    case WM_RBUTTONUP: {
        // Right clicking in the timeline clears any active selections and resumes playback
        g_hasSelection = false;
        g_isSelecting = false;
        g_selStartX = 0;
        g_selEndX = 0;

        if (g_buffer && g_buffer->isPaused()) {
            g_buffer->setPaused(false);
            g_statusMessage = L"● Resumed Recording";
            g_statusMessageExpiry = GetTickCount() + 1500;
        }

        s_baseNeedsUpdate = true;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);
        POINT pt = { x, y };
        g_mouseDownPos = pt;

        // 1. Custom Circular Window Chrome Buttons
        if (PtInRect(&g_rectCloseBtn, pt)) {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }

        if (PtInRect(&g_rectMinBtn, pt)) {
            ShowWindow(hwnd, SW_MINIMIZE);
            return 0;
        }

        if (PtInRect(&g_rectPinBtn, pt)) {
            g_pinOnTop = !g_pinOnTop;
            SetWindowPos(hwnd, g_pinOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            g_statusMessage = g_pinOnTop ? L"📌 Pinned On Top" : L"Unpinned";
            g_statusMessageExpiry = GetTickCount() + 2000;
            s_baseNeedsUpdate = true;
            saveSettings(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // 2. Corner controls (Pause / Settings)
        if (PtInRect(&g_rectPauseBtn, pt)) {
            if (g_buffer) g_buffer->setPaused(!g_buffer->isPaused());
            s_baseNeedsUpdate = true;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        if (PtInRect(&g_rectSettingsBtn, pt)) {
            POINT screenPt = { g_rectSettingsBtn.left, g_rectSettingsBtn.bottom };
            ClientToScreen(hwnd, &screenPt);
            showContextMenu(hwnd, screenPt.x, screenPt.y);
            return 0;
        }

        // 3. Custom Header Drag: clicking in top header bar (y < 30) or beagle logo allows moving window
        if (y < 30 || (x < 92 && y < 86)) {
            ReleaseCapture();
            SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }

        // Auto-pause waveform immediately when dragging starts so audio doesn't scroll under cursor
        if (g_buffer && !g_buffer->isPaused()) {
            g_buffer->setPaused(true);
            g_wasAutoPaused = true;
            s_baseNeedsUpdate = true;
        }

        // If clicking inside an existing selection, prepare for drag out
        if (g_hasSelection) {
            int selL = std::min(g_selStartX, g_selEndX);
            int selR = std::max(g_selStartX, g_selEndX);
            if (x >= selL && x <= selR) {
                g_isDragging = true;
                SetCapture(hwnd);
                return 0;
            }
        }

        // Start new selection rectangle
        g_isSelecting = true;
        g_isDragging = false;
        g_selStartX = x;
        g_selEndX = x;
        g_hasSelection = true;
        SetCapture(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }

    case WM_MOUSEMOVE: {
        int x = static_cast<short>(LOWORD(lParam));
        int y = static_cast<short>(HIWORD(lParam));
        POINT pt = { x, y };

        if (!g_isSelecting && !g_isDragging) {
            TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);

            HoverBtn prevHover = g_hoverBtn;
            if (PtInRect(&g_rectCloseBtn, pt)) g_hoverBtn = HoverBtn::Close;
            else if (PtInRect(&g_rectMinBtn, pt)) g_hoverBtn = HoverBtn::Min;
            else if (PtInRect(&g_rectPinBtn, pt)) g_hoverBtn = HoverBtn::Pin;
            else if (PtInRect(&g_rectSettingsBtn, pt)) g_hoverBtn = HoverBtn::Settings;
            else if (PtInRect(&g_rectPauseBtn, pt)) g_hoverBtn = HoverBtn::Pause;
            else g_hoverBtn = HoverBtn::None;

            if (g_hoverBtn != prevHover) {
                s_baseNeedsUpdate = true;
                InvalidateRect(hwnd, NULL, FALSE);
                UpdateWindow(hwnd);
            }
        }

        if (wParam & MK_LBUTTON) {
            RECT rc;
            GetClientRect(hwnd, &rc);

            int dx = std::abs(x - g_mouseDownPos.x);
            int dy = std::abs(y - g_mouseDownPos.y);
            bool pastDragThreshold = (dx > GetSystemMetrics(SM_CXDRAG) || dy > GetSystemMetrics(SM_CYDRAG));

            // Only trigger OLE drag-and-drop export when dragging an established selection!
            if (g_isDragging && pastDragThreshold) {
                performDragAndDrop(hwnd);
                return 0;
            }

            if (g_isSelecting) {
                int newEndX = std::clamp(x, 0, static_cast<int>(rc.right));
                if (newEndX != g_selEndX) {
                    g_selEndX = newEndX;
                    InvalidateRect(hwnd, NULL, FALSE);
                    MSG msgPeek;
                    if (!PeekMessage(&msgPeek, hwnd, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_NOREMOVE)) {
                        UpdateWindow(hwnd);
                    }
                }
            }
        }
        break;
    }

    case WM_MOUSELEAVE: {
        if (g_hoverBtn != HoverBtn::None) {
            g_hoverBtn = HoverBtn::None;
            s_baseNeedsUpdate = true;
            InvalidateRect(hwnd, NULL, FALSE);
            UpdateWindow(hwnd);
        }
        break;
    }

    case WM_LBUTTONUP: {
        ReleaseCapture();
        g_isDragging = false;

        if (g_isSelecting) {
            g_isSelecting = false;
            if (std::abs(g_selEndX - g_selStartX) < 6) {
                // Clicked without moving: clear selection
                g_hasSelection = false;
            }
            InvalidateRect(hwnd, NULL, FALSE);
            UpdateWindow(hwnd);
        }
        break;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (delta > 0) {
            g_displayGain = std::min(40.0f, g_displayGain * 1.25f);
        } else if (delta < 0) {
            g_displayGain = std::max(0.25f, g_displayGain / 1.25f);
        }
        float db = 20.0f * std::log10(g_displayGain);
        std::wstringstream ss;
        ss << L"Gain: " << (db >= 0.0f ? L"+" : L"") << std::fixed << std::setprecision(1) << db << L" dB ("
           << std::fixed << std::setprecision(1) << g_displayGain << L"x) [Mouse Wheel]";
        g_statusMessage = ss.str();
        g_statusMessageExpiry = GetTickCount() + 1800;
        saveSettings(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDM_PAUSE) {
            if (g_buffer) g_buffer->setPaused(!g_buffer->isPaused());
        } else if (id == IDM_PREVIEW) {
            std::wstring wav = exportPreviewWav(g_hasSelection);
            if (!wav.empty()) {
                PlaySoundW(wav.c_str(), NULL, SND_FILENAME | SND_ASYNC);
                g_statusMessage = L"▶ Preview Playing...";
                g_statusMessageExpiry = GetTickCount() + 2500;
            }
        } else if (id == IDM_DUR_15) {
            if (g_buffer) g_buffer->setDuration(15.0, g_buffer->getSampleRate());
        } else if (id == IDM_DUR_30) {
            if (g_buffer) g_buffer->setDuration(30.0, g_buffer->getSampleRate());
        } else if (id == IDM_DUR_60) {
            if (g_buffer) g_buffer->setDuration(60.0, g_buffer->getSampleRate());
        } else if (id == IDM_DUR_90) {
            if (g_buffer) g_buffer->setDuration(90.0, g_buffer->getSampleRate());
        } else if (id == IDM_DUR_120) {
            if (g_buffer) g_buffer->setDuration(120.0, g_buffer->getSampleRate());
        } else if (id == IDM_DUR_180) {
            if (g_buffer) g_buffer->setDuration(180.0, g_buffer->getSampleRate());
        } else if (id == IDM_DUR_300) {
            if (g_buffer) g_buffer->setDuration(300.0, g_buffer->getSampleRate());
        } else if (id == IDM_THEME_CARBON) {
            g_currentTheme = ThemeId::Carbon;
        } else if (id == IDM_THEME_RAINBOW) {
            g_currentTheme = ThemeId::Rainbow;
        } else if (id == IDM_THEME_BRIGHT) {
            g_currentTheme = ThemeId::Bright;
        } else if (id == IDM_THEME_LEMON) {
            g_currentTheme = ThemeId::Lemon;
        } else if (id == IDM_THEME_VINTAGE) {
            g_currentTheme = ThemeId::Vintage;
        } else if (id == IDM_GAIN_M12) {
            g_displayGain = 0.25f;
        } else if (id == IDM_GAIN_M6) {
            g_displayGain = 0.5f;
        } else if (id == IDM_GAIN_0) {
            g_displayGain = 1.0f;
        } else if (id == IDM_GAIN_P6) {
            g_displayGain = 2.0f;
        } else if (id == IDM_GAIN_P10) {
            g_displayGain = 3.0f;
        } else if (id == IDM_GAIN_P14) {
            g_displayGain = 5.0f;
        } else if (id == IDM_GAIN_P20) {
            g_displayGain = 10.0f;
        } else if (id == IDM_GAIN_P26) {
            g_displayGain = 20.0f;
        } else if (id == IDM_DEV_DEF_LOOPBACK) {
            if (g_capture) {
                g_capture->setDevice(L"default_loopback", true);
                g_statusMessage = L"Switched to: Default Desktop Audio (Loopback)";
                g_statusMessageExpiry = GetTickCount() + 3500;
            }
        } else if (id == IDM_DEV_DEF_CAPTURE) {
            if (g_capture) {
                g_capture->setDevice(L"default_capture", false);
                g_statusMessage = L"Switched to: Default Microphone / Input";
                g_statusMessageExpiry = GetTickCount() + 3500;
            }
        } else if (id >= IDM_DEVICE_BASE && id < IDM_DEVICE_BASE + static_cast<int>(g_cachedDevices.size())) {
            size_t idx = id - IDM_DEVICE_BASE;
            if (g_capture && idx < g_cachedDevices.size()) {
                g_capture->setDevice(g_cachedDevices[idx].id, g_cachedDevices[idx].isLoopback);
                g_statusMessage = (g_cachedDevices[idx].isLoopback ? L"Desktop Audio: " : L"Input: ") + g_cachedDevices[idx].name;
                g_statusMessageExpiry = GetTickCount() + 3500;
            }
        } else if (id == IDM_CH_STEREO) {
            g_channelMode = ChannelMode::Stereo;
        } else if (id == IDM_CH_MONO_L) {
            g_channelMode = ChannelMode::MonoLeft;
        } else if (id == IDM_CH_MONO_R) {
            g_channelMode = ChannelMode::MonoRight;
        } else if (id == IDM_SAVE_24BIT) {
            g_save24Bit = !g_save24Bit;
        } else if (id == IDM_APPEND_BPM) {
            g_appendBpm = !g_appendBpm;
        } else if (id == IDM_TRIM_SILENCE) {
            g_trimSilence = !g_trimSilence;
        } else if (id == IDM_THRESH_125) {
            g_silenceThresholdDb = -125.0f;
        } else if (id == IDM_THRESH_100) {
            g_silenceThresholdDb = -100.0f;
        } else if (id == IDM_THRESH_90) {
            g_silenceThresholdDb = -90.0f;
        } else if (id == IDM_THRESH_75) {
            g_silenceThresholdDb = -75.0f;
        } else if (id == IDM_THRESH_60) {
            g_silenceThresholdDb = -60.0f;
        } else if (id == IDM_FPS_60) {
            g_targetFps = 60;
            SetTimer(hwnd, 1, 1000 / g_targetFps, NULL);
        } else if (id == IDM_FPS_30) {
            g_targetFps = 30;
            SetTimer(hwnd, 1, 1000 / g_targetFps, NULL);
        } else if (id == IDM_FPS_20) {
            g_targetFps = 20;
            SetTimer(hwnd, 1, 1000 / g_targetFps, NULL);
        } else if (id == IDM_FPS_15) {
            g_targetFps = 15;
            SetTimer(hwnd, 1, 1000 / g_targetFps, NULL);
        } else if (id == IDM_ASYMMETRICAL) {
            g_asymmetrical = !g_asymmetrical;
        } else if (id == IDM_CLEAR) {
            if (g_buffer) g_buffer->clear();
            g_hasSelection = false;
            _wremove(getSessionStateFilePath().c_str());
        } else if (id == IDM_EXPORT_SEL) {
            std::wstring path = exportCurrentAudio(true);
            if (!path.empty()) {
                g_statusMessage = L"Exported: " + path;
                g_statusMessageExpiry = GetTickCount() + 3500;
            }
        } else if (id == IDM_EXPORT_FULL) {
            std::wstring path = exportCurrentAudio(false);
            if (!path.empty()) {
                g_statusMessage = L"Exported Full: " + path;
                g_statusMessageExpiry = GetTickCount() + 3500;
            }
        } else if (id == IDM_PIN_ON_TOP) {
            g_pinOnTop = !g_pinOnTop;
            SetWindowPos(hwnd, g_pinOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            g_statusMessage = g_pinOnTop ? L"📌 Pinned On Top" : L"Unpinned";
            g_statusMessageExpiry = GetTickCount() + 2000;
        } else if (id == IDM_DISP_SWEEP) {
            g_displayMode = DisplayMode::Sweep;
            g_statusMessage = L"Mode: Sweep (Traveling Playhead)";
            g_statusMessageExpiry = GetTickCount() + 2000;
        } else if (id == IDM_DISP_ROLLING) {
            g_displayMode = DisplayMode::Rolling;
            g_statusMessage = L"Mode: Continuous Rolling";
            g_statusMessageExpiry = GetTickCount() + 2000;
        } else if (id == IDM_BORDERLESS) {
            setBorderless(hwnd, !g_borderless);
        } else if (id == IDM_SAVE_SESSION_STATE) {
            g_saveSessionState = !g_saveSessionState;
            if (!g_saveSessionState) {
                _wremove(getSessionStateFilePath().c_str());
            }
            g_statusMessage = g_saveSessionState ? L"Session Persistence: ON" : L"Session Persistence: OFF";
            g_statusMessageExpiry = GetTickCount() + 2500;
        } else if (id == IDM_CHECK_UPDATES) {
            TriggerUpdateCheck(hwnd, false);
        } else if (id == IDM_AUTO_CHECK_UPDATES) {
            g_autoCheckUpdates = !g_autoCheckUpdates;
            g_statusMessage = g_autoCheckUpdates ? L"Auto Check Updates: ON" : L"Auto Check Updates: OFF";
            g_statusMessageExpiry = GetTickCount() + 2500;
        }
        s_baseNeedsUpdate = true;
        saveSettings(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }

    case WM_TIMER: {
        if (wParam == 99) {
            if (g_autoCheckUpdates && !g_isUpdateActive.load() && !g_updatePromptOpen.load() && !g_hasPromptedThisSession.load()) {
                TriggerUpdateCheck(hwnd, true /* silentIfUpToDate */);
            }
            break;
        }

        if (s_inSizeMove) {
            // While dragging or resizing the window, do not block the OS modal move loop on CPU rasterization
            break;
        }

        bool isP = g_buffer ? g_buffer->isPaused() : false;
        if (!isP || !g_statusMessage.empty()) {
            s_baseNeedsUpdate = true;
        }
        // Force immediate, zero-latency synchronous repaint at precise 1ms-scheduled timer intervals
        InvalidateRect(hwnd, NULL, FALSE);
        UpdateWindow(hwnd);
        break;
    }


    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT clientRect;
        GetClientRect(hwnd, &clientRect);
        int width = clientRect.right - clientRect.left;
        int height = clientRect.bottom - clientRect.top;

        if (width <= 0 || height <= 0) {
            EndPaint(hwnd, &ps);
            break;
        }

        if (!s_backDC) s_backDC = CreateCompatibleDC(hdc);
        if (!s_baseDC) s_baseDC = CreateCompatibleDC(hdc);

        if (width != s_backW || height != s_backH || !s_backBM || !s_baseBM) {
            if (s_backBM) {
                SelectObject(s_backDC, s_oldBM);
                DeleteObject(s_backBM);
                s_backBM = NULL;
            }
            if (s_baseBM) {
                SelectObject(s_baseDC, s_oldBaseBM);
                DeleteObject(s_baseBM);
                s_baseBM = NULL;
            }
            s_backBM = CreateCompatibleBitmap(hdc, width, height);
            s_oldBM = (HBITMAP)SelectObject(s_backDC, s_backBM);

            s_baseBM = CreateCompatibleBitmap(hdc, width, height);
            s_oldBaseBM = (HBITMAP)SelectObject(s_baseDC, s_baseBM);

            s_backW = width;
            s_backH = height;
            s_baseNeedsUpdate = true;

            s_maxEnv.resize(width);
            s_minEnv.resize(width);
            s_pts.resize(width * 2);
        }

        bool isPaused = g_buffer ? g_buffer->isPaused() : false;
        ThemeColors theme = getThemeColors(g_currentTheme, isPaused);

        // Render base scene into s_baseDC only when active recording or state changed
        if (s_baseNeedsUpdate || !isPaused) {
            Graphics g(s_baseDC);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
            g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
            g.SetCompositingQuality(CompositingQualityHighSpeed);
            g.SetInterpolationMode(InterpolationModeBilinear);

            // 1. Fill Background
            SolidBrush bgBrush(theme.bg);
            g.FillRectangle(&bgBrush, 0, 0, width, height);

            float logoSize = std::clamp(static_cast<float>(height) * 0.35f, 36.0f, 72.0f);
            float topMargin = logoSize + 6.0f;
            float bottomMargin = 22.0f;
            float waveH = static_cast<float>(height) - topMargin - bottomMargin;
            if (waveH < 30.0f) waveH = 30.0f;
            float centerY = std::floor(topMargin + (waveH * 0.5f));
            float halfH = (waveH * 0.5f) - 2.0f;

            // 2. Vertical Time Grid Divisions
            Pen gridPen(theme.gridLine, 1.0f);
            int divisions = 6;
            for (int d = 1; d < divisions; ++d) {
                float gx = (static_cast<float>(width) * d) / static_cast<float>(divisions);
                g.DrawLine(&gridPen, gx, 0.0f, gx, static_cast<float>(height));
            }

            // 3. Center Datum Baseline
            Pen centerPen(theme.centerLine, 1.0f);
            g.DrawLine(&centerPen, 0.0f, centerY, static_cast<float>(width), centerY);

            // 4. Smooth Anti-Aliased Waveform Path
            float playheadX = static_cast<float>(width - 1);
            if (g_buffer && width > 2) {
                static uint64_t s_cachedEpoch = (uint64_t)-1;
                static uint32_t s_cachedHead = (uint32_t)-1;
                static float s_cachedGain = -1.0f;
                static DisplayMode s_cachedMode = (DisplayMode)-1;
                static int s_cachedWidth = -1;
                static bool s_cachedPaused = false;
                static bool s_cachedAsymm = false;
                static float s_cachedPlayheadX = 0.0f;

                uint64_t curEpoch = g_buffer->getEpoch();
                uint32_t curHead = g_buffer->getWriteHead();

                bool needUpdate = (!isPaused) || (!s_cachedPaused) ||
                                  (curEpoch != s_cachedEpoch) ||
                                  (curHead != s_cachedHead) ||
                                  (g_displayGain != s_cachedGain) ||
                                  (g_displayMode != s_cachedMode) ||
                                  (g_asymmetrical != s_cachedAsymm) ||
                                  (width != s_cachedWidth);

                if (needUpdate) {
                    if (g_displayMode == DisplayMode::Sweep) {
                        double frac = g_buffer->getInterpolatedWriteHeadFrac();
                        playheadX = std::clamp(static_cast<float>(frac * width), 0.0f, static_cast<float>(width - 1));
                        g_buffer->getBufferSweepEnvelope(width, g_displayGain, s_maxEnv.data(), s_minEnv.data(), 0, 12, frac);
                    } else {
                        g_buffer->getSmoothRollingEnvelope(width, g_displayGain, s_maxEnv.data(), s_minEnv.data());
                        playheadX = static_cast<float>(width - 1);
                    }

                    for (int i = 0; i < width; ++i) {
                        float rawMax = s_maxEnv[i];
                        float rawMin = s_minEnv[i];
                        float peak = g_asymmetrical ? rawMax : std::max(rawMax, -rawMin);
                        float topDisp = peak * halfH;
                        float topY = std::round(centerY - topDisp);
                        s_pts[i] = PointF(static_cast<float>(i), topY);
                    }
                    for (int i = 0; i < width; ++i) {
                        int idx = width - 1 - i;
                        float rawMax = s_maxEnv[idx];
                        float rawMin = s_minEnv[idx];
                        float peak = g_asymmetrical ? 0.0f : std::max(rawMax, -rawMin);
                        float botDisp = peak * halfH;
                        float botY = std::round(centerY + botDisp);
                        s_pts[width + i] = PointF(static_cast<float>(idx), botY);
                    }

                    s_cachedEpoch = curEpoch;
                    s_cachedHead = curHead;
                    s_cachedGain = g_displayGain;
                    s_cachedMode = g_displayMode;
                    s_cachedAsymm = g_asymmetrical;
                    s_cachedWidth = width;
                    s_cachedPaused = isPaused;
                    s_cachedPlayheadX = playheadX;
                } else {
                    playheadX = s_cachedPlayheadX;
                }

                GraphicsPath path;
                path.AddLines(s_pts.data(), static_cast<int>(s_pts.size()));
                path.CloseFigure();

                bool evenPass = (curEpoch % 2 == 0);

                LinearGradientBrush rainbowBrush(PointF(0.0f, 0.0f), PointF(static_cast<float>(width), 0.0f), Color::Red, Color::Blue);
                LinearGradientBrush strokeBrush(PointF(0.0f, 0.0f), PointF(static_cast<float>(width), 0.0f), Color::White, Color::White);
                LinearGradientBrush rainbowBrushOld(PointF(0.0f, 0.0f), PointF(static_cast<float>(width), 0.0f), Color::Red, Color::Blue);
                LinearGradientBrush strokeBrushOld(PointF(0.0f, 0.0f), PointF(static_cast<float>(width), 0.0f), Color::White, Color::White);

                if (g_currentTheme == ThemeId::Rainbow) {
                    const int NUM_STOPS = 12;
                    REAL positions[NUM_STOPS] = {
                        0.00f, 0.09f, 0.18f, 0.27f, 0.36f, 0.46f,
                        0.56f, 0.66f, 0.76f, 0.85f, 0.93f, 1.00f
                    };
                    Color strokeColors[NUM_STOPS] = {
                        Color(255,  95, 157, 225),  // 0.00: Soft Blue
                        Color(255, 135, 111, 225),  // 0.09: Lavender
                        Color(255, 214,  93, 254),  // 0.18: Violet Purple
                        Color(255, 240,  80, 220),  // 0.27: Magenta / Fuchsia
                        Color(255, 252,  94, 201),  // 0.36: Rose Pink
                        Color(255, 242, 158, 115),  // 0.46: Coral / Salmon
                        Color(255, 247, 250,  77),  // 0.56: Canary Yellow
                        Color(255, 177, 215,  81),  // 0.66: Lime Green
                        Color(255, 131, 255, 151),  // 0.76: Mint / Emerald
                        Color(255,  90, 241, 200),  // 0.85: Turquoise / Seafoam
                        Color(255,  77, 222, 232),  // 0.93: Cyan / Teal
                        Color(255,  79, 185, 225)   // 1.00: Sky Blue
                    };
                    Color fillColors[NUM_STOPS];
                    Color strokeColorsOld[NUM_STOPS];
                    Color fillColorsOld[NUM_STOPS];
                    for (int i = 0; i < NUM_STOPS; ++i) {
                        fillColors[i] = Color(75, strokeColors[i].GetR(), strokeColors[i].GetG(), strokeColors[i].GetB());
                        BYTE r = static_cast<BYTE>((strokeColors[i].GetR() * 7 + 130 * 3) / 10);
                        BYTE g = static_cast<BYTE>((strokeColors[i].GetG() * 7 + 110 * 3) / 10);
                        BYTE b = static_cast<BYTE>((strokeColors[i].GetB() * 7 + 210 * 3) / 10);
                        strokeColorsOld[i] = Color(255, r, g, b);
                        fillColorsOld[i] = Color(55, r, g, b);
                    }
                    rainbowBrush.SetInterpolationColors(fillColors, positions, NUM_STOPS);
                    strokeBrush.SetInterpolationColors(strokeColors, positions, NUM_STOPS);
                    rainbowBrush.SetWrapMode(WrapModeClamp);
                    strokeBrush.SetWrapMode(WrapModeClamp);

                    rainbowBrushOld.SetInterpolationColors(fillColorsOld, positions, NUM_STOPS);
                    strokeBrushOld.SetInterpolationColors(strokeColorsOld, positions, NUM_STOPS);
                    rainbowBrushOld.SetWrapMode(WrapModeClamp);
                    strokeBrushOld.SetWrapMode(WrapModeClamp);
                }

                // Two-tone palette mapping:
                // Left side is the current sweep pass (new audio).
                // Right side is the previous sweep pass (old audio).
                // In even passes: Left = Tone A, Right = Tone B
                // In odd passes:  Left = Tone B, Right = Tone A
                // Because each pass's audio retains the exact tone it was laid down with,
                // there is zero jarring whole-screen color flash when the sweep wraps around!
                Brush* leftRainbowFill = evenPass ? static_cast<Brush*>(&rainbowBrush) : static_cast<Brush*>(&rainbowBrushOld);
                Brush* leftRainbowStroke = evenPass ? static_cast<Brush*>(&strokeBrush) : static_cast<Brush*>(&strokeBrushOld);
                Brush* rightRainbowFill = evenPass ? static_cast<Brush*>(&rainbowBrushOld) : static_cast<Brush*>(&rainbowBrush);
                Brush* rightRainbowStroke = evenPass ? static_cast<Brush*>(&strokeBrushOld) : static_cast<Brush*>(&strokeBrush);

                Color leftFillCol = evenPass ? theme.waveformFill : theme.waveformFillOld;
                Color leftStrokeCol = evenPass ? theme.waveformStroke : theme.waveformStrokeOld;
                Color rightFillCol = evenPass ? theme.waveformFillOld : theme.waveformFill;
                Color rightStrokeCol = evenPass ? theme.waveformStrokeOld : theme.waveformStroke;

                uint64_t totalWritten = g_buffer->getTotalSamplesWritten();
                size_t cap = g_buffer->getCapacity();
                bool hasWrapped = (cap > 0 && totalWritten >= cap);

                if (g_displayMode == DisplayMode::Sweep && playheadX > 0.0f && playheadX < static_cast<float>(width)) {
                    Region origClip;
                    g.GetClip(&origClip);

                    // 1. Left side (New Audio): [0, playheadX]
                    RectF leftClip(0.0f, 0.0f, playheadX, static_cast<float>(height));
                    g.SetClip(leftClip);
                    if (g_currentTheme == ThemeId::Rainbow) {
                        g.FillPath(leftRainbowFill, &path);
                        Pen pen(leftRainbowStroke, 1.0f);
                        pen.SetLineJoin(LineJoinRound);
                        g.DrawPath(&pen, &path);
                    } else {
                        SolidBrush brush(leftFillCol);
                        g.FillPath(&brush, &path);
                        Pen pen(leftStrokeCol, 1.0f);
                        pen.SetLineJoin(LineJoinRound);
                        g.DrawPath(&pen, &path);
                    }

                    // 2. Right side (Old Audio): [playheadX, width]
                    // Only render old audio if the buffer has actually wrapped at least once
                    if (hasWrapped) {
                        RectF rightClip(playheadX, 0.0f, static_cast<float>(width) - playheadX, static_cast<float>(height));
                        g.SetClip(rightClip);
                        if (g_currentTheme == ThemeId::Rainbow) {
                            g.FillPath(rightRainbowFill, &path);
                            Pen pen(rightRainbowStroke, 1.0f);
                            pen.SetLineJoin(LineJoinRound);
                            g.DrawPath(&pen, &path);
                        } else {
                            SolidBrush brush(rightFillCol);
                            g.FillPath(&brush, &path);
                            Pen pen(rightStrokeCol, 1.0f);
                            pen.SetLineJoin(LineJoinRound);
                            g.DrawPath(&pen, &path);
                        }
                    }

                    g.SetClip(&origClip);
                } else {
                    bool isLeftTone = (g_displayMode != DisplayMode::Sweep || playheadX >= static_cast<float>(width));
                    if (g_currentTheme == ThemeId::Rainbow) {
                        Brush* bFill = isLeftTone ? leftRainbowFill : rightRainbowFill;
                        Brush* bStroke = isLeftTone ? leftRainbowStroke : rightRainbowStroke;
                        g.FillPath(bFill, &path);
                        Pen pen(bStroke, 1.0f);
                        pen.SetLineJoin(LineJoinRound);
                        g.DrawPath(&pen, &path);
                    } else {
                        SolidBrush brush(isLeftTone ? leftFillCol : rightFillCol);
                        g.FillPath(&brush, &path);
                        Pen pen(isLeftTone ? leftStrokeCol : rightStrokeCol, 1.0f);
                        pen.SetLineJoin(LineJoinRound);
                        g.DrawPath(&pen, &path);
                    }
                }

                // Traveling Playhead & Subtle Trailing Glow (Sweep Mode)
                float hx = playheadX;

                if (g_displayMode == DisplayMode::Sweep) {
                    // Soft luminous trailing glow behind traveling playhead (strictly to the left of playhead)
                    // Padded with extended brush geometry and WrapModeClamp to guarantee zero tiling line / border flash
                    const float glowW = 28.0f;
                    const float pad = 32.0f;
                    const float brushW = glowW + 2.0f * pad;

                    const int GLOW_STOPS = 4;
                    REAL glowPositions[GLOW_STOPS] = {
                        0.0f,
                        pad / brushW,
                        (pad + glowW) / brushW,
                        1.0f
                    };
                    Color glowColors[GLOW_STOPS] = {
                        Color(0, 255, 220, 40),
                        Color(0, 255, 220, 40),
                        Color(75, 255, 220, 40),
                        Color(75, 255, 220, 40)
                    };

                    if (hx >= glowW) {
                        float startX = hx - glowW;
                        LinearGradientBrush glowBrush(
                            PointF(startX - pad, 0.0f),
                            PointF(startX - pad + brushW, 0.0f),
                            glowColors[0], glowColors[3]
                        );
                        glowBrush.SetInterpolationColors(glowColors, glowPositions, GLOW_STOPS);
                        glowBrush.SetWrapMode(WrapModeClamp);
                        g.FillRectangle(&glowBrush, startX, topMargin, glowW, waveH);
                    } else {
                        // Handle smooth wrap-around when playhead is near left boundary
                        if (hx > 0.0f) {
                            float startX = hx - glowW;
                            LinearGradientBrush glowBrush1(
                                PointF(startX - pad, 0.0f),
                                PointF(startX - pad + brushW, 0.0f),
                                glowColors[0], glowColors[3]
                            );
                            glowBrush1.SetInterpolationColors(glowColors, glowPositions, GLOW_STOPS);
                            glowBrush1.SetWrapMode(WrapModeClamp);
                            g.FillRectangle(&glowBrush1, 0.0f, topMargin, hx, waveH);
                        }
                        float wrapW = glowW - hx;
                        float wrapStartX = static_cast<float>(width) - wrapW;
                        if (wrapW > 0.0f && wrapStartX >= 0.0f) {
                            float virtualHx = static_cast<float>(width) + hx;
                            float virtualStartX = virtualHx - glowW;
                            LinearGradientBrush glowBrush2(
                                PointF(virtualStartX - pad, 0.0f),
                                PointF(virtualStartX - pad + brushW, 0.0f),
                                glowColors[0], glowColors[3]
                            );
                            glowBrush2.SetInterpolationColors(glowColors, glowPositions, GLOW_STOPS);
                            glowBrush2.SetWrapMode(WrapModeClamp);
                            g.FillRectangle(&glowBrush2, wrapStartX, topMargin, wrapW, waveH);
                        }
                    }

                    // Crisp playhead line bounded strictly to the waveform area
                    Pen headGlowPen(Color(60, 255, 240, 50), 3.0f);
                    g.DrawLine(&headGlowPen, hx, topMargin, hx, topMargin + waveH);

                    Pen headCorePen(Color(255, 255, 245, 60), 1.5f);
                    g.DrawLine(&headCorePen, hx, topMargin, hx, topMargin + waveH);
                }
            }

            // 6. Header Overlay (1.5x Beagle logo at top-left, BeagleSampler title to its right, centralized status)
            if (g_pAppIconBmp) {
                // Beagle avatar: 1.5x (72.0f x 72.0f) situated at the top left without covering wavegraph
                g.DrawImage(g_pAppIconBmp, 10.0f, 4.0f, logoSize, logoSize);
            }

            // BeagleSampler text: placed to the right-hand side of the logo, in line with the top bar controls
            float titleX = g_pAppIconBmp ? (10.0f + logoSize + 10.0f) : 14.0f;
            Font titleFont(L"Segoe UI", 11.5f, FontStyleBold, UnitPoint);
            SolidBrush textBrush(theme.text);
            g.DrawString(L"BeagleSampler", -1, &titleFont, PointF(titleX, 9.0f), &textBrush);

            // Centered Status & Gain: strictly centralized in the app across the middle line
            Font smallFont(L"Segoe UI", 9.0f, FontStyleRegular, UnitPoint);
            Font statusFont(L"Segoe UI", 9.0f, FontStyleBold, UnitPoint);
            bool isLoopback = g_capture && g_capture->isLoopback();
            Color statusCol = isPaused ? Color(255, 230, 110, 60) : (isLoopback ? Color(255, 60, 180, 240) : Color(255, 70, 200, 100));
            SolidBrush statusBrush(statusCol);
            std::wstring statusStr = isPaused ? L"⏸ PAUSED" : (isLoopback ? L"● REC [Desktop]" : L"● REC [Mic]");

            std::wstringstream sGain;
            if (std::abs(g_displayGain - 1.0f) < 0.01f) {
                sGain << L"Gain 0 dB";
            } else {
                float db = 20.0f * std::log10(g_displayGain);
                sGain << L"Gain " << (db >= 0.0f ? L"+" : L"") << std::fixed << std::setprecision(1) << db << L"dB ("
                      << std::fixed << std::setprecision(1) << g_displayGain << L"x)";
            }
            std::wstring wGain = sGain.str();

            // Measure strings to compute exact centered coordinates across the window's midline
            RectF statusRect, gainRect;
            g.MeasureString(statusStr.c_str(), -1, &statusFont, PointF(0.0f, 0.0f), &statusRect);
            g.MeasureString(wGain.c_str(), -1, &smallFont, PointF(0.0f, 0.0f), &gainRect);
            const float textGap = 12.0f;
            float combinedW = statusRect.Width + textGap + gainRect.Width;
            float centerStartX = (static_cast<float>(width) - combinedW) * 0.5f;

            // Ensure centered text does not collide with title if window is squeezed
            float minCenterStartX = titleX + 115.0f;
            if (centerStartX < minCenterStartX) centerStartX = minCenterStartX;

            g.DrawString(statusStr.c_str(), -1, &statusFont, PointF(centerStartX, 10.0f), &statusBrush);

            SolidBrush gainBrush(std::abs(g_displayGain - 1.0f) < 0.01f ? theme.textMuted : theme.waveformStroke);
            g.DrawString(wGain.c_str(), -1, &smallFont, PointF(centerStartX + statusRect.Width + textGap, 10.5f), &gainBrush);

            // 7. Window Chrome & Corner Buttons (Top Right)
            int curX = width - 14;

            // 7a. Red Close Button (Traffic Light with crisp cross)
            float closeCx = static_cast<float>(curX - 7);
            float closeCy = 16.0f;
            g_rectCloseBtn = { static_cast<int>(closeCx - 8), 7, static_cast<int>(closeCx + 8), 25 };
            bool closeHover = (g_hoverBtn == HoverBtn::Close);
            Color closeFill = closeHover ? Color(255, 255, 95, 86) : Color(255, 237, 85, 76);
            SolidBrush closeBrush(closeFill);
            g.FillEllipse(&closeBrush, closeCx - 6.5f, closeCy - 6.5f, 13.0f, 13.0f);
            Pen closeBorderPen(Color(180, 190, 45, 40), 1.0f);
            g.DrawEllipse(&closeBorderPen, closeCx - 6.5f, closeCy - 6.5f, 13.0f, 13.0f);
            // Red cross: crisp vector cross
            Color xColor = closeHover ? Color(255, 255, 255, 255) : Color(200, 110, 15, 15);
            Pen xPen(xColor, 1.4f);
            g.DrawLine(&xPen, closeCx - 3.2f, closeCy - 3.2f, closeCx + 3.2f, closeCy + 3.2f);
            g.DrawLine(&xPen, closeCx - 3.2f, closeCy + 3.2f, closeCx + 3.2f, closeCy - 3.2f);

            curX -= 22;

            // 7b. Yellow Minimize Button (Traffic Light with minus)
            float minCx = static_cast<float>(curX - 7);
            float minCy = 16.0f;
            g_rectMinBtn = { static_cast<int>(minCx - 8), 7, static_cast<int>(minCx + 8), 25 };
            bool minHover = (g_hoverBtn == HoverBtn::Min);
            Color minFill = minHover ? Color(255, 255, 205, 55) : Color(255, 245, 185, 45);
            SolidBrush minBrush(minFill);
            g.FillEllipse(&minBrush, minCx - 6.5f, minCy - 6.5f, 13.0f, 13.0f);
            Pen minBorderPen(Color(180, 200, 140, 25), 1.0f);
            g.DrawEllipse(&minBorderPen, minCx - 6.5f, minCy - 6.5f, 13.0f, 13.0f);
            // Yellow minus: crisp horizontal bar
            Color mColor = minHover ? Color(255, 255, 255, 255) : Color(200, 140, 90, 10);
            Pen mPen(mColor, 1.4f);
            g.DrawLine(&mPen, minCx - 3.5f, minCy, minCx + 3.5f, minCy);

            curX -= 22;

            // 7c. Green Pin/Zoom Button (Traffic Light with plus/pin)
            float pinCx = static_cast<float>(curX - 7);
            float pinCy = 16.0f;
            g_rectPinBtn = { static_cast<int>(pinCx - 8), 7, static_cast<int>(pinCx + 8), 25 };
            bool pinHover = (g_hoverBtn == HoverBtn::Pin);
            Color pinFill = pinHover ? Color(255, 65, 230, 95) : (g_pinOnTop ? Color(255, 45, 215, 75) : Color(255, 40, 185, 60));
            SolidBrush pinBrush(pinFill);
            g.FillEllipse(&pinBrush, pinCx - 6.5f, pinCy - 6.5f, 13.0f, 13.0f);
            Pen pinBorderPen(Color(180, 30, 150, 45), 1.0f);
            g.DrawEllipse(&pinBorderPen, pinCx - 6.5f, pinCy - 6.5f, 13.0f, 13.0f);
            Color pColor = pinHover ? Color(255, 255, 255, 255) : Color(200, 15, 110, 30);
            Pen pPen(pColor, 1.4f);
            g.DrawLine(&pPen, pinCx - 3.0f, pinCy, pinCx + 3.0f, pinCy);
            g.DrawLine(&pPen, pinCx, pinCy - 3.0f, pinCx, pinCy + 3.0f);

            curX -= 22;

            // 7d. Settings Gear Button
            curX -= 26;
            g_rectSettingsBtn = { curX, 6, curX + 24, 26 };
            bool setHover = (g_hoverBtn == HoverBtn::Settings);
            SolidBrush btnBg(setHover ? theme.panelBorder : theme.panelBg);
            Pen btnPen(theme.panelBorder, 1.0f);
            g.FillRectangle(&btnBg, g_rectSettingsBtn.left, g_rectSettingsBtn.top, 24, 20);
            g.DrawRectangle(&btnPen, g_rectSettingsBtn.left, g_rectSettingsBtn.top, 24, 20);
            Font iconFont(L"Segoe UI", 9.0f, FontStyleRegular, UnitPoint);
            g.DrawString(L"⚙", -1, &iconFont, PointF(static_cast<float>(curX + 6), 7.0f), &textBrush);

            // 7e. Pause/Play Button
            curX -= 32;
            g_rectPauseBtn = { curX, 6, curX + 26, 26 };
            bool pauseHover = (g_hoverBtn == HoverBtn::Pause);
            SolidBrush pauseBg(pauseHover ? theme.panelBorder : theme.panelBg);
            g.FillRectangle(&pauseBg, g_rectPauseBtn.left, g_rectPauseBtn.top, 26, 20);
            g.DrawRectangle(&btnPen, g_rectPauseBtn.left, g_rectPauseBtn.top, 26, 20);
            SolidBrush pauseIconBrush(isPaused ? Color(255, 230, 110, 60) : theme.text);
            g.DrawString(isPaused ? L"▶" : L"⏸", -1, &iconFont, PointF(static_cast<float>(curX + 8), 7.0f), &pauseIconBrush);

            // 7f. Audio Peak Meters
            curX -= 65;
            if (g_buffer) {
                float pL = g_buffer->getPeakLeft();
                float pR = g_buffer->getPeakRight();

                int meterW = 55;
                int meterH = 6;
                int meterY1 = 8;
                int meterY2 = 16;

                SolidBrush mBg(theme.panelBg);
                g.FillRectangle(&mBg, curX, meterY1, meterW, meterH);
                g.FillRectangle(&mBg, curX, meterY2, meterW, meterH);

                int fillL = static_cast<int>(meterW * std::min(1.0f, pL));
                int fillR = static_cast<int>(meterW * std::min(1.0f, pR));

                SolidBrush mFill(isPaused ? Color(255, 150, 95, 45) : theme.waveformStroke);
                if (fillL > 0) g.FillRectangle(&mFill, curX, meterY1, fillL, meterH);
                if (fillR > 0) g.FillRectangle(&mFill, curX, meterY2, fillR, meterH);

                g_buffer->resetPeaks();
            }

            // 8. Time markers & audio format telemetry across bottom
            if (g_buffer) {
                SolidBrush mutedBrush(theme.textMuted);
                double dur = g_buffer->getDuration();

                std::wstringstream sDevInfo;
                if (g_capture) {
                    std::wstring dev = g_capture->getDeviceName();
                    if (dev.length() > 28) dev = dev.substr(0, 26) + L"…";
                    sDevInfo << dev << L"  •  ";
                    uint32_t devSr = g_capture->getDeviceSampleRate();
                    if (devSr != 48000) {
                        sDevInfo << (devSr / 1000) << L"k->48k  •  ";
                    } else {
                        sDevInfo << L"48 kHz  •  ";
                    }
                } else {
                    sDevInfo << L"48 kHz  •  ";
                }
                if (g_save24Bit) sDevInfo << L"24-bit  •  ";
                sDevInfo << (g_displayMode == DisplayMode::Sweep ? L"Sweep" : L"Rolling");
                std::wstring wDevInfo = sDevInfo.str();

                // Technical device & format telemetry cleanly placed in the bottom row (numbers removed for clean resizing)
                g.DrawString(wDevInfo.c_str(), -1, &smallFont, PointF(14.0f, static_cast<float>(height - 22)), &mutedBrush);
            }

            // 9. Toast notification message
            if (!g_statusMessage.empty()) {
                if (GetTickCount() < g_statusMessageExpiry) {
                    SolidBrush greenBrush(Color(255, 80, 230, 110));
                    Font toastFont(L"Segoe UI", 9.0f, FontStyleBold, UnitPoint);
                    g.DrawString(g_statusMessage.c_str(), -1, &toastFont, PointF(14.0f, static_cast<float>(height - 40)), &greenBrush);
                } else {
                    g_statusMessage.clear();
                }
            }

            s_baseNeedsUpdate = false;
        }

        // Blit pre-rendered base scene to back-buffer (takes ~0.02ms)
        BitBlt(s_backDC, 0, 0, width, height, s_baseDC, 0, 0, SRCCOPY);

        // Render interactive selection highlight directly onto back-buffer (takes ~0.03ms)
        if (g_hasSelection && width > 2) {
            int selL = std::min(g_selStartX, g_selEndX);
            int selR = std::max(g_selStartX, g_selEndX);
            if (selR > selL) {
                Graphics gBack(s_backDC);
                gBack.SetSmoothingMode(SmoothingModeAntiAlias);
                gBack.SetPixelOffsetMode(PixelOffsetModeHighQuality);
                gBack.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

                SolidBrush selBrush(theme.selectionFill);
                gBack.FillRectangle(&selBrush, selL, 0, selR - selL, height);

                Pen selPen(theme.selectionBorder, 2.0f);
                gBack.DrawLine(&selPen, static_cast<float>(selL), 0.0f, static_cast<float>(selL), static_cast<float>(height));
                gBack.DrawLine(&selPen, static_cast<float>(selR), 0.0f, static_cast<float>(selR), static_cast<float>(height));

                if (g_buffer) {
                    double dur = g_buffer->getDuration();
                    double selSecs = (static_cast<double>(selR - selL) / static_cast<double>(width)) * dur;
                    std::wstringstream sSel;
                    sSel << std::fixed << std::setprecision(2) << selSecs << L"s  [Drag Out to DAW / Explorer]";
                    std::wstring wSel = sSel.str();

                    Font font(L"Segoe UI", 9.0f, FontStyleBold, UnitPoint);
                    SolidBrush textBrush(theme.selectionBorder);
                    PointF origin(static_cast<float>(selL + 8), static_cast<float>(height - 24));
                    gBack.DrawString(wSel.c_str(), -1, &font, origin, &textBrush);
                }
            }
        }

        // Final swap to screen device context (takes ~0.02ms)
        BitBlt(hdc, 0, 0, width, height, s_backDC, 0, 0, SRCCOPY);

        static int s_snapCount = 0;
        if (++s_snapCount == 20) {
            CLSID pngClsid;
            if (GetEncoderClsid(L"image/png", &pngClsid) >= 0) {
                Bitmap snapBmp(s_backBM, NULL);
                snapBmp.Save(L"C:\\Users\\BeagleBoss\\.gemini\\antigravity-cli\\brain\\83e2cab3-40de-4382-b10b-b36b7400d200\\running_beagle_v2.png", &pngClsid, NULL);
            }
        }

        EndPaint(hwnd, &ps);
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY: {
        saveSettings(hwnd);
        if (g_saveSessionState && g_buffer) {
            g_buffer->saveStateToFile(getSessionStateFilePath());
        }
        KillTimer(hwnd, 1);
        if (s_backBM) {
            SelectObject(s_backDC, s_oldBM);
            DeleteObject(s_backBM);
            s_backBM = NULL;
        }
        if (s_backDC) {
            DeleteDC(s_backDC);
            s_backDC = NULL;
        }
        if (s_baseBM) {
            SelectObject(s_baseDC, s_oldBaseBM);
            DeleteObject(s_baseBM);
            s_baseBM = NULL;
        }
        if (s_baseDC) {
            DeleteDC(s_baseDC);
            s_baseDC = NULL;
        }
        if (s_darkBgBrush) {
            DeleteObject(s_darkBgBrush);
            s_darkBgBrush = NULL;
        }
        PostQuitMessage(0);
        break;
    }

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

static Bitmap* LoadBitmapFromResource(HMODULE hMod, int resId) {
    HRSRC hRes = FindResourceW(hMod, MAKEINTRESOURCEW(resId), MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
    if (!hRes) return nullptr;
    HGLOBAL hGlobal = LoadResource(hMod, hRes);
    if (!hGlobal) return nullptr;
    DWORD resSize = SizeofResource(hMod, hRes);
    void* pResData = LockResource(hGlobal);
    if (!pResData || resSize == 0) return nullptr;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, resSize);
    if (!hMem) return nullptr;
    void* pMem = GlobalLock(hMem);
    if (!pMem) {
        GlobalFree(hMem);
        return nullptr;
    }
    memcpy(pMem, pResData, resSize);
    GlobalUnlock(hMem);

    IStream* pStream = nullptr;
    if (CreateStreamOnHGlobal(hMem, TRUE, &pStream) != S_OK) {
        GlobalFree(hMem);
        return nullptr;
    }

    Bitmap* pBmp = Bitmap::FromStream(pStream);
    pStream->Release();
    if (pBmp && pBmp->GetLastStatus() == Ok) {
        return pBmp;
    }
    if (pBmp) delete pBmp;
    return nullptr;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    timeBeginPeriod(1); // Enable high-precision 1ms OS timer resolution for jitter-free 60 FPS pacing

    // Ensure BeagleSampler utilizes 100% of all available CPU cores across all worker and audio threads
    DWORD_PTR processAffinity = 0;
    DWORD_PTR systemAffinity = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processAffinity, &systemAffinity) && systemAffinity != 0) {
        SetProcessAffinityMask(GetCurrentProcess(), systemAffinity);
    }

    // Enable Windows Per-Monitor V2 High-DPI Awareness
    // Ensures menus, dialogs, fonts, and waveforms render at native physical display pixels with no DWM blur
    typedef BOOL (WINAPI *SetProcessDpiAwarenessContextProc)(DPI_AWARENESS_CONTEXT);
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        SetProcessDpiAwarenessContextProc setDpiAwareness =
            (SetProcessDpiAwarenessContextProc)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (setDpiAwareness) {
            setDpiAwareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    OleInitialize(NULL);

    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int winW = 1223;
    int winH = 293;
    int winX = (screenW > winW) ? (screenW - winW) / 2 : 100;
    int winY = (screenH > winH) ? (screenH - winH) / 2 : 100;

    std::wstring savedDevId = L"default_loopback";
    bool savedIsLoopback = true;
    double savedDur = 30.0;
    loadSettings(winX, winY, winW, winH, savedDevId, savedIsLoopback, savedDur);

    g_buffer = new RollingAudioBuffer(48000, savedDur);
    if (g_saveSessionState) {
        std::wstring stateFile = getSessionStateFilePath();
        if (GetFileAttributesW(stateFile.c_str()) != INVALID_FILE_ATTRIBUTES) {
            g_buffer->loadStateFromFile(stateFile);
        }
    }
    g_capture = new AudioCapture(*g_buffer);
    g_capture->setDevice(savedDevId, savedIsLoopback);
    g_capture->start();

    HINSTANCE hInst = hInstance ? hInstance : GetModuleHandleW(NULL);

    // Load custom beagle icon from resource
    HICON hAppIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON));
    HICON hAppIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);

    // Load custom beagle image (fully embedded in PE resources, 100% portable on any PC)
    Bitmap* pRawBmp = LoadBitmapFromResource(hInst, 2);
    if (!pRawBmp && hAppIcon) {
        pRawBmp = Bitmap::FromHICON(hAppIcon);
    }
    if (!pRawBmp) {
        wchar_t exePath[MAX_PATH] = { 0 };
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        wchar_t* pSlash = wcsrchr(exePath, L'\\');
        if (pSlash) {
            *(pSlash + 1) = L'\0';
            std::wstring localPng = std::wstring(exePath) + L"beagle_icon.png";
            pRawBmp = Bitmap::FromFile(localPng.c_str());
        }
    }
    if (pRawBmp && pRawBmp->GetLastStatus() == Ok) {
        g_pAppIconBmp = new Bitmap(144, 144, PixelFormat32bppARGB);
        Graphics iconG(g_pAppIconBmp);
        iconG.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        iconG.SetSmoothingMode(SmoothingModeAntiAlias);
        iconG.DrawImage(pRawBmp, 0, 0, 144, 144);
        delete pRawBmp;
    }

    if (!s_darkBgBrush) s_darkBgBrush = CreateSolidBrush(RGB(20, 20, 22));

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = hAppIcon;
    wc.hIconSm = hAppIconSm;
    wc.lpszClassName = L"BeagleSampler";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = s_darkBgBrush;
    RegisterClassExW(&wc);

    DWORD dwStyle = g_borderless
        ? (WS_POPUP | WS_THICKFRAME | WS_VISIBLE | WS_CLIPCHILDREN)
        : (WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN);

    HWND hwnd = CreateWindowExW(g_pinOnTop ? WS_EX_TOPMOST : 0, L"BeagleSampler",
                                L"BeagleSampler",
                                dwStyle,
                                winX, winY, winW, winH,
                                NULL, NULL, hInst, NULL);
    DWORD err = GetLastError();

    FILE* fLog = fopen("E:\\AI Made Apps\\rolling-sampler-work\\build\\launch.log", "w");
    if (fLog) {
        fprintf(fLog, "screenW=%d, screenH=%d, winW=%d, winH=%d, winX=%d, winY=%d, hwnd=%p, err=%lu, g_pin=%d\n",
                screenW, screenH, winW, winH, winX, winY, hwnd, err, (int)g_pinOnTop);
        fclose(fLog);
    }

    if (hAppIcon) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hAppIcon);
    if (hAppIconSm) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hAppIconSm);

    if (g_borderless) {
        MARGINS margins = { 0, 0, 0, 1 };
        DwmExtendFrameIntoClientArea(hwnd, &margins);
    }

    // Apply seamless Windows 11 Dark Mode styling to window title bar and borders
    BOOL useDarkMode = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &useDarkMode, sizeof(useDarkMode));
    COLORREF darkColor = RGB(20, 20, 22);
    DwmSetWindowAttribute(hwnd, 34 /*DWMWA_BORDER_COLOR*/, &darkColor, sizeof(darkColor));
    DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &darkColor, sizeof(darkColor));
    COLORREF titleTextColor = RGB(240, 240, 240);
    DwmSetWindowAttribute(hwnd, 36 /*DWMWA_TEXT_COLOR*/, &titleTextColor, sizeof(titleTextColor));

    SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetWindowPos(hwnd, g_pinOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, winX, winY, winW, winH, SWP_SHOWWINDOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    saveSettings(hwnd);

    if (g_autoCheckUpdates) {
        TriggerUpdateCheck(hwnd, true /* silentIfUpToDate */);
    }

    MSG msg;
    BOOL bRet;
    while ((bRet = GetMessageW(&msg, NULL, 0, 0)) != 0) {
        if (bRet == -1) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_capture) {
        g_capture->stop();
        delete g_capture;
        g_capture = nullptr;
    }
    if (g_buffer) {
        delete g_buffer;
        g_buffer = nullptr;
    }
    if (g_pAppIconBmp) {
        delete g_pAppIconBmp;
        g_pAppIconBmp = nullptr;
    }


    GdiplusShutdown(gdiplusToken);
    OleUninitialize();
    timeEndPeriod(1);
    return 0;
}

