#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

using namespace Gdiplus;

#define IDI_APP_ICON    1
#define IDR_BEAGLE_LOGO 2
#define IDR_STANDALONE  10
#define IDR_VST3        11
#define IDR_CLAP        12

#define IDC_CHK_STANDALONE 101
#define IDC_CHK_VST3       102
#define IDC_CHK_CLAP       103
#define IDC_CHK_DESKTOP    104
#define IDC_CHK_STARTMENU  105
#define IDC_CHK_LAUNCH     106
#define IDC_BTN_INSTALL    107
#define IDC_BTN_CANCEL     108
#define IDC_PROGRESS       109
#define IDC_STATUS_LABEL   110

#define IDC_LBL_STANDALONE 111
#define IDC_LBL_VST3       112
#define IDC_LBL_CLAP       113
#define IDC_LBL_DESKTOP    114
#define IDC_LBL_STARTMENU  115
#define IDC_LBL_LAUNCH     116
#define IDC_BTN_UNINSTALL  117

static HWND g_hChkStandalone = NULL;
static HWND g_hChkVst3 = NULL;
static HWND g_hChkClap = NULL;
static HWND g_hChkDesktop = NULL;
static HWND g_hChkStartMenu = NULL;
static HWND g_hChkLaunch = NULL;

static HWND g_hLblStandalone = NULL;
static HWND g_hLblVst3 = NULL;
static HWND g_hLblClap = NULL;
static HWND g_hLblDesktop = NULL;
static HWND g_hLblStartMenu = NULL;
static HWND g_hLblLaunch = NULL;

static HWND g_hBtnInstall = NULL;
static HWND g_hBtnUninstall = NULL;
static HWND g_hBtnCancel = NULL;
static HWND g_hProgress = NULL;
static HWND g_hStatusLabel = NULL;
static Bitmap* g_pLogoBmp = nullptr;
static HBRUSH g_hDarkBgBrush = NULL;

static bool g_installFinished = false;
static bool g_isAlreadyInstalled = false;
static bool g_wasUninstalled = false;
static bool g_isTempRunner = false;
static std::wstring g_passedTargetDir;

static void UpdateLayout(HWND hwnd, int cx, int cy);

static bool EnsureDirectoryExists(const std::wstring& path) {
    if (path.empty()) return false;
    wchar_t folder[MAX_PATH];
    wcsncpy_s(folder, path.c_str(), _TRUNCATE);
    PathRemoveFileSpecW(folder);
    if (GetFileAttributesW(folder) != INVALID_FILE_ATTRIBUTES) return true;
    int res = SHCreateDirectoryExW(NULL, folder, NULL);
    return (res == ERROR_SUCCESS || res == ERROR_FILE_EXISTS || res == ERROR_ALREADY_EXISTS);
}

static bool ExtractResourceToFile(HMODULE hMod, int resId, const std::wstring& outPath) {
    HRSRC hRes = FindResourceW(hMod, MAKEINTRESOURCEW(resId), MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
    if (!hRes) return false;
    HGLOBAL hGlobal = LoadResource(hMod, hRes);
    if (!hGlobal) return false;
    DWORD resSize = SizeofResource(hMod, hRes);
    void* pResData = LockResource(hGlobal);
    if (!pResData || resSize == 0) return false;

    if (!EnsureDirectoryExists(outPath)) return false;

    HANDLE hFile = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD bytesWritten = 0;
    BOOL bOk = WriteFile(hFile, pResData, resSize, &bytesWritten, NULL);
    CloseHandle(hFile);
    return (bOk && bytesWritten == resSize);
}

static bool CreateShortcut(const std::wstring& targetPath, const std::wstring& shortcutPath, const std::wstring& desc, const std::wstring& iconPath) {
    EnsureDirectoryExists(shortcutPath);
    IShellLinkW* psl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&psl);
    if (FAILED(hr)) return false;

    psl->SetPath(targetPath.c_str());
    psl->SetDescription(desc.c_str());
    if (!iconPath.empty()) {
        psl->SetIconLocation(iconPath.c_str(), 0);
    }
    wchar_t workingDir[MAX_PATH];
    wcsncpy_s(workingDir, targetPath.c_str(), _TRUNCATE);
    PathRemoveFileSpecW(workingDir);
    psl->SetWorkingDirectory(workingDir);

    IPersistFile* ppf = nullptr;
    hr = psl->QueryInterface(IID_IPersistFile, (void**)&ppf);
    if (SUCCEEDED(hr)) {
        hr = ppf->Save(shortcutPath.c_str(), TRUE);
        ppf->Release();
    }
    psl->Release();
    return SUCCEEDED(hr);
}

static void RegisterUninstaller(const std::wstring& installDir, const std::wstring& uninstallerPath) {
    HKEY hKey = NULL;
    const wchar_t* subKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\BeagleSampler";
    LONG lRes = RegCreateKeyExW(HKEY_LOCAL_MACHINE, subKey, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    if (lRes != ERROR_SUCCESS) {
        lRes = RegCreateKeyExW(HKEY_CURRENT_USER, subKey, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    }
    if (lRes == ERROR_SUCCESS && hKey) {
        const wchar_t* name = L"BeagleSampler";
        const wchar_t* ver = L"1.0.15";
        const wchar_t* pub = L"BeagleBoss";
        std::wstring icon = installDir + L"\\BeagleSampler.exe,0";
        DWORD dword1 = 1;

        RegSetValueExW(hKey, L"DisplayName", 0, REG_SZ, (const BYTE*)name, (DWORD)(wcslen(name) + 1) * sizeof(wchar_t));
        RegSetValueExW(hKey, L"DisplayVersion", 0, REG_SZ, (const BYTE*)ver, (DWORD)(wcslen(ver) + 1) * sizeof(wchar_t));
        RegSetValueExW(hKey, L"Publisher", 0, REG_SZ, (const BYTE*)pub, (DWORD)(wcslen(pub) + 1) * sizeof(wchar_t));
        RegSetValueExW(hKey, L"DisplayIcon", 0, REG_SZ, (const BYTE*)icon.c_str(), (DWORD)(icon.length() + 1) * sizeof(wchar_t));
        RegSetValueExW(hKey, L"InstallLocation", 0, REG_SZ, (const BYTE*)installDir.c_str(), (DWORD)(installDir.length() + 1) * sizeof(wchar_t));
        
        std::wstring uninstCmd = L"\"" + uninstallerPath + L"\" --uninstall";
        RegSetValueExW(hKey, L"UninstallString", 0, REG_SZ, (const BYTE*)uninstCmd.c_str(), (DWORD)(uninstCmd.length() + 1) * sizeof(wchar_t));
        RegSetValueExW(hKey, L"NoModify", 0, REG_DWORD, (const BYTE*)&dword1, sizeof(DWORD));
        RegSetValueExW(hKey, L"NoRepair", 0, REG_DWORD, (const BYTE*)&dword1, sizeof(DWORD));
        RegCloseKey(hKey);
    }
}

static bool IsAlreadyInstalled() {
    wchar_t progFiles[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, 0, progFiles) == S_OK) {
        std::wstring p = std::wstring(progFiles) + L"\\BeagleSampler\\BeagleSampler.exe";
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    }
    wchar_t localApp[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localApp) == S_OK) {
        std::wstring p = std::wstring(localApp) + L"\\Programs\\BeagleSampler\\BeagleSampler.exe";
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    }
    wchar_t commonFiles[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES_COMMON, NULL, 0, commonFiles) == S_OK) {
        std::wstring v = std::wstring(commonFiles) + L"\\VST3\\BeagleSampler.vst3";
        if (GetFileAttributesW(v.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    }
    return false;
}

static void DeleteDirectoryCompletely(const std::wstring& dir) {
    if (dir.empty() || GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    // 1. Recursive manual traversal to remove attributes and files
    std::wstring searchPattern = dir + L"\\*.*";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
                std::wstring itemPath = dir + L"\\" + fd.cFileName;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    DeleteDirectoryCompletely(itemPath);
                } else {
                    SetFileAttributesW(itemPath.c_str(), FILE_ATTRIBUTE_NORMAL);
                    DeleteFileW(itemPath.c_str());
                }
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    // 2. Shell delete
    wchar_t doubleNull[MAX_PATH + 2] = { 0 };
    wcsncpy_s(doubleNull, dir.c_str(), MAX_PATH);
    SHFILEOPSTRUCTW op = { 0 };
    op.wFunc = FO_DELETE;
    op.pFrom = doubleNull;
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    SHFileOperationW(&op);

    // 3. Remove direct directory
    SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
    RemoveDirectoryW(dir.c_str());
}

static void ScheduleDelayedSelfDelete() {
    wchar_t currentExe[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, currentExe, MAX_PATH);

    wchar_t progFiles[MAX_PATH] = { 0 };
    SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, 0, progFiles);
    std::wstring pfDir = std::wstring(progFiles) + L"\\BeagleSampler";

    wchar_t localApp[MAX_PATH] = { 0 };
    SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localApp);
    std::wstring laDir = std::wstring(localApp) + L"\\Programs\\BeagleSampler";

    // Delayed cmd ping to ensure all handles released, delete self & wipe install folder
    std::wstring cmd = L"/C ping 127.0.0.1 -n 2 > nul";

    // ONLY delete currentExe if it is the temp uninstaller runner or named uninstall.exe!
    // NEVER delete the user's installer (e.g. BeagleSampler_Setup.exe in Downloads or dist)!
    if (g_isTempRunner || _wcsicmp(PathFindFileNameW(currentExe), L"uninstall.exe") == 0) {
        cmd += L" & del /F /Q \"" + std::wstring(currentExe) + L"\"";
    }

    cmd += L" & rd /S /Q \"" + pfDir + L"\"";
    cmd += L" & rd /S /Q \"" + laDir + L"\"";
    if (!g_passedTargetDir.empty()) {
        cmd += L" & rd /S /Q \"" + g_passedTargetDir + L"\"";
    }

    ShellExecuteW(NULL, L"open", L"cmd.exe", cmd.c_str(), NULL, SW_HIDE);
}

static void CleanRemoveAllFiles() {
    // 1. Terminate running instances
    system("taskkill /F /IM BeagleSampler.exe >nul 2>&1");
    Sleep(150);

    // 2. Remove VST3 and CLAP plugins
    wchar_t commonFiles[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES_COMMON, NULL, 0, commonFiles) == S_OK) {
        std::wstring vst3File = std::wstring(commonFiles) + L"\\VST3\\BeagleSampler.vst3";
        std::wstring clapFile = std::wstring(commonFiles) + L"\\CLAP\\BeagleSampler.clap";
        DeleteFileW(vst3File.c_str());
        DeleteFileW(clapFile.c_str());
    }
    wchar_t localApp[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localApp) == S_OK) {
        std::wstring vst3User = std::wstring(localApp) + L"\\Programs\\Common\\VST3\\BeagleSampler.vst3";
        std::wstring clapUser = std::wstring(localApp) + L"\\Programs\\Common\\CLAP\\BeagleSampler.clap";
        DeleteFileW(vst3User.c_str());
        DeleteFileW(clapUser.c_str());
    }

    // 3. Remove Shortcuts
    wchar_t desktop[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_COMMON_DESKTOPDIRECTORY, NULL, 0, desktop) == S_OK) {
        DeleteFileW((std::wstring(desktop) + L"\\BeagleSampler.lnk").c_str());
    }
    if (SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desktop) == S_OK) {
        DeleteFileW((std::wstring(desktop) + L"\\BeagleSampler.lnk").c_str());
    }

    wchar_t programs[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_COMMON_PROGRAMS, NULL, 0, programs) == S_OK) {
        std::wstring progDir = std::wstring(programs) + L"\\BeagleSampler";
        DeleteFileW((progDir + L"\\BeagleSampler.lnk").c_str());
        RemoveDirectoryW(progDir.c_str());
    }
    if (SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, 0, programs) == S_OK) {
        std::wstring progDir = std::wstring(programs) + L"\\BeagleSampler";
        DeleteFileW((progDir + L"\\BeagleSampler.lnk").c_str());
        RemoveDirectoryW(progDir.c_str());
    }

    // 4. Remove Registry keys
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\BeagleSampler");
    RegDeleteKeyW(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\BeagleSampler");

    // 5. Completely remove Program Files & Local AppData BeagleSampler installation directories
    wchar_t progFiles[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, 0, progFiles) == S_OK) {
        std::wstring pfDir = std::wstring(progFiles) + L"\\BeagleSampler";
        DeleteDirectoryCompletely(pfDir);
    }
    if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localApp) == S_OK) {
        std::wstring laDir = std::wstring(localApp) + L"\\Programs\\BeagleSampler";
        DeleteDirectoryCompletely(laDir);
    }
    if (!g_passedTargetDir.empty()) {
        DeleteDirectoryCompletely(g_passedTargetDir);
    }

    g_wasUninstalled = true;
}

static void PerformUninstallStandalone(HWND hwnd) {
    int ans = MessageBoxW(hwnd, L"Are you sure you want to completely uninstall BeagleSampler and all its DAW plugins?",
                          L"Uninstall BeagleSampler", MB_YESNO | MB_ICONQUESTION);
    if (ans != IDYES) return;

    CleanRemoveAllFiles();

    MessageBoxW(hwnd, L"BeagleSampler and all associated plugins have been completely removed from your computer.",
                L"Uninstall Complete", MB_OK | MB_ICONINFORMATION);

    ScheduleDelayedSelfDelete();
    ExitProcess(0);
}

static void PerformUninstallInline(HWND hwnd) {
    int ans = MessageBoxW(hwnd, L"Are you sure you want to completely uninstall BeagleSampler and all its DAW plugins?",
                          L"Uninstall BeagleSampler", MB_YESNO | MB_ICONQUESTION);
    if (ans != IDYES) return;

    EnableWindow(g_hBtnInstall, FALSE);
    if (g_hBtnUninstall) EnableWindow(g_hBtnUninstall, FALSE);
    EnableWindow(g_hBtnCancel, FALSE);

    SendMessageW(g_hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessageW(g_hProgress, PBM_SETPOS, 40, 0);
    SetWindowTextW(g_hStatusLabel, L"Removing plugins, shortcuts, and application files...");

    CleanRemoveAllFiles();

    SendMessageW(g_hProgress, PBM_SETPOS, 100, 0);
    SetWindowTextW(g_hStatusLabel, L"Uninstallation completed! BeagleSampler and plugins have been completely removed.");
    
    // Hide checkboxes and uninstall button, show Close button
    ShowWindow(g_hChkStandalone, SW_HIDE); ShowWindow(g_hLblStandalone, SW_HIDE);
    ShowWindow(g_hChkVst3, SW_HIDE); ShowWindow(g_hLblVst3, SW_HIDE);
    ShowWindow(g_hChkClap, SW_HIDE); ShowWindow(g_hLblClap, SW_HIDE);
    ShowWindow(g_hChkDesktop, SW_HIDE); ShowWindow(g_hLblDesktop, SW_HIDE);
    ShowWindow(g_hChkStartMenu, SW_HIDE); ShowWindow(g_hLblStartMenu, SW_HIDE);
    ShowWindow(g_hChkLaunch, SW_HIDE); ShowWindow(g_hLblLaunch, SW_HIDE);

    if (g_hBtnUninstall) ShowWindow(g_hBtnUninstall, SW_HIDE);
    ShowWindow(g_hBtnInstall, SW_HIDE);

    SetWindowTextW(g_hBtnCancel, L"Close");
    EnableWindow(g_hBtnCancel, TRUE);
    ShowWindow(g_hBtnCancel, SW_SHOW);
    g_installFinished = true;

    RECT rc;
    GetClientRect(hwnd, &rc);
    UpdateLayout(hwnd, rc.right - rc.left, rc.bottom - rc.top);
}

static void DoInstall(HWND hwnd) {
    EnableWindow(g_hBtnInstall, FALSE);
    if (g_hBtnUninstall) EnableWindow(g_hBtnUninstall, FALSE);
    EnableWindow(g_hBtnCancel, FALSE);
    SendMessageW(g_hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessageW(g_hProgress, PBM_SETPOS, 10, 0);

    bool optStandalone = (SendMessageW(g_hChkStandalone, BM_GETCHECK, 0, 0) == BST_CHECKED);
    bool optVst3 = (SendMessageW(g_hChkVst3, BM_GETCHECK, 0, 0) == BST_CHECKED);
    bool optClap = (SendMessageW(g_hChkClap, BM_GETCHECK, 0, 0) == BST_CHECKED);
    bool optDesktop = (SendMessageW(g_hChkDesktop, BM_GETCHECK, 0, 0) == BST_CHECKED);
    bool optStartMenu = (SendMessageW(g_hChkStartMenu, BM_GETCHECK, 0, 0) == BST_CHECKED);
    bool optLaunch = (SendMessageW(g_hChkLaunch, BM_GETCHECK, 0, 0) == BST_CHECKED);

    HMODULE hMod = GetModuleHandleW(NULL);
    wchar_t progFiles[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, 0, progFiles);
    wchar_t commonFiles[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES_COMMON, NULL, 0, commonFiles);
    wchar_t localApp[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localApp);

    // Test write permission on Program Files
    bool canWriteProgFiles = true;
    std::wstring testPath = std::wstring(progFiles) + L"\\test_perm.tmp";
    HANDLE hTest = CreateFileW(testPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hTest == INVALID_HANDLE_VALUE) {
        canWriteProgFiles = false;
    } else {
        CloseHandle(hTest);
        DeleteFileW(testPath.c_str());
    }

    std::wstring appDir = canWriteProgFiles ? (std::wstring(progFiles) + L"\\BeagleSampler")
                                            : (std::wstring(localApp) + L"\\Programs\\BeagleSampler");
    std::wstring appExe = appDir + L"\\BeagleSampler.exe";
    std::wstring uninstallerExe = appDir + L"\\uninstall.exe";

    std::wstring vst3File = canWriteProgFiles ? (std::wstring(commonFiles) + L"\\VST3\\BeagleSampler.vst3")
                                              : (std::wstring(localApp) + L"\\Programs\\Common\\VST3\\BeagleSampler.vst3");
    std::wstring clapFile = canWriteProgFiles ? (std::wstring(commonFiles) + L"\\CLAP\\BeagleSampler.clap")
                                              : (std::wstring(localApp) + L"\\Programs\\Common\\CLAP\\BeagleSampler.clap");

    SetWindowTextW(g_hStatusLabel, L"Extracting components...");

    // 1. Standalone
    if (optStandalone) {
        SetWindowTextW(g_hStatusLabel, L"Installing BeagleSampler Standalone App...");
        ExtractResourceToFile(hMod, IDR_STANDALONE, appExe);
        SendMessageW(g_hProgress, PBM_SETPOS, 35, 0);

        // Copy current installer as uninstaller
        wchar_t currentExe[MAX_PATH];
        GetModuleFileNameW(NULL, currentExe, MAX_PATH);
        CopyFileW(currentExe, uninstallerExe.c_str(), FALSE);
        RegisterUninstaller(appDir, uninstallerExe);
    }

    // 2. VST3
    if (optVst3) {
        SetWindowTextW(g_hStatusLabel, L"Installing VST3 Plugin to VST3 directory...");
        ExtractResourceToFile(hMod, IDR_VST3, vst3File);
        SendMessageW(g_hProgress, PBM_SETPOS, 60, 0);
    }

    // 3. CLAP
    if (optClap) {
        SetWindowTextW(g_hStatusLabel, L"Installing CLAP Plugin to CLAP directory...");
        ExtractResourceToFile(hMod, IDR_CLAP, clapFile);
        SendMessageW(g_hProgress, PBM_SETPOS, 80, 0);
    }

    // 4. Shortcuts
    if (optStandalone) {
        if (optDesktop) {
            wchar_t desktop[MAX_PATH];
            int csidl = canWriteProgFiles ? CSIDL_COMMON_DESKTOPDIRECTORY : CSIDL_DESKTOPDIRECTORY;
            if (SHGetFolderPathW(NULL, csidl, NULL, 0, desktop) == S_OK) {
                std::wstring deskLnk = std::wstring(desktop) + L"\\BeagleSampler.lnk";
                CreateShortcut(appExe, deskLnk, L"BeagleSampler Audio Sampler", appExe);
            }
        }
        if (optStartMenu) {
            wchar_t programs[MAX_PATH];
            int csidl = canWriteProgFiles ? CSIDL_COMMON_PROGRAMS : CSIDL_PROGRAMS;
            if (SHGetFolderPathW(NULL, csidl, NULL, 0, programs) == S_OK) {
                std::wstring progDir = std::wstring(programs) + L"\\BeagleSampler";
                std::wstring progLnk = progDir + L"\\BeagleSampler.lnk";
                CreateShortcut(appExe, progLnk, L"BeagleSampler Audio Sampler", appExe);
            }
        }
    }

    SendMessageW(g_hProgress, PBM_SETPOS, 100, 0);
    SetWindowTextW(g_hStatusLabel, L"Installation completed successfully! BeagleSampler is ready.");
    SetWindowTextW(g_hBtnInstall, L"Finish");
    EnableWindow(g_hBtnInstall, TRUE);
    if (g_hBtnUninstall) ShowWindow(g_hBtnUninstall, SW_HIDE);
    ShowWindow(g_hBtnCancel, SW_HIDE);
    g_installFinished = true;

    RECT rc;
    GetClientRect(hwnd, &rc);
    UpdateLayout(hwnd, rc.right - rc.left, rc.bottom - rc.top);

    if (optLaunch && optStandalone) {
        ShellExecuteW(NULL, L"open", appExe.c_str(), NULL, NULL, SW_SHOWNORMAL);
    }
}

static void DoInstallSilent(const std::wstring& fromDir) {
    HMODULE hMod = GetModuleHandleW(NULL);
    wchar_t progFiles[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, 0, progFiles);
    wchar_t commonFiles[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES_COMMON, NULL, 0, commonFiles);
    wchar_t localApp[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localApp);

    bool canWriteProgFiles = true;
    std::wstring testPath = std::wstring(progFiles) + L"\\test_perm.tmp";
    HANDLE hTest = CreateFileW(testPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hTest == INVALID_HANDLE_VALUE) {
        canWriteProgFiles = false;
    } else {
        CloseHandle(hTest);
        DeleteFileW(testPath.c_str());
    }

    std::wstring appDir = canWriteProgFiles ? (std::wstring(progFiles) + L"\\BeagleSampler")
                                            : (std::wstring(localApp) + L"\\Programs\\BeagleSampler");
    std::wstring appExe = appDir + L"\\BeagleSampler.exe";
    std::wstring uninstallerExe = appDir + L"\\uninstall.exe";

    std::wstring vst3File = canWriteProgFiles ? (std::wstring(commonFiles) + L"\\VST3\\BeagleSampler.vst3")
                                              : (std::wstring(localApp) + L"\\Programs\\Common\\VST3\\BeagleSampler.vst3");
    std::wstring clapFile = canWriteProgFiles ? (std::wstring(commonFiles) + L"\\CLAP\\BeagleSampler.clap")
                                              : (std::wstring(localApp) + L"\\Programs\\Common\\CLAP\\BeagleSampler.clap");

    // Wait a brief moment for previous process to fully terminate
    Sleep(600);

    // 1. Standalone in appDir
    ExtractResourceToFile(hMod, IDR_STANDALONE, appExe);
    wchar_t currentExe[MAX_PATH];
    GetModuleFileNameW(NULL, currentExe, MAX_PATH);
    CopyFileW(currentExe, uninstallerExe.c_str(), FALSE);
    RegisterUninstaller(appDir, uninstallerExe);

    // 2. Also update in fromDir if provided (e.g. portable / dist directory)
    if (!fromDir.empty()) {
        std::wstring fromExe = fromDir + L"\\BeagleSampler.exe";
        ExtractResourceToFile(hMod, IDR_STANDALONE, fromExe);
    }

    // 3. VST3 & CLAP
    ExtractResourceToFile(hMod, IDR_VST3, vst3File);
    ExtractResourceToFile(hMod, IDR_CLAP, clapFile);

    // 4. Desktop & Start Menu Shortcuts
    wchar_t desktop[MAX_PATH];
    int csidl = canWriteProgFiles ? CSIDL_COMMON_DESKTOPDIRECTORY : CSIDL_DESKTOPDIRECTORY;
    if (SHGetFolderPathW(NULL, csidl, NULL, 0, desktop) == S_OK) {
        std::wstring deskLnk = std::wstring(desktop) + L"\\BeagleSampler.lnk";
        CreateShortcut(appExe, deskLnk, L"BeagleSampler Audio Sampler", appExe);
    }
    wchar_t programs[MAX_PATH];
    int csidlProg = canWriteProgFiles ? CSIDL_COMMON_PROGRAMS : CSIDL_PROGRAMS;
    if (SHGetFolderPathW(NULL, csidlProg, NULL, 0, programs) == S_OK) {
        std::wstring progDir = std::wstring(programs) + L"\\BeagleSampler";
        std::wstring progLnk = progDir + L"\\BeagleSampler.lnk";
        CreateShortcut(appExe, progLnk, L"BeagleSampler Audio Sampler", appExe);
    }

    // 5. Automatically launch the updated BeagleSampler!
    std::wstring launchTarget = (!fromDir.empty() && GetFileAttributesW((fromDir + L"\\BeagleSampler.exe").c_str()) != INVALID_FILE_ATTRIBUTES)
        ? (fromDir + L"\\BeagleSampler.exe")
        : appExe;
    ShellExecuteW(NULL, L"open", launchTarget.c_str(), NULL, NULL, SW_SHOWNORMAL);
}

static void UpdateLayout(HWND hwnd, int cx, int cy) {
    if (cx <= 0 || cy <= 0) return;

    int startY = 96;
    int gapY = 28;

    // Checkboxes and labels
    if (g_hChkStandalone) SetWindowPos(g_hChkStandalone, NULL, 30, startY, 20, 20, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_hLblStandalone) SetWindowPos(g_hLblStandalone, NULL, 56, startY - 1, cx - 80, 22, SWP_NOZORDER | SWP_NOACTIVATE);

    if (g_hChkVst3) SetWindowPos(g_hChkVst3, NULL, 30, startY + gapY, 20, 20, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_hLblVst3) SetWindowPos(g_hLblVst3, NULL, 56, startY + gapY - 1, cx - 80, 22, SWP_NOZORDER | SWP_NOACTIVATE);

    if (g_hChkClap) SetWindowPos(g_hChkClap, NULL, 30, startY + gapY * 2, 20, 20, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_hLblClap) SetWindowPos(g_hLblClap, NULL, 56, startY + gapY * 2 - 1, cx - 80, 22, SWP_NOZORDER | SWP_NOACTIVATE);

    int col1W = (cx - 100) / 2;
    int col2X = 56 + col1W + 16;
    if (g_hChkDesktop) SetWindowPos(g_hChkDesktop, NULL, 30, startY + gapY * 3 + 8, 20, 20, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_hLblDesktop) SetWindowPos(g_hLblDesktop, NULL, 56, startY + gapY * 3 + 7, col1W, 22, SWP_NOZORDER | SWP_NOACTIVATE);

    if (g_hChkStartMenu) SetWindowPos(g_hChkStartMenu, NULL, col2X, startY + gapY * 3 + 8, 20, 20, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_hLblStartMenu) SetWindowPos(g_hLblStartMenu, NULL, col2X + 26, startY + gapY * 3 + 7, cx - col2X - 40, 22, SWP_NOZORDER | SWP_NOACTIVATE);

    if (g_hChkLaunch) SetWindowPos(g_hChkLaunch, NULL, 30, startY + gapY * 4 + 14, 20, 20, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_hLblLaunch) SetWindowPos(g_hLblLaunch, NULL, 56, startY + gapY * 4 + 13, cx - 80, 22, SWP_NOZORDER | SWP_NOACTIVATE);

    // Responsive bottom dock controls anchored to cy:
    int btnH = 34;
    int btnY = cy - btnH - 18;
    int statusH = 20;
    int statusY = btnY - statusH - 10;
    int progH = 16;
    int progY = statusY - progH - 8;

    if (g_hProgress) SetWindowPos(g_hProgress, NULL, 30, progY, cx - 60, progH, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_hStatusLabel) SetWindowPos(g_hStatusLabel, NULL, 30, statusY, cx - 60, statusH, SWP_NOZORDER | SWP_NOACTIVATE);

    if (g_installFinished) {
        int doneW = 100;
        int doneX = cx - 30 - doneW;
        if (g_hBtnCancel && IsWindowVisible(g_hBtnCancel)) {
            SetWindowPos(g_hBtnCancel, NULL, doneX, btnY, doneW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
        } else if (g_hBtnInstall && IsWindowVisible(g_hBtnInstall)) {
            SetWindowPos(g_hBtnInstall, NULL, doneX, btnY, doneW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    } else if (g_isAlreadyInstalled) {
        int cancelW = 85;
        int cancelX = cx - 30 - cancelW;
        int uninstW = 100;
        int uninstX = cancelX - 10 - uninstW;
        int instW = 150;
        int instX = uninstX - 10 - instW;

        if (g_hBtnCancel) SetWindowPos(g_hBtnCancel, NULL, cancelX, btnY, cancelW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_hBtnUninstall) SetWindowPos(g_hBtnUninstall, NULL, uninstX, btnY, uninstW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_hBtnInstall) SetWindowPos(g_hBtnInstall, NULL, instX, btnY, instW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        int cancelW = 95;
        int cancelX = cx - 30 - cancelW;
        int instW = 105;
        int instX = cancelX - 12 - instW;

        if (g_hBtnCancel) SetWindowPos(g_hBtnCancel, NULL, cancelX, btnY, cancelW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_hBtnInstall) SetWindowPos(g_hBtnInstall, NULL, instX, btnY, instW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hInst = ((LPCREATESTRUCT)lParam)->hInstance;
        g_isAlreadyInstalled = IsAlreadyInstalled();

        // Fonts
        HFONT hFontUi = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT hFontBold = CreateFontW(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        int startY = 96;
        int gapY = 28;

        // Checkbox 1: Standalone
        g_hChkStandalone = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 30, startY, 20, 20, hwnd, (HMENU)IDC_CHK_STANDALONE, hInst, NULL);
        SendMessageW(g_hChkStandalone, BM_SETCHECK, BST_CHECKED, 0);
        g_hLblStandalone = CreateWindowW(L"STATIC", L"Install Standalone Application (C:\\Program Files\\BeagleSampler)",
                                         WS_CHILD | WS_VISIBLE | SS_NOTIFY, 56, startY - 1, 470, 22, hwnd, (HMENU)IDC_LBL_STANDALONE, hInst, NULL);
        SendMessageW(g_hLblStandalone, WM_SETFONT, (WPARAM)hFontBold, TRUE);

        // Checkbox 2: VST3
        g_hChkVst3 = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 30, startY + gapY, 20, 20, hwnd, (HMENU)IDC_CHK_VST3, hInst, NULL);
        SendMessageW(g_hChkVst3, BM_SETCHECK, BST_CHECKED, 0);
        g_hLblVst3 = CreateWindowW(L"STATIC", L"Install VST3 Plugin for DAWs (REAPER, FL Studio, Cubase...)",
                                   WS_CHILD | WS_VISIBLE | SS_NOTIFY, 56, startY + gapY - 1, 470, 22, hwnd, (HMENU)IDC_LBL_VST3, hInst, NULL);
        SendMessageW(g_hLblVst3, WM_SETFONT, (WPARAM)hFontBold, TRUE);

        // Checkbox 3: CLAP
        g_hChkClap = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 30, startY + gapY * 2, 20, 20, hwnd, (HMENU)IDC_CHK_CLAP, hInst, NULL);
        SendMessageW(g_hChkClap, BM_SETCHECK, BST_CHECKED, 0);
        g_hLblClap = CreateWindowW(L"STATIC", L"Install CLAP Plugin (REAPER, Bitwig Studio...)",
                                   WS_CHILD | WS_VISIBLE | SS_NOTIFY, 56, startY + gapY * 2 - 1, 470, 22, hwnd, (HMENU)IDC_LBL_CLAP, hInst, NULL);
        SendMessageW(g_hLblClap, WM_SETFONT, (WPARAM)hFontBold, TRUE);

        // Checkbox 4: Desktop
        g_hChkDesktop = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 30, startY + gapY * 3 + 8, 20, 20, hwnd, (HMENU)IDC_CHK_DESKTOP, hInst, NULL);
        SendMessageW(g_hChkDesktop, BM_SETCHECK, BST_CHECKED, 0);
        g_hLblDesktop = CreateWindowW(L"STATIC", L"Create Desktop Shortcut",
                                      WS_CHILD | WS_VISIBLE | SS_NOTIFY, 56, startY + gapY * 3 + 7, 190, 22, hwnd, (HMENU)IDC_LBL_DESKTOP, hInst, NULL);
        SendMessageW(g_hLblDesktop, WM_SETFONT, (WPARAM)hFontUi, TRUE);

        // Checkbox 5: Start Menu
        g_hChkStartMenu = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 260, startY + gapY * 3 + 8, 20, 20, hwnd, (HMENU)IDC_CHK_STARTMENU, hInst, NULL);
        SendMessageW(g_hChkStartMenu, BM_SETCHECK, BST_CHECKED, 0);
        g_hLblStartMenu = CreateWindowW(L"STATIC", L"Create Start Menu Shortcut",
                                        WS_CHILD | WS_VISIBLE | SS_NOTIFY, 286, startY + gapY * 3 + 7, 240, 22, hwnd, (HMENU)IDC_LBL_STARTMENU, hInst, NULL);
        SendMessageW(g_hLblStartMenu, WM_SETFONT, (WPARAM)hFontUi, TRUE);

        // Checkbox 6: Launch
        g_hChkLaunch = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 30, startY + gapY * 4 + 14, 20, 20, hwnd, (HMENU)IDC_CHK_LAUNCH, hInst, NULL);
        SendMessageW(g_hChkLaunch, BM_SETCHECK, BST_CHECKED, 0);
        g_hLblLaunch = CreateWindowW(L"STATIC", L"Launch BeagleSampler when setup finishes",
                                     WS_CHILD | WS_VISIBLE | SS_NOTIFY, 56, startY + gapY * 4 + 13, 470, 22, hwnd, (HMENU)IDC_LBL_LAUNCH, hInst, NULL);
        SendMessageW(g_hLblLaunch, WM_SETFONT, (WPARAM)hFontUi, TRUE);

        // Progress & Status
        g_hProgress = CreateWindowExW(0, PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                      30, 280, 500, 16, hwnd, (HMENU)IDC_PROGRESS, hInst, NULL);

        std::wstring initialStatus = g_isAlreadyInstalled 
            ? L"BeagleSampler is currently installed. Choose to reinstall/update or uninstall."
            : L"Ready to install. Choose components above and click Install.";

        g_hStatusLabel = CreateWindowW(L"STATIC", initialStatus.c_str(),
                                       WS_CHILD | WS_VISIBLE | SS_LEFT, 30, 305, 500, 20, hwnd, (HMENU)IDC_STATUS_LABEL, hInst, NULL);
        SendMessageW(g_hStatusLabel, WM_SETFONT, (WPARAM)hFontUi, TRUE);

        // Buttons
        if (g_isAlreadyInstalled) {
            g_hBtnInstall = CreateWindowW(L"BUTTON", L"Reinstall / Update", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                          175, 340, 150, 34, hwnd, (HMENU)IDC_BTN_INSTALL, hInst, NULL);
            SendMessageW(g_hBtnInstall, WM_SETFONT, (WPARAM)hFontBold, TRUE);

            g_hBtnUninstall = CreateWindowW(L"BUTTON", L"Uninstall", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                            335, 340, 100, 34, hwnd, (HMENU)IDC_BTN_UNINSTALL, hInst, NULL);
            SendMessageW(g_hBtnUninstall, WM_SETFONT, (WPARAM)hFontBold, TRUE);

            g_hBtnCancel = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                         445, 340, 85, 34, hwnd, (HMENU)IDC_BTN_CANCEL, hInst, NULL);
            SendMessageW(g_hBtnCancel, WM_SETFONT, (WPARAM)hFontUi, TRUE);
        } else {
            g_hBtnInstall = CreateWindowW(L"BUTTON", L"Install", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                          320, 340, 105, 34, hwnd, (HMENU)IDC_BTN_INSTALL, hInst, NULL);
            SendMessageW(g_hBtnInstall, WM_SETFONT, (WPARAM)hFontBold, TRUE);

            g_hBtnCancel = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                         435, 340, 95, 34, hwnd, (HMENU)IDC_BTN_CANCEL, hInst, NULL);
            SendMessageW(g_hBtnCancel, WM_SETFONT, (WPARAM)hFontUi, TRUE);
        }

        RECT rc;
        GetClientRect(hwnd, &rc);
        UpdateLayout(hwnd, rc.right - rc.left, rc.bottom - rc.top);
        break;
    }

    case WM_SIZE: {
        int cx = LOWORD(lParam);
        int cy = HIWORD(lParam);
        UpdateLayout(hwnd, cx, cy);
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }

    case WM_DPICHANGED: {
        RECT* prcNew = (RECT*)lParam;
        SetWindowPos(hwnd, NULL,
                     prcNew->left, prcNew->top,
                     prcNew->right - prcNew->left,
                     prcNew->bottom - prcNew->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdcStatic = (HDC)wParam;
        HWND hCtrl = (HWND)lParam;
        if (hCtrl == g_hStatusLabel) {
            SetTextColor(hdcStatic, RGB(220, 220, 225));
        } else {
            // Warm Beagle Orange for all option text!
            SetTextColor(hdcStatic, RGB(255, 175, 45));
        }
        SetBkColor(hdcStatic, RGB(24, 24, 26));
        return (INT_PTR)g_hDarkBgBrush;
    }

    case WM_CTLCOLORBTN: {
        HDC hdcBtn = (HDC)wParam;
        SetTextColor(hdcBtn, RGB(255, 175, 45));
        SetBkColor(hdcBtn, RGB(24, 24, 26));
        return (INT_PTR)g_hDarkBgBrush;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int cx = rc.right - rc.left;

        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);

        // Header Background spanning full client width
        SolidBrush headBg(Color(255, 32, 32, 35));
        g.FillRectangle(&headBg, 0, 0, cx, 80);

        Pen linePen(Color(255, 48, 48, 52), 1.0f);
        g.DrawLine(&linePen, 0.0f, 80.0f, (float)cx, 80.0f);

        // Beagle Logo
        if (g_pLogoBmp) {
            g.DrawImage(g_pLogoBmp, 24.0f, 14.0f, 52.0f, 52.0f);
        }

        // Header Titles
        FontFamily ff(L"Segoe UI");
        Gdiplus::Font titleFont(&ff, 16.0f, FontStyleBold, UnitPixel);
        Gdiplus::Font subFont(&ff, 12.0f, FontStyleRegular, UnitPixel);

        SolidBrush textBrush(Color(255, 245, 245, 245));
        SolidBrush subBrush(Color(255, 160, 160, 165));

        std::wstring titleStr = g_isAlreadyInstalled ? L"BeagleSampler Setup (Maintenance)" : L"BeagleSampler Setup";
        std::wstring subStr = g_isAlreadyInstalled 
            ? L"BeagleSampler is currently installed. Choose to update or uninstall."
            : L"Real-time Rolling Audio Sampler & DAW Plugin (VST3 / CLAP)";

        g.DrawString(titleStr.c_str(), -1, &titleFont, PointF(88.0f, 18.0f), &textBrush);
        g.DrawString(subStr.c_str(), -1, &subFont, PointF(88.0f, 44.0f), &subBrush);

        EndPaint(hwnd, &ps);
        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDC_BTN_INSTALL) {
            if (g_installFinished) {
                DestroyWindow(hwnd);
            } else {
                DoInstall(hwnd);
            }
        } else if (id == IDC_BTN_UNINSTALL) {
            PerformUninstallInline(hwnd);
        } else if (id == IDC_BTN_CANCEL) {
            DestroyWindow(hwnd);
        }
        // Clicking labels toggles corresponding checkboxes
        else if (id == IDC_LBL_STANDALONE) {
            LRESULT c = SendMessageW(g_hChkStandalone, BM_GETCHECK, 0, 0);
            SendMessageW(g_hChkStandalone, BM_SETCHECK, (c == BST_CHECKED) ? BST_UNCHECKED : BST_CHECKED, 0);
        } else if (id == IDC_LBL_VST3) {
            LRESULT c = SendMessageW(g_hChkVst3, BM_GETCHECK, 0, 0);
            SendMessageW(g_hChkVst3, BM_SETCHECK, (c == BST_CHECKED) ? BST_UNCHECKED : BST_CHECKED, 0);
        } else if (id == IDC_LBL_CLAP) {
            LRESULT c = SendMessageW(g_hChkClap, BM_GETCHECK, 0, 0);
            SendMessageW(g_hChkClap, BM_SETCHECK, (c == BST_CHECKED) ? BST_UNCHECKED : BST_CHECKED, 0);
        } else if (id == IDC_LBL_DESKTOP) {
            LRESULT c = SendMessageW(g_hChkDesktop, BM_GETCHECK, 0, 0);
            SendMessageW(g_hChkDesktop, BM_SETCHECK, (c == BST_CHECKED) ? BST_UNCHECKED : BST_CHECKED, 0);
        } else if (id == IDC_LBL_STARTMENU) {
            LRESULT c = SendMessageW(g_hChkStartMenu, BM_GETCHECK, 0, 0);
            SendMessageW(g_hChkStartMenu, BM_SETCHECK, (c == BST_CHECKED) ? BST_UNCHECKED : BST_CHECKED, 0);
        } else if (id == IDC_LBL_LAUNCH) {
            LRESULT c = SendMessageW(g_hChkLaunch, BM_GETCHECK, 0, 0);
            SendMessageW(g_hChkLaunch, BM_SETCHECK, (c == BST_CHECKED) ? BST_UNCHECKED : BST_CHECKED, 0);
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;

    case WM_DESTROY:
        if (g_wasUninstalled) {
            ScheduleDelayedSelfDelete();
        }
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    OleInitialize(NULL);
    InitCommonControls();

    // Check module file path
    wchar_t currentExe[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, currentExe, MAX_PATH);

    wchar_t progFiles[MAX_PATH] = { 0 };
    SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, 0, progFiles);
    std::wstring pfDir = std::wstring(progFiles) + L"\\BeagleSampler";

    wchar_t localApp[MAX_PATH] = { 0 };
    SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localApp);
    std::wstring laDir = std::wstring(localApp) + L"\\Programs\\BeagleSampler";

    bool isRunningInInstallDir = false;
    if (_wcsnicmp(currentExe, pfDir.c_str(), pfDir.length()) == 0) {
        isRunningInInstallDir = true;
    } else if (_wcsnicmp(currentExe, laDir.c_str(), laDir.length()) == 0) {
        isRunningInInstallDir = true;
    }

    // Parse command line arguments
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool isUninstallReq = false;
    bool isSilentReq = false;

    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (_wcsicmp(argv[i], L"--uninstall") == 0 || _wcsicmp(argv[i], L"-uninstall") == 0) {
                isUninstallReq = true;
            } else if (_wcsicmp(argv[i], L"--temp-runner") == 0) {
                g_isTempRunner = true;
            } else if (_wcsicmp(argv[i], L"--from-dir") == 0 && i + 1 < argc) {
                g_passedTargetDir = argv[++i];
            } else if (_wcsicmp(argv[i], L"--silent") == 0 || _wcsicmp(argv[i], L"-silent") == 0 ||
                       _wcsicmp(argv[i], L"--headless") == 0 || _wcsicmp(argv[i], L"-s") == 0 ||
                       _wcsicmp(argv[i], L"--update") == 0) {
                isSilentReq = true;
            }
        }
    }

    // Auto-detect update downloaded to %TEMP%: run silently!
    wchar_t tempDirBuf[MAX_PATH] = { 0 };
    GetTempPathW(MAX_PATH, tempDirBuf);
    if (!isUninstallReq && !g_isTempRunner) {
        if (_wcsnicmp(currentExe, tempDirBuf, wcslen(tempDirBuf)) == 0) {
            isSilentReq = true;
        }
    }

    // Standalone headless install / update mode
    if (isSilentReq && !isUninstallReq) {
        if (argv) { LocalFree(argv); argv = nullptr; }
        DoInstallSilent(g_passedTargetDir);
        return 0;
    }

    // If running directly inside the installation directory and not already the temp runner:
    // Delegate to a copy in %TEMP% so the entire BeagleSampler installation folder can be wiped cleanly!
    if (isRunningInInstallDir && !g_isTempRunner) {
        wchar_t tempDir[MAX_PATH] = { 0 };
        GetTempPathW(MAX_PATH, tempDir);
        std::wstring tempExe = std::wstring(tempDir) + L"BeagleSampler_Uninstall.exe";

        CopyFileW(currentExe, tempExe.c_str(), FALSE);

        std::wstring curDirStr = currentExe;
        size_t lastSlash = curDirStr.find_last_of(L"\\/");
        std::wstring parentDir = (lastSlash != std::wstring::npos) ? curDirStr.substr(0, lastSlash) : L"";

        std::wstring cmdLine = L"\"" + tempExe + L"\" --temp-runner";
        if (!parentDir.empty()) {
            cmdLine += L" --from-dir \"" + parentDir + L"\"";
        }
        if (argv) {
            for (int i = 1; i < argc; ++i) {
                if (_wcsicmp(argv[i], L"--temp-runner") != 0 && _wcsicmp(argv[i], L"--from-dir") != 0) {
                    cmdLine += L" \"";
                    cmdLine += argv[i];
                    cmdLine += L"\"";
                }
            }
            LocalFree(argv);
            argv = nullptr;
        }

        STARTUPINFOW si = { sizeof(STARTUPINFOW) };
        PROCESS_INFORMATION pi = { 0 };
        std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
        cmdBuf.push_back(L'\0');

        if (CreateProcessW(tempExe.c_str(), cmdBuf.data(), NULL, NULL, FALSE, 0, NULL, tempDir, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            ExitProcess(0);
        }
    }

    if (argv) {
        LocalFree(argv);
        argv = nullptr;
    }

    // Standalone uninstall mode
    if (isUninstallReq) {
        PerformUninstallStandalone(NULL);
        return 0;
    }

    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    // Load Beagle logo from resource
    HMODULE hMod = hInstance ? hInstance : GetModuleHandleW(NULL);
    HRSRC hRes = FindResourceW(hMod, MAKEINTRESOURCEW(IDR_BEAGLE_LOGO), MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
    if (hRes) {
        HGLOBAL hGlobal = LoadResource(hMod, hRes);
        DWORD resSize = SizeofResource(hMod, hRes);
        void* pData = LockResource(hGlobal);
        if (pData && resSize > 0) {
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, resSize);
            if (hMem) {
                void* pMem = GlobalLock(hMem);
                if (pMem) {
                    memcpy(pMem, pData, resSize);
                    GlobalUnlock(hMem);
                    IStream* pStream = nullptr;
                    if (CreateStreamOnHGlobal(hMem, TRUE, &pStream) == S_OK) {
                        g_pLogoBmp = Bitmap::FromStream(pStream);
                        pStream->Release();
                    }
                }
            }
        }
    }

    g_hDarkBgBrush = CreateSolidBrush(RGB(24, 24, 26));

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hMod;
    wc.lpszClassName = L"BeagleSamplerSetupClass";
    wc.hIcon = LoadIconW(hMod, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = g_hDarkBgBrush;
    RegisterClassExW(&wc);

    // Guaranteed client size 580x460 so controls never get clipped on any DPI/display
    int clientW = 580;
    int clientH = 460;
    RECT rc = { 0, 0, clientW, clientH };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, WS_EX_APPWINDOW);

    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW - winW) / 2;
    int posY = (screenH - winH) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"BeagleSamplerSetupClass", L"BeagleSampler Setup",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                posX, posY, winW, winH, NULL, NULL, hMod, NULL);

    BOOL useDarkMode = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &useDarkMode, sizeof(useDarkMode));
    COLORREF capCol = RGB(32, 32, 35);
    DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &capCol, sizeof(capCol));

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_pLogoBmp) delete g_pLogoBmp;
    if (g_hDarkBgBrush) DeleteObject(g_hDarkBgBrush);
    GdiplusShutdown(gdiplusToken);
    OleUninitialize();
    return 0;
}
