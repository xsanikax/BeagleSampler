#pragma once
#include <string>
#include <windows.h>

namespace RollingSampler {

struct AppSettings {
    // Audio
    std::wstring audioDeviceId = L"default_loopback";
    bool isLoopback = true;
    double duration = 30.0;
    bool save24Bit = false;
    int channelMode = 0; // 0=Stereo, 1=MonoLeft, 2=MonoRight
    bool trimSilence = true;
    float silenceThresholdDb = -90.0f;
    bool appendBpm = false;
    bool saveSessionState = true;

    // Display
    int themeId = 1; // 1=Carbon (Default), 2=Rainbow, 3=Bright, 4=Lemon, 5=Vintage
    float displayGain = 1.0f; // 1.0 = 0 dB
    int displayMode = 0; // 0=Sweep, 1=Rolling
    bool asymmetrical = false;
    bool pinOnTop = false;
    int targetFps = 60;
    bool borderless = true;
    bool autoCheckUpdates = true;

    // Window
    int windowX = -99999;
    int windowY = -99999;
    int windowWidth = 1223;
    int windowHeight = 293;
};

class SettingsManager {
public:
    static std::wstring getAppDataDir();
    static std::wstring getIniPath();
    static std::wstring getBbSettingsPath();
    static std::wstring getSessionStatePath();

    static void saveSettings(const AppSettings& settings, HWND hwnd = NULL);
    static AppSettings loadSettings();

    // XML helper for .bbsettings format
    static std::string serializeToBbSettingsXml(const AppSettings& settings);
    static bool deserializeFromBbSettingsXml(const std::string& xml, AppSettings& settings);
};

} // namespace RollingSampler
