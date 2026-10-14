#include "WaveformEditor.h"
#include <ole2.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <mmsystem.h>
#include <dwmapi.h>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>

#define IDI_APP_ICON 1

// Menu Command IDs
#define IDM_DUR_15          1001
#define IDM_DUR_30          1002
#define IDM_DUR_60          1003
#define IDM_DUR_90          1004
#define IDM_DUR_120         1005
#define IDM_DUR_180         1006
#define IDM_DUR_300         1007

#define IDM_THEME_CARBON    1011
#define IDM_THEME_RAINBOW   1012
#define IDM_THEME_BRIGHT    1013
#define IDM_THEME_LEMON     1014
#define IDM_THEME_VINTAGE   1015

#define IDM_GAIN_P14        1021
#define IDM_GAIN_P10        1022
#define IDM_GAIN_P6         1023
#define IDM_GAIN_0          1024
#define IDM_GAIN_M6         1025
#define IDM_GAIN_M12        1026

#define IDM_DEVICE_LOOPBACK 1030
#define IDM_DEVICE_BASE     1040

#define IDM_CH_STEREO       1050
#define IDM_CH_LEFT         1051
#define IDM_CH_RIGHT        1052

#define IDM_FORMAT_16BIT    1060
#define IDM_FORMAT_24BIT    1061

#define IDM_TRIM_SILENCE    1070
#define IDM_APPEND_BPM      1071
#define IDM_ASYMMETRICAL    1072
#define IDM_PIN_ON_TOP      1073
#define IDM_FPS_60          1074
#define IDM_FPS_30          1075
#define IDM_FPS_20          1076
#define IDM_FPS_15          1077
#define IDM_CLEAR           1078
#define IDM_EXPORT_SEL      1079
#define IDM_PREVIEW         1080
#define IDM_BORDERLESS      1081
#define IDM_DISPLAY_MODE    1082
#define IDM_SAVE_SESSION_STATE 1083

using namespace Gdiplus;

namespace RollingSampler {

HBRUSH WaveformEditor::s_darkBgBrush = NULL;
bool WaveformEditor::s_classRegistered = false;

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

static ThemeColors getThemeColors(int themeId, bool isPaused) {
    switch (themeId) {
    case 1: // Carbon
        return {
            Color(255, 20, 20, 20),
            Color(255, 26, 26, 26),
            Color(255, 42, 42, 42),
            isPaused ? Color(55, 185, 125, 55) : Color(75, 245, 160, 50),     // waveformFill (New: Translucent Amber Gold)
            isPaused ? Color(255, 205, 145, 75) : Color(255, 255, 200, 90),   // waveformStroke (New: Bright Amber Gold)
            isPaused ? Color(55, 175, 95, 75) : Color(75, 230, 115, 80),      // waveformFillOld (Old: Translucent Coral)
            isPaused ? Color(255, 195, 120, 100) : Color(255, 255, 150, 120), // waveformStrokeOld (Old: Terracotta Coral)
            Color(255, 42, 42, 42),
            Color(255, 28, 28, 28),
            Color(255, 240, 240, 240),
            Color(255, 130, 130, 130),
            Color(255, 255, 215, 140),
            Color(75, 240, 150, 80),
            Color(255, 255, 190, 120),
            L"Carbon"
        };
    case 2: // Rainbow
        return {
            Color(255, 18, 18, 20),
            Color(255, 24, 24, 28),
            Color(255, 40, 40, 48),
            Color(75, 255, 120, 160),                             // waveformFill (Translucent)
            Color(255, 255, 200, 220),                            // waveformStroke (Opaque)
            Color(55, 210, 130, 220),                             // waveformFillOld (Translucent)
            Color(255, 230, 180, 240),                            // waveformStrokeOld (Opaque)
            Color(255, 45, 45, 55),
            Color(255, 30, 30, 38),
            Color(255, 245, 245, 245),
            Color(255, 140, 140, 150),
            Color(255, 255, 255, 255),
            Color(80, 255, 255, 255),
            Color(255, 255, 255, 255),
            L"Rainbow"
        };
    case 3: // Bright
        return {
            Color(255, 245, 245, 245),
            Color(255, 255, 255, 255),
            Color(255, 210, 210, 210),
            isPaused ? Color(50, 75, 105, 140) : Color(70, 30, 85, 165),      // waveformFill (New: Translucent Cobalt Slate)
            isPaused ? Color(255, 75, 105, 140) : Color(255, 18, 70, 145),    // waveformStroke (New: Deep Cobalt)
            isPaused ? Color(50, 65, 120, 125) : Color(70, 20, 125, 135),     // waveformFillOld (Old: Translucent Ocean Teal)
            isPaused ? Color(255, 65, 120, 125) : Color(255, 12, 105, 115),   // waveformStrokeOld (Old: Deep Ocean Teal)
            Color(255, 215, 215, 215),
            Color(255, 230, 230, 230),
            Color(255, 20, 20, 20),
            Color(255, 110, 110, 110),
            Color(255, 0, 120, 215),
            Color(65, 0, 120, 215),
            Color(255, 0, 120, 215),
            L"Bright"
        };
    case 4: // Lemon
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
    case 5: // Vintage
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
        return getThemeColors(1, isPaused);
    }
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
    STDMETHODIMP GiveFeedback(DWORD) override {
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
    STDMETHODIMP GetData(FORMATETC* pformatetcIn, STGMEDIUM* pmedium) override {
        if (!pformatetcIn || !pmedium) return E_POINTER;
        if (pformatetcIn->cfFormat == CF_HDROP && (pformatetcIn->tymed & TYMED_HGLOBAL)) {
            size_t pathBytes = (filePath.length() + 2) * sizeof(wchar_t);
            size_t totalBytes = sizeof(DROPFILES) + pathBytes;
            HGLOBAL hMem = GlobalAlloc(GHND | GMEM_SHARE, totalBytes);
            if (!hMem) return E_OUTOFMEMORY;
            DROPFILES* df = static_cast<DROPFILES*>(GlobalLock(hMem));
            df->pFiles = sizeof(DROPFILES);
            df->fWide = TRUE;
            wchar_t* pDest = reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(df) + sizeof(DROPFILES));
            memcpy(pDest, filePath.c_str(), filePath.length() * sizeof(wchar_t));
            pDest[filePath.length()] = L'\0';
            pDest[filePath.length() + 1] = L'\0';
            GlobalUnlock(hMem);

            pmedium->tymed = TYMED_HGLOBAL;
            pmedium->hGlobal = hMem;
            pmedium->pUnkForRelease = nullptr;
            return S_OK;
        }
        return DV_E_FORMATETC;
    }
    STDMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    STDMETHODIMP QueryGetData(FORMATETC* pformatetc) override {
        if (!pformatetc) return E_POINTER;
        if (pformatetc->cfFormat == CF_HDROP && (pformatetc->tymed & TYMED_HGLOBAL)) return S_OK;
        return DV_E_FORMATETC;
    }
    STDMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override { return DATA_S_SAMEFORMATETC; }
    STDMETHODIMP SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    STDMETHODIMP EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC** ppenumFormatEtc) override {
        if (dwDirection == DATADIR_GET) {
            FORMATETC fe = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
            *ppenumFormatEtc = new FormatEtcEnum(fe);
            return S_OK;
        }
        return E_NOTIMPL;
    }
    STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    STDMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    STDMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
private:
    LONG refCount;
    std::wstring filePath;
};

static bool exportWav(const std::wstring& path, const float* left, const float* right, size_t numSamples,
                      int sampleRate, int numChannels, bool save24Bit) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    int bytesPerSample = save24Bit ? 3 : 2;
    int blockAlign = numChannels * bytesPerSample;
    int byteRate = sampleRate * blockAlign;
    DWORD dataSize = static_cast<DWORD>(numSamples * blockAlign);
    DWORD riffSize = 36 + dataSize;

    DWORD written = 0;
    WriteFile(hFile, "RIFF", 4, &written, NULL);
    WriteFile(hFile, &riffSize, 4, &written, NULL);
    WriteFile(hFile, "WAVEfmt ", 8, &written, NULL);
    DWORD subchunk1Size = 16;
    WORD audioFormat = 1;
    WORD channels = static_cast<WORD>(numChannels);
    DWORD sRate = sampleRate;
    WORD bitsPerSample = save24Bit ? 24 : 16;
    WORD align = static_cast<WORD>(blockAlign);
    WriteFile(hFile, &subchunk1Size, 4, &written, NULL);
    WriteFile(hFile, &audioFormat, 2, &written, NULL);
    WriteFile(hFile, &channels, 2, &written, NULL);
    WriteFile(hFile, &sRate, 4, &written, NULL);
    WriteFile(hFile, &byteRate, 4, &written, NULL);
    WriteFile(hFile, &align, 2, &written, NULL);
    WriteFile(hFile, &bitsPerSample, 2, &written, NULL);
    WriteFile(hFile, "data", 4, &written, NULL);
    WriteFile(hFile, &dataSize, 4, &written, NULL);

    const size_t CHUNK = 4096;
    std::vector<uint8_t> buf(CHUNK * blockAlign);
    size_t writtenSamples = 0;

    while (writtenSamples < numSamples) {
        size_t toWrite = std::min(CHUNK, numSamples - writtenSamples);
        uint8_t* p = buf.data();
        for (size_t i = 0; i < toWrite; ++i) {
            size_t idx = writtenSamples + i;
            float l = std::clamp(left[idx], -1.0f, 1.0f);
            float r = (right && numChannels > 1) ? std::clamp(right[idx], -1.0f, 1.0f) : l;

            if (save24Bit) {
                int32_t valL = static_cast<int32_t>(l * 8388607.0f);
                *p++ = static_cast<uint8_t>(valL & 0xFF);
                *p++ = static_cast<uint8_t>((valL >> 8) & 0xFF);
                *p++ = static_cast<uint8_t>((valL >> 16) & 0xFF);
                if (numChannels > 1) {
                    int32_t valR = static_cast<int32_t>(r * 8388607.0f);
                    *p++ = static_cast<uint8_t>(valR & 0xFF);
                    *p++ = static_cast<uint8_t>((valR >> 8) & 0xFF);
                    *p++ = static_cast<uint8_t>((valR >> 16) & 0xFF);
                }
            } else {
                int16_t valL = static_cast<int16_t>(l * 32767.0f);
                *p++ = static_cast<uint8_t>(valL & 0xFF);
                *p++ = static_cast<uint8_t>((valL >> 8) & 0xFF);
                if (numChannels > 1) {
                    int16_t valR = static_cast<int16_t>(r * 32767.0f);
                    *p++ = static_cast<uint8_t>(valR & 0xFF);
                    *p++ = static_cast<uint8_t>((valR >> 8) & 0xFF);
                }
            }
        }
        DWORD bWritten = 0;
        WriteFile(hFile, buf.data(), static_cast<DWORD>(toWrite * blockAlign), &bWritten, NULL);
        writtenSamples += toWrite;
    }

    CloseHandle(hFile);
    return true;
}

WaveformEditor::WaveformEditor(RollingAudioBuffer* buffer, AudioCapture* capture, bool isChild)
    : m_buffer(buffer), m_capture(capture), m_isChild(isChild) {
    m_settings = SettingsManager::loadSettings();
}

WaveformEditor::~WaveformEditor() {
    destroy();
}

void WaveformEditor::registerClass(HINSTANCE hInstance) {
    if (s_classRegistered) return;
    if (!s_darkBgBrush) s_darkBgBrush = CreateSolidBrush(RGB(20, 20, 22));

    HICON hAppIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    HICON hAppIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WaveformEditor::WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = hAppIcon;
    wc.hIconSm = hAppIconSm;
    wc.lpszClassName = L"BeagleSamplerEditorClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = s_darkBgBrush;
    RegisterClassExW(&wc);
    s_classRegistered = true;
}

HWND WaveformEditor::create(HWND parentHwnd, HINSTANCE hInstance, int x, int y, int w, int h) {
    m_parentHwnd = parentHwnd;
    m_hInstance = hInstance ? hInstance : GetModuleHandleW(NULL);
    registerClass(m_hInstance);

    // Pre-scale Beagle logo to 144x144 bicubic thumbnail (fully embedded in PE resources, 100% portable on any PC)
    if (!m_pAppIconBmp) {
        HMODULE hMod = m_hInstance ? m_hInstance : GetModuleHandleW(NULL);
        Bitmap* pRaw = LoadBitmapFromResource(hMod, 2);
        if (!pRaw) {
            HICON hIco = (HICON)LoadImageW(hMod, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 144, 144, LR_DEFAULTCOLOR);
            if (hIco) {
                pRaw = Bitmap::FromHICON(hIco);
                DestroyIcon(hIco);
            }
        }
        if (!pRaw) {
            wchar_t modPath[MAX_PATH] = { 0 };
            GetModuleFileNameW(hMod, modPath, MAX_PATH);
            wchar_t* pSlash = wcsrchr(modPath, L'\\');
            if (pSlash) {
                *(pSlash + 1) = L'\0';
                std::wstring localPng = std::wstring(modPath) + L"beagle_icon.png";
                pRaw = Bitmap::FromFile(localPng.c_str());
            }
        }
        if (pRaw && pRaw->GetLastStatus() == Ok) {
            m_pAppIconBmp = new Bitmap(144, 144, PixelFormat32bppARGB);
            Graphics g(m_pAppIconBmp);
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.DrawImage(pRaw, 0, 0, 144, 144);
            delete pRaw;
        }
    }

    DWORD dwStyle = 0;
    DWORD dwExStyle = 0;

    if (m_isChild) {
        dwStyle = WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        dwExStyle = 0;
    } else {
        dwStyle = m_settings.borderless
            ? (WS_POPUP | WS_THICKFRAME | WS_VISIBLE | WS_CLIPCHILDREN)
            : (WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN);
        dwExStyle = m_settings.pinOnTop ? WS_EX_TOPMOST : 0;
    }

    m_hwnd = CreateWindowExW(dwExStyle, L"BeagleSamplerEditorClass", L"BeagleSampler",
                             dwStyle, x, y, w, h,
                             parentHwnd, NULL, m_hInstance, this);

    if (m_hwnd) {
        SetTimer(m_hwnd, 1, 1000 / m_settings.targetFps, NULL);
    }
    return m_hwnd;
}

void WaveformEditor::destroy() {
    if (m_hwnd && IsWindow(m_hwnd)) {
        KillTimer(m_hwnd, 1);
        DestroyWindow(m_hwnd);
        m_hwnd = NULL;
    }
    if (m_baseBM) {
        SelectObject(m_baseDC, m_oldBaseBM);
        DeleteObject(m_baseBM);
        DeleteDC(m_baseDC);
        m_baseBM = NULL;
        m_baseDC = NULL;
    }
    if (m_backBM) {
        SelectObject(m_backDC, m_oldBM);
        DeleteObject(m_backBM);
        DeleteDC(m_backDC);
        m_backBM = NULL;
        m_backDC = NULL;
    }
    if (m_pAppIconBmp) {
        delete m_pAppIconBmp;
        m_pAppIconBmp = nullptr;
    }
}

void WaveformEditor::setSize(int w, int h) {
    if (m_hwnd && IsWindow(m_hwnd)) {
        SetWindowPos(m_hwnd, NULL, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

LRESULT CALLBACK WaveformEditor::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WaveformEditor* pThis = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = static_cast<WaveformEditor*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        if (pThis) pThis->m_hwnd = hwnd;
    } else {
        pThis = reinterpret_cast<WaveformEditor*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (pThis) {
        return pThis->handleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void WaveformEditor::updateBaseBackbuffer(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (m_baseDC && m_baseW == width && m_baseH == height && !m_baseNeedsUpdate) return;

    if (!m_baseDC) {
        HDC screenDC = GetDC(m_hwnd);
        m_baseDC = CreateCompatibleDC(screenDC);
        ReleaseDC(m_hwnd, screenDC);
    }
    if (m_baseBM && (m_baseW != width || m_baseH != height)) {
        SelectObject(m_baseDC, m_oldBaseBM);
        DeleteObject(m_baseBM);
        m_baseBM = NULL;
    }
    if (!m_baseBM) {
        HDC screenDC = GetDC(m_hwnd);
        m_baseBM = CreateCompatibleBitmap(screenDC, width, height);
        m_oldBaseBM = (HBITMAP)SelectObject(m_baseDC, m_baseBM);
        ReleaseDC(m_hwnd, screenDC);
        m_baseW = width;
        m_baseH = height;
    }

    Graphics g(m_baseDC);
    g.SetCompositingMode(CompositingModeSourceOver);
    g.SetCompositingQuality(CompositingQualityHighQuality);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    bool isPaused = m_buffer ? m_buffer->isPaused() : false;
    ThemeColors colors = getThemeColors(m_settings.themeId, isPaused);

    SolidBrush bgBrush(colors.bg);
    g.FillRectangle(&bgBrush, 0, 0, width, height);

    Pen gridPen(colors.gridLine, 1.0f);
    int numGridCols = 8;
    for (int i = 1; i < numGridCols; ++i) {
        float x = (float)width * ((float)i / (float)numGridCols);
        g.DrawLine(&gridPen, x, 30.0f, x, (float)height - 20.0f);
    }

    m_baseNeedsUpdate = false;
}

void WaveformEditor::onPaint(HWND hwnd, HDC hdc) {
    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    int width = clientRect.right - clientRect.left;
    int height = clientRect.bottom - clientRect.top;
    if (width <= 0 || height <= 0) return;

    if (!m_backDC) {
        m_backDC = CreateCompatibleDC(hdc);
    }
    if (m_backBM && (m_backW != width || m_backH != height)) {
        SelectObject(m_backDC, m_oldBM);
        DeleteObject(m_backBM);
        m_backBM = NULL;
    }
    if (!m_backBM) {
        m_backBM = CreateCompatibleBitmap(hdc, width, height);
        m_oldBM = (HBITMAP)SelectObject(m_backDC, m_backBM);
        m_backW = width;
        m_backH = height;
        m_baseNeedsUpdate = true;
    }

    updateBaseBackbuffer(width, height);
    BitBlt(m_backDC, 0, 0, width, height, m_baseDC, 0, 0, SRCCOPY);

    Graphics g(m_backDC);
    g.SetCompositingMode(CompositingModeSourceOver);
    g.SetCompositingQuality(CompositingQualityHighSpeed);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    bool isPaused = m_buffer ? m_buffer->isPaused() : false;
    ThemeColors colors = getThemeColors(m_settings.themeId, isPaused);

    // Dynamic layout bounds: waveform strictly below header
    float logoSize = std::clamp((float)height * 0.35f, 36.0f, 72.0f);
    float topMargin = logoSize + 6.0f;
    float bottomMargin = 22.0f;
    float waveH = (float)height - topMargin - bottomMargin;
    float centerY = std::floor(topMargin + waveH * 0.5f);
    float halfH = (waveH * 0.5f) - 2.0f;

    // Draw center baseline
    Pen centerPen(colors.centerLine, 1.0f);
    g.DrawLine(&centerPen, 0.0f, centerY, (float)width, centerY);

    // Draw Waveform Polygon
    if (m_buffer && width > 0) {
        std::vector<float> topEnv(width, 0.0f);
        std::vector<float> botEnv(width, 0.0f);

        float playheadX = static_cast<float>(width - 1);
        if (m_settings.displayMode == 0) {
            // Sweep Mode: silky-smooth sub-sample interpolation and hermite easing
            double frac = m_buffer->getInterpolatedWriteHeadFrac();
            playheadX = std::clamp(static_cast<float>(frac * width), 0.0f, static_cast<float>(width - 1));
            m_buffer->getBufferSweepEnvelope(width, m_settings.displayGain, topEnv.data(), botEnv.data(), 0, 12, frac);
        } else {
            // Rolling Mode
            m_buffer->getSmoothRollingEnvelope(width, m_settings.displayGain, topEnv.data(), botEnv.data());
        }

        std::vector<PointF> poly;
        poly.reserve(width * 2);
        for (int x = 0; x < width; ++x) {
            float rawMax = topEnv[x];
            float rawMin = botEnv[x];
            float peak = m_settings.asymmetrical ? rawMax : std::max(rawMax, -rawMin);
            float topDisp = peak * halfH;
            float y = std::round(centerY - topDisp);
            poly.push_back(PointF((float)x, y));
        }
        for (int x = width - 1; x >= 0; --x) {
            float rawMax = topEnv[x];
            float rawMin = botEnv[x];
            float peak = m_settings.asymmetrical ? 0.0f : std::max(rawMax, -rawMin);
            float botDisp = peak * halfH;
            float y = std::round(centerY + botDisp);
            poly.push_back(PointF((float)x, y));
        }

        GraphicsPath path;
        path.AddPolygon(poly.data(), static_cast<int>(poly.size()));

        bool evenPass = (m_buffer->getEpoch() % 2 == 0);

        LinearGradientBrush rainbowBrush(PointF(0.0f, 0.0f), PointF(static_cast<float>(width), 0.0f), Color::Red, Color::Blue);
        LinearGradientBrush strokeBrush(PointF(0.0f, 0.0f), PointF(static_cast<float>(width), 0.0f), Color::White, Color::White);
        LinearGradientBrush rainbowBrushOld(PointF(0.0f, 0.0f), PointF(static_cast<float>(width), 0.0f), Color::Red, Color::Blue);
        LinearGradientBrush strokeBrushOld(PointF(0.0f, 0.0f), PointF(static_cast<float>(width), 0.0f), Color::White, Color::White);

        if (m_settings.themeId == 2) {
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

        Brush* leftRainbowFill = evenPass ? static_cast<Brush*>(&rainbowBrush) : static_cast<Brush*>(&rainbowBrushOld);
        Brush* leftRainbowStroke = evenPass ? static_cast<Brush*>(&strokeBrush) : static_cast<Brush*>(&strokeBrushOld);
        Brush* rightRainbowFill = evenPass ? static_cast<Brush*>(&rainbowBrushOld) : static_cast<Brush*>(&rainbowBrush);
        Brush* rightRainbowStroke = evenPass ? static_cast<Brush*>(&strokeBrushOld) : static_cast<Brush*>(&strokeBrush);

        Color leftFillCol = evenPass ? colors.waveformFill : colors.waveformFillOld;
        Color leftStrokeCol = evenPass ? colors.waveformStroke : colors.waveformStrokeOld;
        Color rightFillCol = evenPass ? colors.waveformFillOld : colors.waveformFill;
        Color rightStrokeCol = evenPass ? colors.waveformStrokeOld : colors.waveformStroke;

        uint64_t totalWritten = m_buffer ? m_buffer->getTotalSamplesWritten() : 0;
        size_t cap = m_buffer ? m_buffer->getCapacity() : 1;
        bool hasWrapped = (cap > 0 && totalWritten >= cap);

        if (m_settings.displayMode == 0 && playheadX > 0.0f && playheadX < static_cast<float>(width)) {
            Region origClip;
            g.GetClip(&origClip);

            RectF leftClip(0.0f, 0.0f, playheadX, static_cast<float>(height));
            g.SetClip(leftClip);
            if (m_settings.themeId == 2) {
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

            if (hasWrapped) {
                RectF rightClip(playheadX, 0.0f, static_cast<float>(width) - playheadX, static_cast<float>(height));
                g.SetClip(rightClip);
                if (m_settings.themeId == 2) {
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
            bool isLeftTone = (m_settings.displayMode != 0 || playheadX >= static_cast<float>(width));
            if (m_settings.themeId == 2) {
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

        // Sweep playhead neon yellow line and soft luminous trailing glow
        if (m_settings.displayMode == 0) {
            float hx = playheadX;
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
            Pen glowPen(Color(60, 255, 240, 50), 3.0f);
            g.DrawLine(&glowPen, hx, topMargin, hx, topMargin + waveH);
            Pen playPen(Color(255, 255, 245, 60), 1.5f);
            g.DrawLine(&playPen, hx, topMargin, hx, topMargin + waveH);
        }
    }

    // Selection highlight
    if (m_hasSelection || m_isSelecting) {
        int x1 = std::min(m_selStartX, m_selEndX);
        int x2 = std::max(m_selStartX, m_selEndX);
        int selW = x2 - x1;
        if (selW > 0) {
            SolidBrush selBrush(colors.selectionFill);
            g.FillRectangle(&selBrush, (float)x1, topMargin, (float)selW, waveH);
            Pen selPen(colors.selectionBorder, 1.5f);
            g.DrawRectangle(&selPen, (float)x1, topMargin, (float)selW, waveH);
        }
    }

    // Beagle Logo Thumbnail (top left)
    if (m_pAppIconBmp) {
        g.DrawImage(m_pAppIconBmp, 10.0f, 4.0f, logoSize, logoSize);
    }

    // Header Branding & Telemetry
    FontFamily ff(L"Segoe UI");
    Gdiplus::Font titleFont(&ff, 13.0f, FontStyleBold, UnitPixel);
    Gdiplus::Font subFont(&ff, 11.0f, FontStyleRegular, UnitPixel);
    Gdiplus::Font badgeFont(&ff, 11.0f, FontStyleBold, UnitPixel);

    SolidBrush textBrush(colors.text);
    SolidBrush mutedBrush(colors.textMuted);

    float titleX = 10.0f + logoSize + 8.0f;
    g.DrawString(L"BeagleSampler", -1, &titleFont, PointF(titleX, 9.0f), &textBrush);

    // Centered Recording Status & Gain across middle line
    std::wstring recText = isPaused ? L"⏸ PAUSED" : (m_isChild ? L"● REC [DAW]" : (m_capture && m_capture->isLoopback() ? L"● REC [Desktop]" : L"● REC [Mic]"));
    Color badgeColor = isPaused ? Color(255, 255, 180, 50) : Color(255, 70, 220, 100);
    SolidBrush badgeBrush(badgeColor);

    RectF boundsRec, boundsGain;
    g.MeasureString(recText.c_str(), -1, &badgeFont, PointF(0, 0), &boundsRec);
    std::wstring gainStr = L"Gain 0 dB";
    if (m_settings.displayGain > 1.05f) {
        int db = (int)std::round(20.0f * std::log10(m_settings.displayGain));
        gainStr = L"Gain +" + std::to_wstring(db) + L" dB";
    }
    g.MeasureString(gainStr.c_str(), -1, &subFont, PointF(0, 0), &boundsGain);

    float totalCenterW = boundsRec.Width + 16.0f + boundsGain.Width;
    float startCenterX = ((float)width - totalCenterW) * 0.5f;
    g.DrawString(recText.c_str(), -1, &badgeFont, PointF(startCenterX, 9.0f), &badgeBrush);
    g.DrawString(gainStr.c_str(), -1, &subFont, PointF(startCenterX + boundsRec.Width + 16.0f, 9.0f), &mutedBrush);

    // Right side controls: Settings Button & Pause Button
    float rightBtnX = (float)width - 34.0f;
    m_rectSettingsBtn = { (int)rightBtnX, 6, (int)rightBtnX + 24, 30 };
    g.DrawString(L"⚙", -1, &titleFont, PointF(rightBtnX + 4.0f, 8.0f), &textBrush);

    rightBtnX -= 30.0f;
    m_rectPauseBtn = { (int)rightBtnX, 6, (int)rightBtnX + 24, 30 };
    g.DrawString(isPaused ? L"▶" : L"⏸", -1, &titleFont, PointF(rightBtnX + 4.0f, 8.0f), &textBrush);

    // Circular Traffic Lights (only if standalone borderless)
    if (!m_isChild && m_settings.borderless) {
        float btnY = 10.0f;
        float r = 12.0f;
        m_rectCloseBtn = { (int)width - 24, (int)btnY, (int)width - 12, (int)btnY + 12 };
        SolidBrush closeBrush(Color(255, 255, 95, 86));
        g.FillEllipse(&closeBrush, (float)width - 24.0f, btnY, r, r);
    }

    // Bottom telemetry
    std::wstring durStr = std::to_wstring((int)m_settings.duration) + L"s Buffer";
    g.DrawString(durStr.c_str(), -1, &subFont, PointF(14.0f, (float)height - 18.0f), &mutedBrush);

    BitBlt(hdc, 0, 0, width, height, m_backDC, 0, 0, SRCCOPY);
}

std::wstring WaveformEditor::exportCurrentAudio(bool selectionOnly) {
    if (!m_buffer) return L"";
    size_t cap = m_buffer->getCapacity();
    size_t readCount = cap;
    std::vector<float> left(cap);
    std::vector<float> right(cap);

    if (selectionOnly && m_hasSelection && m_backW > 0) {
        int x1 = std::min(m_selStartX, m_selEndX);
        int x2 = std::max(m_selStartX, m_selEndX);
        double s0 = (double)x1 / (double)m_backW;
        double s1 = (double)x2 / (double)m_backW;
        if (m_settings.displayMode == 0) {
            readCount = m_buffer->readSweepSelectionSamples(left.data(), right.data(), s0, s1, m_settings.trimSilence, m_settings.silenceThresholdDb);
        } else {
            readCount = m_buffer->readSelectionSamples(left.data(), right.data(), s0, s1, m_settings.trimSilence, m_settings.silenceThresholdDb);
        }
    } else {
        readCount = m_buffer->readRecentSamples(left.data(), right.data(), cap);
    }

    if (readCount == 0) return L"";

    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t filename[MAX_PATH];
    swprintf_s(filename, MAX_PATH, L"%s\\BeagleSampler_%04d%02d%02d_%02d%02d%02d.wav",
               tempDir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    int numChannels = (m_settings.channelMode == 0) ? 2 : 1;
    const float* pL = left.data();
    const float* pR = (m_settings.channelMode == 2) ? right.data() : (numChannels > 1 ? right.data() : left.data());

    if (exportWav(filename, pL, pR, readCount, (int)m_buffer->getSampleRate(), numChannels, m_settings.save24Bit)) {
        return filename;
    }
    return L"";
}

void WaveformEditor::handleSelectionDragDrop(HWND hwnd) {
    std::wstring wavPath = exportCurrentAudio(true);
    if (wavPath.empty()) return;

    ReleaseCapture();
    m_isDragging = false;
    m_isSelecting = false;

    DataObject dataObj(wavPath);
    DropSource dropSrc;
    DWORD dwEffect = 0;
    DoDragDrop(&dataObj, &dropSrc, DROPEFFECT_COPY | DROPEFFECT_MOVE, &dwEffect);
    InvalidateRect(hwnd, NULL, FALSE);
}

LRESULT WaveformEditor::handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        onPaint(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_TIMER: {
        if (wParam == 1) {
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);

        if (PtInRect(&m_rectPauseBtn, POINT{ x, y })) {
            if (m_buffer) {
                m_buffer->setPaused(!m_buffer->isPaused());
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (PtInRect(&m_rectSettingsBtn, POINT{ x, y })) {
            SendMessageW(hwnd, WM_CONTEXTMENU, (WPARAM)hwnd, MAKELPARAM(x, y));
            return 0;
        }

        m_isSelecting = true;
        m_hasSelection = false;
        m_selStartX = x;
        m_selEndX = x;
        m_mouseDownPos = { x, y };

        if (m_buffer && !m_buffer->isPaused()) {
            m_buffer->setPaused(true);
            m_wasAutoPaused = true;
        }
        SetCapture(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);

        if (m_isSelecting) {
            m_selEndX = x;
            int dx = std::abs(x - m_mouseDownPos.x);
            int dy = std::abs(y - m_mouseDownPos.y);

            if (m_hasSelection && (dx > 8 || dy > 8)) {
                // Drag and drop export
                handleSelectionDragDrop(hwnd);
                return 0;
            }
            if (dx > 4) m_hasSelection = true;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (m_isSelecting) {
            m_isSelecting = false;
            ReleaseCapture();
            if (std::abs(m_selEndX - m_selStartX) < 4) {
                m_hasSelection = false;
                if (m_wasAutoPaused && m_buffer) {
                    m_buffer->setPaused(false);
                    m_wasAutoPaused = false;
                }
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        // Right click resumes playback and clears selection
        m_hasSelection = false;
        m_isSelecting = false;
        if (m_buffer && m_buffer->isPaused()) {
            m_buffer->setPaused(false);
            m_wasAutoPaused = false;
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_KEYDOWN: {
        if (wParam == VK_SPACE) {
            if (m_buffer) {
                m_buffer->setPaused(!m_buffer->isPaused());
            }
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wParam == 'P') {
            std::wstring wav = exportCurrentAudio(m_hasSelection);
            if (!wav.empty()) {
                PlaySoundW(wav.c_str(), NULL, SND_FILENAME | SND_ASYNC);
            }
        } else if (wParam == 'T') {
            m_settings.themeId = (m_settings.themeId % 5) + 1;
            m_baseNeedsUpdate = true;
            SettingsManager::saveSettings(m_settings, hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wParam == 'G') {
            if (m_settings.displayGain <= 1.05f) m_settings.displayGain = 2.0f;
            else if (m_settings.displayGain <= 2.05f) m_settings.displayGain = 3.0f;
            else if (m_settings.displayGain <= 3.05f) m_settings.displayGain = 5.0f;
            else m_settings.displayGain = 1.0f;
            SettingsManager::saveSettings(m_settings, hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wParam == 'W') {
            m_settings.displayMode = (m_settings.displayMode == 0) ? 1 : 0;
            SettingsManager::saveSettings(m_settings, hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wParam == 'C') {
            if (m_buffer) m_buffer->clear();
            m_hasSelection = false;
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wParam == VK_ESCAPE) {
            m_hasSelection = false;
            if (m_wasAutoPaused && m_buffer) {
                m_buffer->setPaused(false);
                m_wasAutoPaused = false;
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (delta > 0) m_settings.displayGain = std::min(20.0f, m_settings.displayGain * 1.15f);
        else if (delta < 0) m_settings.displayGain = std::max(0.1f, m_settings.displayGain / 1.15f);
        SettingsManager::saveSettings(m_settings, hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_DESTROY: {
        SettingsManager::saveSettings(m_settings, hwnd);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace RollingSampler
