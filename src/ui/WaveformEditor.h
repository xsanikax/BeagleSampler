#pragma once
#include "../core/RollingAudioBuffer.h"
#include "../core/SettingsManager.h"
#include "../audio/WasapiAudioCapture.h"
#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <vector>

namespace RollingSampler {

class WaveformEditor {
public:
    WaveformEditor(RollingAudioBuffer* buffer, AudioCapture* capture = nullptr, bool isChild = false);
    ~WaveformEditor();

    HWND create(HWND parentHwnd, HINSTANCE hInstance, int x, int y, int w, int h);
    void destroy();
    HWND getHwnd() const { return m_hwnd; }
    void setSize(int w, int h);

    bool isChild() const { return m_isChild; }
    RollingAudioBuffer* getBuffer() const { return m_buffer; }
    AudioCapture* getCapture() const { return m_capture; }
    AppSettings& getSettings() { return m_settings; }

    static void registerClass(HINSTANCE hInstance);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void onPaint(HWND hwnd, HDC hdc);
    void updateBaseBackbuffer(int width, int height);
    std::wstring exportCurrentAudio(bool selectionOnly);
    void handleSelectionDragDrop(HWND hwnd);

    RollingAudioBuffer* m_buffer = nullptr;
    AudioCapture* m_capture = nullptr;
    bool m_isChild = false;
    HWND m_hwnd = NULL;
    HWND m_parentHwnd = NULL;
    HINSTANCE m_hInstance = NULL;

    AppSettings m_settings;

    // Selection & Drag state
    bool m_isSelecting = false;
    bool m_hasSelection = false;
    int m_selStartX = 0;
    int m_selEndX = 0;
    bool m_isDragging = false;
    POINT m_mouseDownPos = { 0, 0 };
    bool m_wasAutoPaused = false;

    // Status notification message
    std::wstring m_statusMessage = L"";
    DWORD m_statusMessageExpiry = 0;

    // UI Buttons
    RECT m_rectCloseBtn = { 0, 0, 0, 0 };
    RECT m_rectMinBtn = { 0, 0, 0, 0 };
    RECT m_rectPinBtn = { 0, 0, 0, 0 };
    RECT m_rectPauseBtn = { 0, 0, 0, 0 };
    RECT m_rectSettingsBtn = { 0, 0, 0, 0 };

    enum class HoverBtn { None, Close, Min, Pin, Settings, Pause };
    HoverBtn m_hoverBtn = HoverBtn::None;

    // Offscreen rendering
    HDC m_baseDC = NULL;
    HBITMAP m_baseBM = NULL;
    HBITMAP m_oldBaseBM = NULL;
    int m_baseW = 0;
    int m_baseH = 0;
    bool m_baseNeedsUpdate = true;
    bool m_inSizeMove = false;

    HDC m_backDC = NULL;
    HBITMAP m_backBM = NULL;
    HBITMAP m_oldBM = NULL;
    int m_backW = 0;
    int m_backH = 0;

    Gdiplus::Bitmap* m_pAppIconBmp = nullptr;
    static HBRUSH s_darkBgBrush;
    static bool s_classRegistered;
};

} // namespace RollingSampler
