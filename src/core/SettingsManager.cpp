#include "SettingsManager.h"
#include <shlobj.h>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cmath>

namespace RollingSampler {

std::wstring SettingsManager::getAppDataDir() {
    wchar_t appData[MAX_PATH] = { 0 };
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appData))) {
        std::wstring dir = std::wstring(appData) + L"\\BeagleSampler";
        CreateDirectoryW(dir.c_str(), NULL);
        return dir;
    }
    return L".";
}

std::wstring SettingsManager::getIniPath() {
    return getAppDataDir() + L"\\settings.ini";
}

std::wstring SettingsManager::getBbSettingsPath() {
    return getAppDataDir() + L"\\BeagleSampler.bbsettings";
}

std::wstring SettingsManager::getSessionStatePath() {
    return getAppDataDir() + L"\\session.state";
}

std::string SettingsManager::serializeToBbSettingsXml(const AppSettings& s) {
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\n";
    oss << "<PROPERTIES>\n";
    oss << "  <VALUE name=\"Selected Theme\" val=\"" << s.themeId << "\"/>\n";
    oss << "  <VALUE name=\"Standalone Display Width\" val=\"" << s.windowWidth << "\"/>\n";
    oss << "  <VALUE name=\"Standalone Display Height\" val=\"" << s.windowHeight << "\"/>\n";
    oss << "  <VALUE name=\"Target FPS\" val=\"" << s.targetFps << "\"/>\n";
    oss << "  <VALUE name=\"Display Gain\" val=\"" << s.displayGain << "\"/>\n";
    oss << "  <VALUE name=\"Duration\" val=\"" << s.duration << "\"/>\n";
    oss << "  <VALUE name=\"Trim Silence\" val=\"" << (s.trimSilence ? 1 : 0) << "\"/>\n";
    oss << "  <VALUE name=\"Noise Threshold\" val=\"" << s.silenceThresholdDb << "\"/>\n";
    oss << "  <VALUE name=\"24 bit\" val=\"" << (s.save24Bit ? 1 : 0) << "\"/>\n";
    oss << "  <VALUE name=\"Channel Mode\" val=\"" << s.channelMode << "\"/>\n";
    oss << "  <VALUE name=\"Append BPM\" val=\"" << (s.appendBpm ? 1 : 0) << "\"/>\n";
    oss << "  <VALUE name=\"Borderless Window\" val=\"" << (s.borderless ? 1 : 0) << "\"/>\n";
    oss << "  <VALUE name=\"Capture Playback\" val=\"" << (s.isLoopback ? 1 : 0) << "\"/>\n";
    oss << "  <VALUE name=\"Sweep Mode\" val=\"" << (s.displayMode == 0 ? 1 : 0) << "\"/>\n";
    oss << "  <VALUE name=\"Pin On Top\" val=\"" << (s.pinOnTop ? 1 : 0) << "\"/>\n";
    oss << "  <VALUE name=\"Save Session State\" val=\"" << (s.saveSessionState ? 1 : 0) << "\"/>\n";
    oss << "</PROPERTIES>\n";
    return oss.str();
}

bool SettingsManager::deserializeFromBbSettingsXml(const std::string& xml, AppSettings& s) {
    std::istringstream stream(xml);
    std::string line;
    bool foundProperties = false;

    while (std::getline(stream, line)) {
        if (line.find("<PROPERTIES>") != std::string::npos) {
            foundProperties = true;
            continue;
        }
        auto namePos = line.find("name=\"");
        auto valPos = line.find("val=\"");
        if (namePos != std::string::npos && valPos != std::string::npos) {
            std::string key = line.substr(namePos + 6);
            auto quoteEnd = key.find("\"");
            if (quoteEnd != std::string::npos) key = key.substr(0, quoteEnd);

            std::string val = line.substr(valPos + 5);
            quoteEnd = val.find("\"");
            if (quoteEnd != std::string::npos) val = val.substr(0, quoteEnd);

            try {
                if (key == "Selected Theme") s.themeId = std::clamp(std::stoi(val), 1, 5);
                else if (key == "Standalone Display Width") s.windowWidth = std::clamp(std::stoi(val), 400, 3840);
                else if (key == "Standalone Display Height") s.windowHeight = std::clamp(std::stoi(val), 140, 2160);
                else if (key == "Target FPS") s.targetFps = std::clamp(std::stoi(val), 15, 144);
                else if (key == "Display Gain") s.displayGain = std::clamp(std::stof(val), 0.1f, 20.0f);
                else if (key == "Duration") s.duration = std::clamp(std::stod(val), 5.0, 600.0);
                else if (key == "Trim Silence") s.trimSilence = (std::stoi(val) != 0);
                else if (key == "Noise Threshold") s.silenceThresholdDb = std::stof(val);
                else if (key == "24 bit") s.save24Bit = (std::stoi(val) != 0);
                else if (key == "Channel Mode") s.channelMode = std::clamp(std::stoi(val), 0, 2);
                else if (key == "Append BPM") s.appendBpm = (std::stoi(val) != 0);
                else if (key == "Borderless Window") s.borderless = (std::stoi(val) != 0);
                else if (key == "Capture Playback") s.isLoopback = (std::stoi(val) != 0);
                else if (key == "Sweep Mode") s.displayMode = (std::stoi(val) != 0 ? 0 : 1);
                else if (key == "Pin On Top") s.pinOnTop = (std::stoi(val) != 0);
                else if (key == "Save Session State") s.saveSessionState = (std::stoi(val) != 0);
            } catch (...) {}
        }
    }
    return foundProperties;
}

void SettingsManager::saveSettings(const AppSettings& s, HWND hwnd) {
    AppSettings out = s;
    if (hwnd && IsWindow(hwnd) && !IsIconic(hwnd)) {
        RECT rc;
        if (GetWindowRect(hwnd, &rc)) {
            out.windowX = rc.left;
            out.windowY = rc.top;
            out.windowWidth = rc.right - rc.left;
            out.windowHeight = rc.bottom - rc.top;
        }
    }

    // 1. Save INI
    std::wstring iniPath = getIniPath();
    WritePrivateProfileStringW(L"Audio", L"DeviceId", out.audioDeviceId.c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"Audio", L"IsLoopback", out.isLoopback ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"Audio", L"Duration", std::to_wstring(out.duration).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"Audio", L"Save24Bit", out.save24Bit ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"Audio", L"ChannelMode", std::to_wstring(out.channelMode).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"Audio", L"TrimSilence", out.trimSilence ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"Audio", L"SilenceThresholdDb", std::to_wstring(out.silenceThresholdDb).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"Audio", L"AppendBpm", out.appendBpm ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"Audio", L"SaveSessionState", out.saveSessionState ? L"1" : L"0", iniPath.c_str());

    WritePrivateProfileStringW(L"Display", L"Theme", std::to_wstring(out.themeId).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"Display", L"DisplayGain", std::to_wstring(out.displayGain).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"Display", L"DisplayMode", std::to_wstring(out.displayMode).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"Display", L"Asymmetrical", out.asymmetrical ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"Display", L"PinOnTop", out.pinOnTop ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"Display", L"TargetFps", std::to_wstring(out.targetFps).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"Display", L"Borderless", out.borderless ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"App", L"AutoCheckUpdates", out.autoCheckUpdates ? L"1" : L"0", iniPath.c_str());

    if (out.windowX != -99999) {
        WritePrivateProfileStringW(L"Window", L"X", std::to_wstring(out.windowX).c_str(), iniPath.c_str());
        WritePrivateProfileStringW(L"Window", L"Y", std::to_wstring(out.windowY).c_str(), iniPath.c_str());
        WritePrivateProfileStringW(L"Window", L"Width", std::to_wstring(out.windowWidth).c_str(), iniPath.c_str());
        WritePrivateProfileStringW(L"Window", L"Height", std::to_wstring(out.windowHeight).c_str(), iniPath.c_str());
    }

    // 2. Save .bbsettings XML
    std::wstring bbPath = getBbSettingsPath();
    std::string xml = serializeToBbSettingsXml(out);
    std::ofstream ofs(bbPath.c_str(), std::ios::binary);
    if (ofs.is_open()) {
        ofs.write(xml.data(), xml.size());
        ofs.close();
    }
}

AppSettings SettingsManager::loadSettings() {
    AppSettings s;
    std::wstring iniPath = getIniPath();

    if (GetFileAttributesW(iniPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        // Load from INI
        wchar_t devBuf[512] = { 0 };
        GetPrivateProfileStringW(L"Audio", L"DeviceId", L"default_loopback", devBuf, 512, iniPath.c_str());
        s.audioDeviceId = devBuf;
        s.isLoopback = (GetPrivateProfileIntW(L"Audio", L"IsLoopback", 1, iniPath.c_str()) != 0);

        wchar_t durBuf[64] = { 0 };
        GetPrivateProfileStringW(L"Audio", L"Duration", L"30.0", durBuf, 64, iniPath.c_str());
        try { s.duration = std::stod(durBuf); } catch (...) { s.duration = 30.0; }

        s.save24Bit = (GetPrivateProfileIntW(L"Audio", L"Save24Bit", 0, iniPath.c_str()) != 0);
        s.channelMode = GetPrivateProfileIntW(L"Audio", L"ChannelMode", 0, iniPath.c_str());
        s.trimSilence = (GetPrivateProfileIntW(L"Audio", L"TrimSilence", 1, iniPath.c_str()) != 0);

        wchar_t threshBuf[64] = { 0 };
        GetPrivateProfileStringW(L"Audio", L"SilenceThresholdDb", L"-90.0", threshBuf, 64, iniPath.c_str());
        try { s.silenceThresholdDb = std::stof(threshBuf); } catch (...) { s.silenceThresholdDb = -90.0f; }

        s.appendBpm = (GetPrivateProfileIntW(L"Audio", L"AppendBpm", 0, iniPath.c_str()) != 0);
        s.saveSessionState = (GetPrivateProfileIntW(L"Audio", L"SaveSessionState", 1, iniPath.c_str()) != 0);

        s.themeId = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Display", L"Theme", 1, iniPath.c_str())), 1, 5);

        wchar_t gainBuf[64] = { 0 };
        GetPrivateProfileStringW(L"Display", L"DisplayGain", L"1.0", gainBuf, 64, iniPath.c_str());
        try { s.displayGain = std::stof(gainBuf); } catch (...) { s.displayGain = 1.0f; }

        s.displayMode = GetPrivateProfileIntW(L"Display", L"DisplayMode", 0, iniPath.c_str());
        s.asymmetrical = (GetPrivateProfileIntW(L"Display", L"Asymmetrical", 0, iniPath.c_str()) != 0);
        s.pinOnTop = (GetPrivateProfileIntW(L"Display", L"PinOnTop", 0, iniPath.c_str()) != 0);
        s.targetFps = GetPrivateProfileIntW(L"Display", L"TargetFps", 60, iniPath.c_str());
        s.borderless = (GetPrivateProfileIntW(L"Display", L"Borderless", 1, iniPath.c_str()) != 0);
        s.autoCheckUpdates = (GetPrivateProfileIntW(L"App", L"AutoCheckUpdates", 1, iniPath.c_str()) != 0);

        int savedX = GetPrivateProfileIntW(L"Window", L"X", -99999, iniPath.c_str());
        int savedY = GetPrivateProfileIntW(L"Window", L"Y", -99999, iniPath.c_str());
        int savedW = GetPrivateProfileIntW(L"Window", L"Width", 1223, iniPath.c_str());
        int savedH = GetPrivateProfileIntW(L"Window", L"Height", 293, iniPath.c_str());
        if (savedX != -99999 && savedY != -99999 && savedW >= 400 && savedH >= 140) {
            s.windowX = savedX;
            s.windowY = savedY;
            s.windowWidth = savedW;
            s.windowHeight = savedH;
        }
        return s;
    }

    // Fallback: check BeagleSampler.bbsettings
    std::wstring bbPath = getBbSettingsPath();

    if (GetFileAttributesW(bbPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::ifstream ifs(bbPath.c_str(), std::ios::binary);
        if (ifs.is_open()) {
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            deserializeFromBbSettingsXml(content, s);
        }
    }

    return s;
}

} // namespace RollingSampler
