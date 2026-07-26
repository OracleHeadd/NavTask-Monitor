#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <winreg.h>
#include <shlobj.h>
#include <iphlpapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <float.h>
#include <string.h>

#pragma comment(lib, "shlobj.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// Menu ID definitions
#define IDM_ABOUT          1000
#define IDM_TOGGLE_NET     1001
#define IDM_TOGGLE_CPURAM  1002
#define IDM_TOGGLE_DISK    1003
#define IDM_BG_TRANSPARENT 1007
#define IDM_BG_CAPSULE     1008
#define IDM_NET_ALL        1009
#define IDM_EXIT           1010
#define IDM_STARTUP        1011
#define IDM_CPU_UTILITY    1012
#define IDM_CPU_TIME       1013

#define IDM_NET_IF_BASE    2000 // IDs 2000 to 2050 for physical network adapters

// Global configuration and state
BOOL g_showNet = TRUE;
BOOL g_showCpuRam = TRUE;
BOOL g_showDisk = TRUE;
int g_bgMode = 0;  // 0 = True Per-Pixel Transparent (100% clickable via Alpha=1), 1 = Dark Capsule
int g_cpuMetric = 0; // 0 = % Processor Utility (Win 10 / Win 11 22H2 standard), 1 = % Processor Time (Win 11 24H2+)
DWORD g_netInterfaceIndex = (DWORD)-1; // -1 = All operational interfaces
wchar_t g_netInterfaceDesc[128] = L"ALL"; // Description string of selected physical network adapter
BOOL g_allowExit = FALSE;

// Metric strings to render
wchar_t g_strUp[64]   = L"\x25B2 0.00KB/s";
wchar_t g_strDown[64] = L"\x25BC 0.00KB/s";
wchar_t g_strCpu[64]  = L"CPU   0.0%";
wchar_t g_strRam[64]  = L"RAM  0.00G";
wchar_t g_strDiskR[64]= L"RD  0.00KB/s";
wchar_t g_strDiskW[64]= L"WR  0.00KB/s";

// Tracking previous network stats
DWORD64 g_lastInOctets = 0;
DWORD64 g_lastOutOctets = 0;
BOOL g_netInitialized = FALSE;
DWORD g_lastTick = 0;

// Tracking CPU stats fallback
FILETIME g_lastIdleTime = {0};
FILETIME g_lastKernelTime = {0};
FILETIME g_lastUserTime = {0};

// PDH Counters (Exact Task Manager synchronization)
PDH_HQUERY g_pdhQuery = NULL;
PDH_HCOUNTER g_counterCpu = NULL;
PDH_HCOUNTER g_counterRead = NULL;
PDH_HCOUNTER g_counterWrite = NULL;

// Cached font
HFONT g_hFont = NULL;

BOOL IsStartupEnabled() {
    HKEY hKey = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t path[MAX_PATH] = {0};
        DWORD size = sizeof(path);
        if (RegQueryValueExW(hKey, L"NavTask", NULL, NULL, (LPBYTE)path, &size) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return TRUE;
        }
        RegCloseKey(hKey);
    }
    return FALSE;
}

void SetStartup(BOOL enable) {
    HKEY hKey = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            wchar_t path[MAX_PATH];
            wchar_t quotedPath[MAX_PATH + 4];
            GetModuleFileNameW(NULL, path, MAX_PATH);
            swprintf_s(quotedPath, MAX_PATH + 4, L"\"%s\"", path);
            RegSetValueExW(hKey, L"NavTask", 0, REG_SZ, (const BYTE*)quotedPath, (DWORD)((wcslen(quotedPath) + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hKey, L"NavTask");
        }
        RegCloseKey(hKey);
    }
}

BOOL IsWeatherWidgetNearTray() {
    HKEY hKey = NULL;
    DWORD taskbarDa = 1; // Default 1 (Widgets ON) in Windows 11
    DWORD taskbarAl = 1; // Default 1 (Centered Start Menu) in Windows 11
    DWORD size = sizeof(DWORD);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExW(hKey, L"TaskbarDa", NULL, NULL, (LPBYTE)&taskbarDa, &size);
        size = sizeof(DWORD);
        RegQueryValueExW(hKey, L"TaskbarAl", NULL, NULL, (LPBYTE)&taskbarAl, &size);
        RegCloseKey(hKey);
    }
    return (taskbarDa != 0 && taskbarAl == 0);
}

// Sensor to detect Fullscreen overlays like Snipping Tool (Win+Shift+S), Fullscreen Games, or Netflix video
BOOL IsFullscreenOverlayActive(HWND hwndMy) {
    HWND hForeground = GetForegroundWindow();
    if (!hForeground || hForeground == hwndMy) return FALSE;

    HWND hTray = FindWindowW(L"Shell_TrayWnd", NULL);
    if (hForeground == hTray || IsChild(hTray, hForeground)) return FALSE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hForeground, &pid);
    if (pid) {
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProc) {
            hProc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
        }
        if (hProc) {
            wchar_t exePath[MAX_PATH] = {0};
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(hProc, 0, exePath, &size)) {
                wchar_t* exeName = wcsrchr(exePath, L'\\');
                exeName = exeName ? (exeName + 1) : exePath;

                // Absolute whitelist: NEVER hide when interacting with Windows Shell, Start Menu, Search, or Taskbar processes!
                if (_wcsicmp(exeName, L"StartMenuExperienceHost.exe") == 0 ||
                    _wcsicmp(exeName, L"SearchHost.exe") == 0 ||
                    _wcsicmp(exeName, L"SearchApp.exe") == 0 ||
                    _wcsicmp(exeName, L"ShellExperienceHost.exe") == 0 ||
                    _wcsicmp(exeName, L"explorer.exe") == 0 ||
                    _wcsicmp(exeName, L"SystemSettings.exe") == 0 ||
                    _wcsicmp(exeName, L"TextInputHost.exe") == 0 ||
                    _wcsicmp(exeName, L"ApplicationFrameHost.exe") == 0 ||
                    _wcsicmp(exeName, L"RuntimeBroker.exe") == 0) {
                    CloseHandle(hProc);
                    return FALSE;
                }

                // Immediate blacklist: Only hide on real screenshot & print screen tools!
                if (_wcsicmp(exeName, L"ScreenClippingHost.exe") == 0 ||
                    _wcsicmp(exeName, L"SnippingTool.exe") == 0 ||
                    _wcsicmp(exeName, L"SnippingToolHost.exe") == 0 ||
                    _wcsicmp(exeName, L"SnipAndSketch.exe") == 0 ||
                    _wcsicmp(exeName, L"ShareX.exe") == 0 ||
                    _wcsicmp(exeName, L"Lightshot.exe") == 0 ||
                    _wcsicmp(exeName, L"Greenshot.exe") == 0 ||
                    _wcsicmp(exeName, L"Gyazo.exe") == 0 ||
                    _wcsicmp(exeName, L"ScreenCapture.exe") == 0 ||
                    _wcsicmp(exeName, L"WindowsCamera.exe") == 0) {
                    CloseHandle(hProc);
                    return TRUE;
                }
            }
            CloseHandle(hProc);
        }
    }

    wchar_t className[256] = {0};
    GetClassNameW(hForeground, className, 256);
    if (wcscmp(className, L"Progman") == 0 || wcscmp(className, L"WorkerW") == 0 || 
        wcscmp(className, L"Windows.UI.Core.CoreWindow") == 0 || 
        wcscmp(className, L"Xaml_WindowedPopupClass") == 0) {
        return FALSE;
    }

    // Direct check for Screenshot / Snipping Tool window classes
    if (wcsstr(className, L"ScreenClipping") || wcsstr(className, L"Snipping") || 
        wcsstr(className, L"Snipper") || wcsstr(className, L"Lightshot") || 
        wcsstr(className, L"Greenshot") || wcsstr(className, L"ShareX") || 
        wcsstr(className, L"Gyazo")) {
        return TRUE;
    }

    // Check window titles (English & Portuguese screenshot titles)
    wchar_t title[256] = {0};
    GetWindowTextW(hForeground, title, 256);
    if (wcslen(title) > 0) {
        if (wcsstr(title, L"Screen Clipping") || wcsstr(title, L"Snipping Tool") || 
            wcsstr(title, L"Snip & Sketch") || wcsstr(title, L"Ferramenta de Captura") || 
            wcsstr(title, L"Captura de tela") || wcsstr(title, L"Recorte e Esboço") ||
            wcsstr(title, L"Lightshot") || wcsstr(title, L"Greenshot") || wcsstr(title, L"ShareX")) {
            return TRUE;
        }
    }

    return FALSE;
}

void FormatNetSpeed(double bytesPerSec, wchar_t* outBuf, size_t bufSize, wchar_t symbol) {
    if (isnan(bytesPerSec) || isinf(bytesPerSec) || bytesPerSec < 0.0) {
        bytesPerSec = 0.0;
    }
    double val;
    const wchar_t* unit;
    if (bytesPerSec < 1024.0) {
        val = bytesPerSec; unit = L"B/s ";
    } else if (bytesPerSec < 1024.0 * 1024.0) {
        val = bytesPerSec / 1024.0; unit = L"KB/s";
    } else if (bytesPerSec < 1024.0 * 1024.0 * 1024.0) {
        val = bytesPerSec / (1024.0 * 1024.0); unit = L"MB/s";
    } else {
        val = bytesPerSec / (1024.0 * 1024.0 * 1024.0); unit = L"GB/s";
    }

    if (val >= 100.0) {
        swprintf_s(outBuf, bufSize, L"%lc %4.0f%s", symbol, val, unit);
    } else if (val >= 10.0) {
        swprintf_s(outBuf, bufSize, L"%lc %4.1f%s", symbol, val, unit);
    } else {
        swprintf_s(outBuf, bufSize, L"%lc %4.2f%s", symbol, val, unit);
    }
}

void FormatDiskSpeed(double bytesPerSec, wchar_t* outBuf, size_t bufSize, const wchar_t* label) {
    if (isnan(bytesPerSec) || isinf(bytesPerSec) || bytesPerSec < 0.0) {
        bytesPerSec = 0.0;
    }
    double val;
    const wchar_t* unit;
    if (bytesPerSec < 1024.0) {
        val = bytesPerSec; unit = L"B/s ";
    } else if (bytesPerSec < 1024.0 * 1024.0) {
        val = bytesPerSec / 1024.0; unit = L"KB/s";
    } else if (bytesPerSec < 1024.0 * 1024.0 * 1024.0) {
        val = bytesPerSec / (1024.0 * 1024.0); unit = L"MB/s";
    } else {
        val = bytesPerSec / (1024.0 * 1024.0 * 1024.0); unit = L"GB/s";
    }

    if (val >= 100.0) {
        swprintf_s(outBuf, bufSize, L"%s %3.0f%s", label, val, unit);
    } else if (val >= 10.0) {
        swprintf_s(outBuf, bufSize, L"%s %3.1f%s", label, val, unit);
    } else {
        swprintf_s(outBuf, bufSize, L"%s %3.2f%s", label, val, unit);
    }
}

ULONGLONG FileTimeToULL(const FILETIME* ft) {
    return (((ULONGLONG)ft->dwHighDateTime) << 32) | ft->dwLowDateTime;
}

void InitCpuCounter() {
    if (g_counterCpu && g_pdhQuery) {
        PdhRemoveCounter(g_counterCpu);
        g_counterCpu = NULL;
    }
    if (g_pdhQuery) {
        if (g_cpuMetric == 0) {
            if (PdhAddEnglishCounterW(g_pdhQuery, L"\\Processor Information(_Total)\\% Processor Utility", 0, &g_counterCpu) != ERROR_SUCCESS) {
                PdhAddEnglishCounterW(g_pdhQuery, L"\\Processor(_Total)\\% Processor Time", 0, &g_counterCpu);
            }
        } else {
            if (PdhAddEnglishCounterW(g_pdhQuery, L"\\Processor Information(_Total)\\% Processor Time", 0, &g_counterCpu) != ERROR_SUCCESS) {
                PdhAddEnglishCounterW(g_pdhQuery, L"\\Processor(_Total)\\% Processor Time", 0, &g_counterCpu);
            }
        }
        PdhCollectQueryData(g_pdhQuery);
    }
}

void InitStats() {
    g_lastTick = GetTickCount();
    GetSystemTimes(&g_lastIdleTime, &g_lastKernelTime, &g_lastUserTime);

    if (PdhOpenQuery(NULL, 0, &g_pdhQuery) == ERROR_SUCCESS) {
        InitCpuCounter();
        PdhAddEnglishCounterW(g_pdhQuery, L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", 0, &g_counterRead);
        PdhAddEnglishCounterW(g_pdhQuery, L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", 0, &g_counterWrite);
        PdhCollectQueryData(g_pdhQuery);
    }

    g_hFont = CreateFontW(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, 
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, 
                          ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
}

void UpdateStats() {
    DWORD currentTick = GetTickCount();
    double dt = (currentTick - g_lastTick) / 1000.0;
    if (dt <= 0.001) dt = 1.0;
    g_lastTick = currentTick;

    // 1. Network Speed
    DWORD dwSize = 0;
    GetIfTable(NULL, &dwSize, TRUE);
    PMIB_IFTABLE pIfTable = (PMIB_IFTABLE)malloc(dwSize + 1024);
    if (pIfTable) {
        if (GetIfTable(pIfTable, &dwSize, TRUE) == NO_ERROR) {
            DWORD64 totalIn = 0, totalOut = 0;
            BOOL interfaceFound = FALSE;
            for (DWORD i = 0; i < pIfTable->dwNumEntries; i++) {
                if (pIfTable->table[i].dwType != MIB_IF_TYPE_LOOPBACK) {
                    if (g_netInterfaceIndex == (DWORD)-1) {
                        if (pIfTable->table[i].dwOperStatus == MIB_IF_OPER_STATUS_OPERATIONAL) {
                            totalIn += pIfTable->table[i].dwInOctets;
                            totalOut += pIfTable->table[i].dwOutOctets;
                            interfaceFound = TRUE;
                        }
                    } else if (pIfTable->table[i].dwIndex == g_netInterfaceIndex) {
                        totalIn += pIfTable->table[i].dwInOctets;
                        totalOut += pIfTable->table[i].dwOutOctets;
                        interfaceFound = TRUE;
                        break;
                    }
                }
            }
            if (g_netInitialized && interfaceFound) {
                double downSpeed = (totalIn >= g_lastInOctets) ? (totalIn - g_lastInOctets) / dt : 0.0;
                double upSpeed = (totalOut >= g_lastOutOctets) ? (totalOut - g_lastOutOctets) / dt : 0.0;
                FormatNetSpeed(upSpeed, g_strUp, 64, L'\x25B2');
                FormatNetSpeed(downSpeed, g_strDown, 64, L'\x25BC');
            } else if (!interfaceFound && g_netInterfaceIndex != (DWORD)-1) {
                FormatNetSpeed(0.0, g_strUp, 64, L'\x25B2');
                FormatNetSpeed(0.0, g_strDown, 64, L'\x25BC');
            }
            if (!g_netInitialized) g_netInitialized = TRUE;
            g_lastInOctets = totalIn;
            g_lastOutOctets = totalOut;
        }
        free(pIfTable);
    }

    // 2. CPU & RAM & Disk (Exact Task Manager synchronization)
    BOOL cpuCollected = FALSE;
    if (g_pdhQuery) {
        if (PdhCollectQueryData(g_pdhQuery) == ERROR_SUCCESS) {
            PDH_FMT_COUNTERVALUE valCpu = {0};
            if (g_counterCpu && PdhGetFormattedCounterValue(g_counterCpu, PDH_FMT_DOUBLE, NULL, &valCpu) == ERROR_SUCCESS) {
                double cpuPct = valCpu.doubleValue;
                if (isnan(cpuPct) || isinf(cpuPct) || cpuPct < 0.0) cpuPct = 0.0;
                if (cpuPct > 100.0) cpuPct = 100.0;
                swprintf_s(g_strCpu, 64, L"CPU %4.1f%%", cpuPct);
                cpuCollected = TRUE;
            }

            PDH_FMT_COUNTERVALUE valRead = {0}, valWrite = {0};
            double diskRead = 0.0, diskWrite = 0.0;
            if (g_counterRead && PdhGetFormattedCounterValue(g_counterRead, PDH_FMT_DOUBLE, NULL, &valRead) == ERROR_SUCCESS) {
                diskRead = valRead.doubleValue;
            }
            if (g_counterWrite && PdhGetFormattedCounterValue(g_counterWrite, PDH_FMT_DOUBLE, NULL, &valWrite) == ERROR_SUCCESS) {
                diskWrite = valWrite.doubleValue;
            }
            FormatDiskSpeed(diskRead, g_strDiskR, 64, L"RD");
            FormatDiskSpeed(diskWrite, g_strDiskW, 64, L"WR");
        }
    }

    if (!cpuCollected) {
        FILETIME idleTime, kernelTime, userTime;
        if (GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
            ULONGLONG idle = FileTimeToULL(&idleTime) - FileTimeToULL(&g_lastIdleTime);
            ULONGLONG kernel = FileTimeToULL(&kernelTime) - FileTimeToULL(&g_lastKernelTime);
            ULONGLONG user = FileTimeToULL(&userTime) - FileTimeToULL(&g_lastUserTime);
            ULONGLONG total = kernel + user;
            double cpuPct = (total > 0) ? (double)(total - idle) * 100.0 / (double)total : 0.0;
            if (isnan(cpuPct) || isinf(cpuPct) || cpuPct < 0.0) cpuPct = 0.0;
            if (cpuPct > 100.0) cpuPct = 100.0;
            swprintf_s(g_strCpu, 64, L"CPU %4.1f%%", cpuPct);

            g_lastIdleTime = idleTime;
            g_lastKernelTime = kernelTime;
            g_lastUserTime = userTime;
        }
    }

    MEMORYSTATUSEX memInfo = {0};
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memInfo)) {
        double memUsedGB = (memInfo.ullTotalPhys - memInfo.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
        if (isnan(memUsedGB) || isinf(memUsedGB) || memUsedGB < 0.0) memUsedGB = 0.0;
        swprintf_s(g_strRam, 64, L"RAM %4.2fG", memUsedGB);
    }
}

void GetConfigPath(wchar_t* path, DWORD maxLen) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    
    // Check if installed in Program Files or protected system directories
    BOOL inProgramFiles = FALSE;
    wchar_t progFiles[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, 0, progFiles)) && progFiles[0]) {
        if (_wcsnicmp(exePath, progFiles, wcslen(progFiles)) == 0) inProgramFiles = TRUE;
    }
    if (!inProgramFiles && SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILESX86, NULL, 0, progFiles)) && progFiles[0]) {
        if (_wcsnicmp(exePath, progFiles, wcslen(progFiles)) == 0) inProgramFiles = TRUE;
    }

    wchar_t localAppData[MAX_PATH] = {0};
    BOOL hasAppData = SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localAppData));
    
    wchar_t appDataConfig[MAX_PATH] = {0};
    if (hasAppData) {
        swprintf_s(appDataConfig, MAX_PATH, L"%s\\NavTask", localAppData);
        CreateDirectoryW(appDataConfig, NULL);
        swprintf_s(appDataConfig, MAX_PATH, L"%s\\NavTask\\navtask.ini", localAppData);
    }
    
    // If installed in Program Files, force LocalAppData
    if (inProgramFiles && hasAppData) {
        wcscpy_s(path, maxLen, appDataConfig);
        return;
    }

    // Otherwise (Portable mode), test if executable folder is writable
    wchar_t* p = wcsrchr(exePath, L'\\');
    if (p) *p = 0;
    wchar_t portableConfig[MAX_PATH];
    swprintf_s(portableConfig, MAX_PATH, L"%s\\navtask.ini", exePath);

    HANDLE hFile = CreateFileW(portableConfig, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(hFile);
        wcscpy_s(path, maxLen, portableConfig);
        return;
    }

    // Fallback if exe directory is read-only or access denied
    if (hasAppData) {
        wcscpy_s(path, maxLen, appDataConfig);
        return;
    }

    wcscpy_s(path, maxLen, portableConfig);
}

void SaveConfig() {
    wchar_t path[MAX_PATH];
    GetConfigPath(path, MAX_PATH);

    wchar_t buf[32];
    swprintf_s(buf, 32, L"%d", g_showNet);
    WritePrivateProfileStringW(L"Settings", L"ShowNet", buf, path);
    swprintf_s(buf, 32, L"%d", g_showCpuRam);
    WritePrivateProfileStringW(L"Settings", L"ShowCpuRam", buf, path);
    swprintf_s(buf, 32, L"%d", g_showDisk);
    WritePrivateProfileStringW(L"Settings", L"ShowDisk", buf, path);
    swprintf_s(buf, 32, L"%d", g_bgMode);
    WritePrivateProfileStringW(L"Settings", L"BgMode", buf, path);
    swprintf_s(buf, 32, L"%d", g_cpuMetric);
    WritePrivateProfileStringW(L"Settings", L"CpuMetric", buf, path);
    swprintf_s(buf, 32, L"%lu", g_netInterfaceIndex);
    WritePrivateProfileStringW(L"Settings", L"NetInterfaceIndex", buf, path);
    WritePrivateProfileStringW(L"Settings", L"NetInterfaceDesc", (g_netInterfaceIndex == (DWORD)-1 || wcslen(g_netInterfaceDesc) == 0) ? L"ALL" : g_netInterfaceDesc, path);

    // Explicitly flush INI cache to disk immediately to guarantee persistence across reboots
    WritePrivateProfileStringW(NULL, NULL, NULL, path);
}

void LoadConfig() {
    wchar_t path[MAX_PATH];
    GetConfigPath(path, MAX_PATH);

    g_showNet = GetPrivateProfileIntW(L"Settings", L"ShowNet", 1, path);
    g_showCpuRam = GetPrivateProfileIntW(L"Settings", L"ShowCpuRam", 1, path);
    g_showDisk = GetPrivateProfileIntW(L"Settings", L"ShowDisk", 1, path);
    g_bgMode = GetPrivateProfileIntW(L"Settings", L"BgMode", 0, path);
    g_cpuMetric = GetPrivateProfileIntW(L"Settings", L"CpuMetric", 0, path);

    wchar_t buf[64];
    if (GetPrivateProfileStringW(L"Settings", L"NetInterfaceIndex", L"-1", buf, 64, path) > 0) {
        g_netInterfaceIndex = (DWORD)_wtoi64(buf);
    }
    
    wchar_t descBuf[128] = {0};
    GetPrivateProfileStringW(L"Settings", L"NetInterfaceDesc", L"ALL", descBuf, 128, path);
    wcscpy_s(g_netInterfaceDesc, 128, descBuf);
    
    if (_wcsicmp(g_netInterfaceDesc, L"ALL") != 0 && wcslen(g_netInterfaceDesc) > 0) {
        DWORD dwSize = 0;
        GetIfTable(NULL, &dwSize, TRUE);
        PMIB_IFTABLE pIfTable = (PMIB_IFTABLE)malloc(dwSize + 1024);
        if (pIfTable) {
            if (GetIfTable(pIfTable, &dwSize, TRUE) == NO_ERROR) {
                BOOL found = FALSE;
                for (DWORD i = 0; i < pIfTable->dwNumEntries; i++) {
                    if (pIfTable->table[i].dwType != MIB_IF_TYPE_LOOPBACK && strlen((const char*)pIfTable->table[i].bDescr) > 0) {
                        wchar_t wDescr[128];
                        MultiByteToWideChar(CP_ACP, 0, (const char*)pIfTable->table[i].bDescr, -1, wDescr, 128);
                        if (wcscmp(wDescr, g_netInterfaceDesc) == 0) {
                            g_netInterfaceIndex = pIfTable->table[i].dwIndex;
                            found = TRUE;
                            break;
                        }
                    }
                }
                if (!found && g_netInterfaceIndex == (DWORD)-1) {
                    wcscpy_s(g_netInterfaceDesc, 128, L"ALL");
                }
            }
            free(pIfTable);
        }
    } else {
        g_netInterfaceIndex = (DWORD)-1;
    }
}

int CalculateWidth() {
    int w = 6;
    if (g_showNet) w += 106;
    if (g_showCpuRam) w += 90;
    if (g_showDisk) w += 106;
    if (w == 6) return 30;
    return w;
}

void RenderWindow(HWND hwnd) {
    RECT rc;
    GetWindowRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // Top-down bitmap
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    DWORD* pPixels = NULL;
    HBITMAP hbm = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, (void**)&pPixels, NULL, 0);
    if (!hbm || !pPixels) {
        if (hbm) DeleteObject(hbm);
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return;
    }

    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hbm);
    ZeroMemory(pPixels, w * h * 4);

    if (g_bgMode == 1) {
        HBRUSH hBg = CreateSolidBrush(RGB(18, 22, 30));
        HPEN hPen = CreatePen(PS_SOLID, 1, RGB(55, 65, 85));
        HPEN hOldPen = (HPEN)SelectObject(hdcMem, hPen);
        HBRUSH hOldBrush = (HBRUSH)SelectObject(hdcMem, hBg);
        RoundRect(hdcMem, 0, 0, w, h, 8, 8);
        SelectObject(hdcMem, hOldPen);
        SelectObject(hdcMem, hOldBrush);
        DeleteObject(hPen);
        DeleteObject(hBg);
    }

    HFONT hOldFont = (HFONT)SelectObject(hdcMem, g_hFont);
    SetBkMode(hdcMem, TRANSPARENT);
    SetTextColor(hdcMem, RGB(255, 255, 255));

    int curX = 6;
    if (g_showNet) {
        TextOutW(hdcMem, curX, 1, g_strUp, (int)wcslen(g_strUp));
        TextOutW(hdcMem, curX, 17, g_strDown, (int)wcslen(g_strDown));
        curX += 106;
    }
    if (g_showCpuRam) {
        TextOutW(hdcMem, curX, 1, g_strCpu, (int)wcslen(g_strCpu));
        TextOutW(hdcMem, curX, 17, g_strRam, (int)wcslen(g_strRam));
        curX += 90;
    }
    if (g_showDisk) {
        TextOutW(hdcMem, curX, 1, g_strDiskR, (int)wcslen(g_strDiskR));
        TextOutW(hdcMem, curX, 17, g_strDiskW, (int)wcslen(g_strDiskW));
    }

    SelectObject(hdcMem, hOldFont);

    int totalPixels = w * h;
    if (g_bgMode == 0) {
        for (int i = 0; i < totalPixels; i++) {
            DWORD c = pPixels[i];
            BYTE r = (c >> 16) & 0xFF;
            BYTE g = (c >> 8) & 0xFF;
            BYTE b = c & 0xFF;
            BYTE brightness = (BYTE)(((DWORD)r * 299 + (DWORD)g * 587 + (DWORD)b * 114) / 1000);

            if (brightness == 0) {
                pPixels[i] = 0x01000000; // Alpha = 1 (invisible, but solid to mouse clicks 100%)
            } else {
                BYTE alpha = brightness;
                if (alpha < 5) alpha = 5;
                pPixels[i] = ((DWORD)alpha << 24) | ((DWORD)alpha << 16) | ((DWORD)alpha << 8) | (DWORD)alpha;
            }
        }
    } else {
        for (int i = 0; i < totalPixels; i++) {
            DWORD c = pPixels[i];
            BYTE r = (c >> 16) & 0xFF;
            BYTE g = (c >> 8) & 0xFF;
            BYTE b = c & 0xFF;
            if (r == 0 && g == 0 && b == 0) {
                pPixels[i] = 0x01000000;
            } else if (r == 255 && g == 255 && b == 255) {
                pPixels[i] = 0xFFFFFFFF;
            } else {
                BYTE a = 230;
                BYTE pr = (r * a) / 255;
                BYTE pg = (g * a) / 255;
                BYTE pb = (b * a) / 255;
                pPixels[i] = ((DWORD)a << 24) | ((DWORD)pr << 16) | ((DWORD)pg << 8) | (DWORD)pb;
            }
        }
    }

    POINT ptSrc = {0, 0};
    POINT ptWin = {rc.left, rc.top};
    SIZE szWin = {w, h};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};

    UpdateLayeredWindow(hwnd, hdcScreen, &ptWin, &szWin, hdcMem, &ptSrc, 0, &bf, ULW_ALPHA);

    SelectObject(hdcMem, hOldBitmap);
    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

void RepositionOnTaskbar(HWND hwnd) {
    if (IsFullscreenOverlayActive(hwnd)) return;

    HWND hwndTray = FindWindowW(L"Shell_TrayWnd", NULL);
    int w = CalculateWidth();
    int h = 34;

    if (!hwndTray) {
        if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
        return;
    }

    if (!IsWindowVisible(hwnd) && !IsFullscreenOverlayActive(hwnd)) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }

    // Guarantee that NavTask is owned by Shell_TrayWnd so Windows 11 elevates NavTask into the shell Z-order band when opening Start Menu!
    if (GetWindowLongPtr(hwnd, GWLP_HWNDPARENT) != (LONG_PTR)hwndTray) {
        SetWindowLongPtr(hwnd, GWLP_HWNDPARENT, (LONG_PTR)hwndTray);
    }

    RECT trayRect;
    GetWindowRect(hwndTray, &trayRect);
    int y = trayRect.top + ((trayRect.bottom - trayRect.top - h) / 2);
    int x = trayRect.right - w - 240;

    HWND hwndNotify = FindWindowExW(hwndTray, NULL, L"TrayNotifyWnd", NULL);
    if (hwndNotify) {
        RECT notifyRect;
        if (GetWindowRect(hwndNotify, &notifyRect)) {
            int margin = IsWeatherWidgetNearTray() ? 185 : 12;
            x = notifyRect.left - w - margin;
        }
    }

    SetWindowPos(hwnd, HWND_NOTOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
    RenderWindow(hwnd);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            InitStats();
            SetTimer(hwnd, 1, 1000, NULL); // Metric interval: 1s
            SetTimer(hwnd, 2, 150, NULL);  // Fast Z-order persistence interval: 150ms to instantly overcome Taskbar occlusion on Start Menu open
            return 0;

        case WM_TIMER:
            if (wParam == 1) {
                UpdateStats();
                if (IsFullscreenOverlayActive(hwnd)) {
                    if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
                    return 0;
                } else {
                    if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                    RepositionOnTaskbar(hwnd); 
                }
            } else if (wParam == 2) {
                if (IsFullscreenOverlayActive(hwnd)) {
                    if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
                    return 0;
                } else {
                    if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                    HWND hwndTray = FindWindowW(L"Shell_TrayWnd", NULL);
                    if (hwndTray && GetWindowLongPtr(hwnd, GWLP_HWNDPARENT) != (LONG_PTR)hwndTray) {
                        SetWindowLongPtr(hwnd, GWLP_HWNDPARENT, (LONG_PTR)hwndTray);
                    }
                    // Toggle NOTOPMOST -> TOPMOST to force Windows DWM to raise NavTask above Shell_TrayWnd and Start Menu XAML overlays!
                    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
                    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
                }
            }
            return 0;

        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            RepositionOnTaskbar(hwnd);
            return 0;

        case WM_CLOSE:
            if (!g_allowExit) {
                return 0; 
            }
            break;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: {
            POINT pt;
            GetCursorPos(&pt);
            
            // Anchor menu to the top edge of Shell_TrayWnd so it NEVER gets cut off or hidden by the taskbar!
            HWND hwndTray = FindWindowW(L"Shell_TrayWnd", NULL);
            int menuY = pt.y;
            if (hwndTray) {
                RECT trayRc;
                if (GetWindowRect(hwndTray, &trayRc)) {
                    menuY = trayRc.top; 
                }
            }

            HMENU hMenu = CreatePopupMenu();

            HMENU hOptionsMenu = CreatePopupMenu();
            AppendMenuW(hOptionsMenu, MF_STRING | (IsStartupEnabled() ? MF_CHECKED : MF_UNCHECKED), IDM_STARTUP, L"Start automatically with Windows");
            AppendMenuW(hOptionsMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hOptionsMenu, MF_STRING, IDM_EXIT, L"Exit NavTask Monitor");

            AppendMenuW(hMenu, MF_STRING, IDM_ABOUT, L"\u2139 About NavTask Monitor");
            AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hOptionsMenu, L"Options");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

            AppendMenuW(hMenu, MF_STRING | (g_showNet ? MF_CHECKED : MF_UNCHECKED), IDM_TOGGLE_NET, L"NavTask: Network Speed (\u25B2/\u25BC)");
            AppendMenuW(hMenu, MF_STRING | (g_showCpuRam ? MF_CHECKED : MF_UNCHECKED), IDM_TOGGLE_CPURAM, L"NavTask: CPU & RAM Usage");
            AppendMenuW(hMenu, MF_STRING | (g_showDisk ? MF_CHECKED : MF_UNCHECKED), IDM_TOGGLE_DISK, L"NavTask: Disk Activity (Read/Write)");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

            // Network Interfaces Submenu (Filtered strictly to Task Manager physical hardware cards)
            HMENU hNetMenu = CreatePopupMenu();
            AppendMenuW(hNetMenu, MF_STRING | (g_netInterfaceIndex == (DWORD)-1 ? MF_CHECKED : MF_UNCHECKED), IDM_NET_ALL, L"\u2714 All Interfaces (Automatic)");
            AppendMenuW(hNetMenu, MF_SEPARATOR, 0, NULL);

            DWORD dwSize = 0;
            GetIfTable(NULL, &dwSize, TRUE);
            PMIB_IFTABLE pIfTable = (PMIB_IFTABLE)malloc(dwSize + 1024);
            if (pIfTable) {
                if (GetIfTable(pIfTable, &dwSize, TRUE) == NO_ERROR) {
                    int menuIdx = 0;
                    for (DWORD i = 0; i < pIfTable->dwNumEntries; i++) {
                        if (pIfTable->table[i].dwType != MIB_IF_TYPE_LOOPBACK && strlen((const char*)pIfTable->table[i].bDescr) > 0) {
                            const char* desc = (const char*)pIfTable->table[i].bDescr;
                            if (strstr(desc, "Miniport") || strstr(desc, "miniport") || strstr(desc, "Loopback") || 
                                strstr(desc, "Virtual") || strstr(desc, "virtual") || strstr(desc, "Teredo") || 
                                strstr(desc, "Pseudo") || strstr(desc, "Kernel") || strstr(desc, "Filter") || 
                                strstr(desc, "ISATAP") || pIfTable->table[i].dwPhysAddrLen != 6) {
                                continue;
                            }
                            wchar_t wDescr[128];
                            MultiByteToWideChar(CP_ACP, 0, desc, -1, wDescr, 128);
                            BOOL checked = (g_netInterfaceIndex == pIfTable->table[i].dwIndex);
                            AppendMenuW(hNetMenu, MF_STRING | (checked ? MF_CHECKED : MF_UNCHECKED), IDM_NET_IF_BASE + i, wDescr);
                            menuIdx++;
                            if (menuIdx >= 25) break;
                        }
                    }
                }
                free(pIfTable);
            }
            AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hNetMenu, L"Select Network Interface");
            
            HMENU hCpuMenu = CreatePopupMenu();
            AppendMenuW(hCpuMenu, MF_STRING | (g_cpuMetric == 0 ? MF_CHECKED : MF_UNCHECKED), IDM_CPU_UTILITY, L"Utility Mode (% Processor Utility - Win 11 22H2 / 23H2)");
            AppendMenuW(hCpuMenu, MF_STRING | (g_cpuMetric == 1 ? MF_CHECKED : MF_UNCHECKED), IDM_CPU_TIME,    L"Pure Time Mode (% Processor Time - Win 11 24H2 & Later)");
            AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hCpuMenu, L"CPU Calculation Mode");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

            AppendMenuW(hMenu, MF_STRING | (g_bgMode == 0 ? MF_CHECKED : MF_UNCHECKED), IDM_BG_TRANSPARENT, L"Background: True Transparent (Per-Pixel Alpha)");
            AppendMenuW(hMenu, MF_STRING | (g_bgMode == 1 ? MF_CHECKED : MF_UNCHECKED), IDM_BG_CAPSULE,    L"Background: Dark Capsule (Dark Mode)");

            SetForegroundWindow(hwnd);
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, menuY, 0, hwnd, NULL);
            DestroyMenu(hMenu);
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDM_ABOUT) {
                MessageBoxW(hwnd, 
                    L"NavTask Monitor v10.2 for Windows 11\n\n"
                    L"Developed by: Mauro Carvalho\n"
                    L"Contact / Support: mauroroberto83@gmail.com\n"
                    L"License: Open-Source (Freeware / MIT)\n\n"
                    L"Key Features:\n"
                    L"\x2022 Ultra-lightweight Taskbar Telemetry (< 7MB RAM)\n"
                    L"\x2022 PDH Kernel Engine synchronized with Task Manager\n"
                    L"\x2022 Live Windows 11 Weather Widget auto-detection", 
                    L"About NavTask Monitor", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
                return 0;
            } else if (id == IDM_TOGGLE_NET) {
                g_showNet = !g_showNet;
            } else if (id == IDM_TOGGLE_CPURAM) {
                g_showCpuRam = !g_showCpuRam;
            } else if (id == IDM_TOGGLE_DISK) {
                g_showDisk = !g_showDisk;
            } else if (id == IDM_BG_TRANSPARENT) {
                g_bgMode = 0;
            } else if (id == IDM_BG_CAPSULE) {
                g_bgMode = 1;
            } else if (id == IDM_CPU_UTILITY) {
                g_cpuMetric = 0;
                InitCpuCounter();
            } else if (id == IDM_CPU_TIME) {
                g_cpuMetric = 1;
                InitCpuCounter();
            } else if (id == IDM_NET_ALL) {
                g_netInterfaceIndex = (DWORD)-1;
                wcscpy_s(g_netInterfaceDesc, 128, L"ALL");
            } else if (id >= IDM_NET_IF_BASE && id <= IDM_NET_IF_BASE + 200) {
                DWORD idx = id - IDM_NET_IF_BASE;
                DWORD dwSize = 0;
                GetIfTable(NULL, &dwSize, TRUE);
                PMIB_IFTABLE pIfTable = (PMIB_IFTABLE)malloc(dwSize + 1024);
                if (pIfTable) {
                    if (GetIfTable(pIfTable, &dwSize, TRUE) == NO_ERROR) {
                        if (idx < pIfTable->dwNumEntries) {
                            g_netInterfaceIndex = pIfTable->table[idx].dwIndex;
                            MultiByteToWideChar(CP_ACP, 0, (const char*)pIfTable->table[idx].bDescr, -1, g_netInterfaceDesc, 128);
                        }
                    }
                    free(pIfTable);
                }
            } else if (id == IDM_STARTUP) {
                BOOL newState = !IsStartupEnabled();
                SetStartup(newState);
                wchar_t cfgPath[MAX_PATH];
                GetConfigPath(cfgPath, MAX_PATH);
                WritePrivateProfileStringW(L"Settings", L"StartupInitialized", L"1", cfgPath);
                WritePrivateProfileStringW(NULL, NULL, NULL, cfgPath);
                return 0;
            } else if (id == IDM_EXIT) {
                DestroyWindow(hwnd);
                return 0;
            }
            SaveConfig();
            RepositionOnTaskbar(hwnd);
            return 0;
        }

        case WM_DESTROY:
            if (g_pdhQuery) PdhCloseQuery(g_pdhQuery);
            if (g_hFont) DeleteObject(g_hFont);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"NavTask_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS || !hMutex) {
        HWND hwndExisting = FindWindowW(L"NavTaskWinClass", L"NavTask Monitor");
        if (hwndExisting) {
            SetForegroundWindow(hwndExisting);
        }
        return 0;
    }

    LoadConfig();
    wchar_t cfgPath[MAX_PATH];
    GetConfigPath(cfgPath, MAX_PATH);
    if (GetPrivateProfileIntW(L"Settings", L"StartupInitialized", 0, cfgPath) == 0) {
        if (!IsStartupEnabled()) {
            SetStartup(TRUE);
        }
        WritePrivateProfileStringW(L"Settings", L"StartupInitialized", L"1", cfgPath);
        WritePrivateProfileStringW(NULL, NULL, NULL, cfgPath);
    }

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"NavTaskWinClass";
    RegisterClassExW(&wc);

    int w = CalculateWidth();
    int h = 34;

    HWND hwndTray = FindWindowW(L"Shell_TrayWnd", NULL);
    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        L"NavTaskWinClass",
        L"NavTask Monitor",
        WS_POPUP,
        0, 0, w, h,
        hwndTray, NULL, hInstance, NULL
    );

    if (!hwnd) return 0;

    RepositionOnTaskbar(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}
