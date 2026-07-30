#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <winreg.h>
#include <shlobj.h>
#include <iphlpapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <float.h>
#include <string.h>
#include <initguid.h>
#include <dxgi.h>

#pragma comment(lib, "shlobj.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "uuid.lib")

// Menu ID definitions
#define IDM_ABOUT          1000
#define IDM_TOGGLE_NET     1001
#define IDM_TOGGLE_CPURAM  1002
#define IDM_TOGGLE_GPU     1003
#define IDM_BG_TRANSPARENT 1007
#define IDM_BG_CAPSULE     1008
#define IDM_NET_ALL        1009
#define IDM_EXIT           1010
#define IDM_STARTUP        1011
#define IDM_CPU_UTILITY    1012
#define IDM_CPU_TIME       1013
#define IDM_LOCK_POSITION  1014
#define IDM_RESET_POSITION 1015
#define IDM_NET_UNIT_BITS  1016
#define IDM_NET_UNIT_BYTES 1017
#define IDM_GPU_ALL        1018
#define IDM_MOVE_NAVTASK   1019

#define WM_TRAYICON (WM_USER + 1)

HHOOK g_hHookMessageBox;
LRESULT CALLBACK CBTMessageBoxProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HCBT_ACTIVATE) {
        HWND hwndBox = (HWND)wParam;
        RECT rcScreen, rcWindow;
        SystemParametersInfo(SPI_GETWORKAREA, 0, &rcScreen, 0);
        GetWindowRect(hwndBox, &rcWindow);
        int w = rcWindow.right - rcWindow.left;
        int h = rcWindow.bottom - rcWindow.top;
        int x = rcScreen.left + (rcScreen.right - rcScreen.left - w) / 2;
        int y = rcScreen.top + (rcScreen.bottom - rcScreen.top - h) / 2;
        SetWindowPos(hwndBox, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        UnhookWindowsHookEx(g_hHookMessageBox);
    }
    return CallNextHookEx(g_hHookMessageBox, nCode, wParam, lParam);
}

#define IDM_NET_IF_BASE    2000 // IDs 2000 to 2050 for physical network adapters
#define IDM_GPU_BASE       3000 // IDs 3000 to 3020 for physical GPU adapters

// NVML Driver Dynamic Types for instantaneous zero-overhead GPU Usage & GPU Temp telemetry
typedef struct {
    unsigned int gpu;
    unsigned int memory;
} NVML_UTILIZATION_RATES;
typedef int (*PFN_nvmlInit)(void);
typedef int (*PFN_nvmlShutdown)(void);
typedef int (*PFN_nvmlDeviceGetHandleByIndex)(unsigned int index, void** device);
typedef int (*PFN_nvmlDeviceGetTemperature)(void* device, int sensorType, unsigned int* temp);
typedef int (*PFN_nvmlDeviceGetUtilizationRates)(void* device, NVML_UTILIZATION_RATES* utilization);

static HMODULE g_hNvml = NULL;
static PFN_nvmlInit p_nvmlInit = NULL;
static PFN_nvmlShutdown p_nvmlShutdown = NULL;
static PFN_nvmlDeviceGetHandleByIndex p_nvmlDeviceGetHandleByIndex = NULL;
static PFN_nvmlDeviceGetTemperature p_nvmlDeviceGetTemperature = NULL;
static PFN_nvmlDeviceGetUtilizationRates p_nvmlDeviceGetUtilizationRates = NULL;
static void* g_nvmlDevice = NULL;

// AMD ADL Driver Dynamic Types for instantaneous zero-overhead GPU Temp telemetry
typedef void* (__stdcall *ADL_MAIN_MALLOC_CALLBACK)(int);
typedef int (*PFN_ADL_Main_Control_Create)(ADL_MAIN_MALLOC_CALLBACK, int);
typedef int (*PFN_ADL_Main_Control_Destroy)(void);
typedef struct { int iSize; int iTemperature; } ADLTemperature;
typedef int (*PFN_ADL_Overdrive5_Temperature_Get)(int iAdapterIndex, int iThermalControllerIndex, ADLTemperature *lpTemperature);

static HMODULE g_hAdl = NULL;
static PFN_ADL_Main_Control_Create p_ADL_Main_Control_Create = NULL;
static PFN_ADL_Main_Control_Destroy p_ADL_Main_Control_Destroy = NULL;
static PFN_ADL_Overdrive5_Temperature_Get p_ADL_Overdrive5_Temperature_Get = NULL;
static void* __stdcall ADL_Main_Memory_Alloc(int iSize) { return malloc(iSize); }
static BOOL g_isAmd = FALSE;

// Intel IGCL Driver Dynamic Types (Intel Arc / Iris Xe)
typedef int (*PFN_ctlInit)(void*, void**);
static HMODULE g_hIgcl = NULL;
static BOOL g_isIntel = FALSE;

// Global configuration and state
BOOL g_showNet = TRUE;
BOOL g_showCpuRam = TRUE;
BOOL g_showGpu = TRUE;
int g_bgMode = 1;  // 0 = True Per-Pixel Transparent, 1 = Dark Capsule (Default)
int g_cpuMetric = 0; // 0 = % Processor Utility (Win 10 / Win 11 22H2 standard), 1 = % Processor Time (Win 11 24H2+)
int g_netUnit = 0; // 0 = Bits/s (Kbps/Mbps - Task Manager style), 1 = Bytes/s (KB/s/MB/s - Download style)
DWORD g_netInterfaceIndex = (DWORD)-1; // -1 = All operational interfaces
wchar_t g_netInterfaceDesc[128] = L"ALL"; // Description string of selected physical network adapter
DWORD g_gpuAdapterIndex = (DWORD)-1; // -1 = All Hardware GPUs / Automatic
wchar_t g_gpuAdapterDesc[128] = L"ALL";
DWORD g_gpuAdapterLuidHigh = 0;
DWORD g_gpuAdapterLuidLow = 0;
BOOL g_allowExit = FALSE;

void RefreshSelectedGpu() {
    g_gpuAdapterLuidHigh = 0;
    g_gpuAdapterLuidLow = 0;
    
    // Fallback or specific 'All' logic
    if (g_gpuAdapterIndex == (DWORD)-1 || _wcsicmp(g_gpuAdapterDesc, L"ALL") == 0) {
        g_gpuAdapterIndex = (DWORD)-1;
        wcscpy_s(g_gpuAdapterDesc, 128, L"ALL");
        if (p_nvmlDeviceGetHandleByIndex) {
            p_nvmlDeviceGetHandleByIndex(0, &g_nvmlDevice);
        }
        g_isAmd = FALSE;
        g_isIntel = FALSE;
        return;
    }

    IDXGIFactory1* pFactory = NULL;
    if (CreateDXGIFactory1(&IID_IDXGIFactory1, (void**)&pFactory) == S_OK) {
        IDXGIAdapter1* pAdapter = NULL;
        BOOL found = FALSE;
        
        // 1. Try to match EXACT index AND description to avoid duplicate GPU selection bug
        for (UINT i = 0; pFactory->lpVtbl->EnumAdapters1(pFactory, i, &pAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc;
            pAdapter->lpVtbl->GetDesc1(pAdapter, &desc);
            if (i == g_gpuAdapterIndex && wcscmp(desc.Description, g_gpuAdapterDesc) == 0) {
                g_gpuAdapterLuidHigh = desc.AdapterLuid.HighPart;
                g_gpuAdapterLuidLow = desc.AdapterLuid.LowPart;
                found = TRUE;
                pAdapter->lpVtbl->Release(pAdapter);
                break;
            }
            pAdapter->lpVtbl->Release(pAdapter);
        }
        
        // 2. If index doesn't match (e.g. unplugged display shifting indices), fallback to first description match
        if (!found) {
            for (UINT i = 0; pFactory->lpVtbl->EnumAdapters1(pFactory, i, &pAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 desc;
                pAdapter->lpVtbl->GetDesc1(pAdapter, &desc);
                if (wcscmp(desc.Description, g_gpuAdapterDesc) == 0) {
                    g_gpuAdapterIndex = i;
                    g_gpuAdapterLuidHigh = desc.AdapterLuid.HighPart;
                    g_gpuAdapterLuidLow = desc.AdapterLuid.LowPart;
                    found = TRUE;
                    pAdapter->lpVtbl->Release(pAdapter);
                    break;
                }
                pAdapter->lpVtbl->Release(pAdapter);
            }
        }
        pFactory->lpVtbl->Release(pFactory);
        if (!found) {
            g_gpuAdapterIndex = (DWORD)-1;
            wcscpy_s(g_gpuAdapterDesc, 128, L"ALL");
        }
    }

    g_nvmlDevice = NULL;
    g_isAmd = (wcsstr(g_gpuAdapterDesc, L"AMD") != NULL || wcsstr(g_gpuAdapterDesc, L"Radeon") != NULL);
    g_isIntel = (wcsstr(g_gpuAdapterDesc, L"Intel") != NULL || wcsstr(g_gpuAdapterDesc, L"Arc") != NULL);

    if (p_nvmlDeviceGetHandleByIndex && (g_gpuAdapterIndex == (DWORD)-1 || wcsstr(g_gpuAdapterDesc, L"NVIDIA") != NULL || wcsstr(g_gpuAdapterDesc, L"GeForce") != NULL || wcsstr(g_gpuAdapterDesc, L"Quadro") != NULL)) {
        p_nvmlDeviceGetHandleByIndex(0, &g_nvmlDevice); // Note: Simple NVML index 0 fallback, but robust for primary GPU
    }
}

// Taskbar custom position and drag state
BOOL g_lockPosition = FALSE;
BOOL g_customPos = FALSE;
int g_customXOffset = 0;
int g_customYOffset = 0;
BOOL g_isDragging = FALSE;
POINT g_dragStartMouse = {0};
int g_dragStartWinX = 0;
int g_dragStartWinY = 0;
BOOL g_isMoveMode = FALSE;
BOOL g_isMenuOpen = FALSE;

// Metric strings to render
wchar_t g_strUp[64]    = L"\x25B2 0.00KB/s";
wchar_t g_strDown[64]  = L"\x25BC 0.00KB/s";
wchar_t g_strCpu[64]   = L"CPU   0.0%";
wchar_t g_strRam[64]   = L"RAM  0.00G";
wchar_t g_strGpuUse[64]= L"GPU   0.0%";
wchar_t g_strGpuTmp[64]= L"TMP    0\u00B0C";

// Tracking previous network stats
#define MAX_INTERFACES 128
typedef struct {
    DWORD dwIndex;
    DWORD64 inOctets;
    DWORD64 outOctets;
} IfaceState;
IfaceState g_ifaceStates[MAX_INTERFACES];
int g_numIfaceStates = 0;

BOOL g_netInitialized = FALSE;
DWORD g_lastTick = 0;

// Tracking CPU stats fallback
FILETIME g_lastIdleTime = {0};
FILETIME g_lastKernelTime = {0};
FILETIME g_lastUserTime = {0};

// PDH Counters (Exact Task Manager synchronization)
PDH_HQUERY g_pdhQuery = NULL;
PDH_HCOUNTER g_counterCpu = NULL;
PDH_HCOUNTER g_counterGpu = NULL; // Fallback PDH counter for non-NVIDIA DirectX GPUs

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

// Sensor directly on NavTask itself to detect Fullscreen games/overlays or auto-hidden taskbars without multi-monitor false positives
BOOL IsFullscreenOverlayActive(HWND hwndMy) {
    HWND hForeground = GetForegroundWindow();
    if (!hForeground || hForeground == hwndMy || IsIconic(hForeground) || !IsWindowVisible(hForeground)) return FALSE;

    // Get the specific Taskbar hosting NavTask and its Monitor
    HWND hTray = (HWND)GetWindowLongPtr(hwndMy, GWLP_HWNDPARENT);
    if (!hTray || !IsWindow(hTray)) hTray = FindWindowW(L"Shell_TrayWnd", NULL);
    if (hForeground == hTray || IsChild(hTray, hForeground)) return FALSE;

    HMONITOR hMonMy = MonitorFromWindow(hwndMy, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO miMy = {0};
    miMy.cbSize = sizeof(MONITORINFO);
    GetMonitorInfoW(hMonMy, &miMy);

    // Check if the specific Taskbar hosting NavTask is Auto-Hidden and currently retracted off-screen (~2px strip visible)
    if (hTray) {
        RECT rcTray = {0};
        if (GetWindowRect(hTray, &rcTray)) {
            RECT rcIntersect = {0};
            IntersectRect(&rcIntersect, &miMy.rcMonitor, &rcTray);
            int visibleWidth = rcIntersect.right - rcIntersect.left;
            int visibleHeight = rcIntersect.bottom - rcIntersect.top;
            if (visibleWidth <= 20 || visibleHeight <= 20) {
                return TRUE;
            }
        }
    }

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

    // Check if the foreground window is on the same monitor as NavTask!
    // If the active window/game is on another monitor, NavTask must NEVER disappear!
    HMONITOR hMonFg = MonitorFromWindow(hForeground, MONITOR_DEFAULTTONULL);
    if (hMonMy && hMonFg && hMonMy == hMonFg) {
        RECT rcClient = {0};
        if (GetClientRect(hForeground, &rcClient)) {
            POINT ptTopLeft = { rcClient.left, rcClient.top };
            POINT ptBottomRight = { rcClient.right, rcClient.bottom };
            ClientToScreen(hForeground, &ptTopLeft);
            ClientToScreen(hForeground, &ptBottomRight);
            
            // 1. True Fullscreen check (exclusive games, YouTube/Netflix fullscreen) covering the entire monitor
            if (ptTopLeft.x <= miMy.rcMonitor.left &&
                ptTopLeft.y <= miMy.rcMonitor.top &&
                ptBottomRight.x >= miMy.rcMonitor.right &&
                ptBottomRight.y >= miMy.rcMonitor.bottom) {
                return TRUE;
            }
            
            // 2. Sensor directly on NavTask itself: Check if a Windowed/Borderless game is physically obscuring NavTask
            RECT rcMy = {0};
            if (GetWindowRect(hwndMy, &rcMy)) {
                RECT rcClientScreen = { ptTopLeft.x, ptTopLeft.y, ptBottomRight.x, ptBottomRight.y };
                RECT rcIntersect = {0};
                if (IntersectRect(&rcIntersect, &rcMy, &rcClientScreen)) {
                    int overlapHeight = rcIntersect.bottom - rcIntersect.top;
                    int myHeight = rcMy.bottom - rcMy.top;
                    // Standard maximized windows stop at rcWork.bottom (taskbar lip).
                    // We only hide if the foreground window intrudes >10px inside the taskbar area AND covers >50% of NavTask's own height!
                    if (overlapHeight >= (myHeight / 2) && rcClientScreen.bottom > miMy.rcWork.bottom + 10) {
                        return TRUE;
                    }
                }
            }
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
    if (g_netUnit == 0) { // Bits per second (Task Manager synchronization)
        double bitsPerSec = bytesPerSec * 8.0;
        if (bitsPerSec < 1000.0) {
            val = bitsPerSec; unit = L"bps ";
        } else if (bitsPerSec < 1000.0 * 1000.0) {
            val = bitsPerSec / 1000.0; unit = L"Kbps";
        } else if (bitsPerSec < 1000.0 * 1000.0 * 1000.0) {
            val = bitsPerSec / (1000.0 * 1000.0); unit = L"Mbps";
        } else {
            val = bitsPerSec / (1000.0 * 1000.0 * 1000.0); unit = L"Gbps";
        }
    } else { // Bytes per second (Traditional)
        if (bytesPerSec < 1024.0) {
            val = bytesPerSec; unit = L"B/s ";
        } else if (bytesPerSec < 1024.0 * 1024.0) {
            val = bytesPerSec / 1024.0; unit = L"KB/s";
        } else if (bytesPerSec < 1024.0 * 1024.0 * 1024.0) {
            val = bytesPerSec / (1024.0 * 1024.0); unit = L"MB/s";
        } else {
            val = bytesPerSec / (1024.0 * 1024.0 * 1024.0); unit = L"GB/s";
        }
    }

    if (val >= 100.0) {
        swprintf_s(outBuf, bufSize, L"%lc %4.0f%s", symbol, val, unit);
    } else if (val >= 10.0) {
        swprintf_s(outBuf, bufSize, L"%lc %4.1f%s", symbol, val, unit);
    } else {
        swprintf_s(outBuf, bufSize, L"%lc %4.2f%s", symbol, val, unit);
    }
}

void InitGpuMonitor() {
    // 1. Initialize NVIDIA NVML
    if (!g_hNvml) {
        g_hNvml = LoadLibraryW(L"nvml.dll");
        if (!g_hNvml) g_hNvml = LoadLibraryW(L"C:\\Windows\\System32\\nvml.dll");
        if (!g_hNvml) g_hNvml = LoadLibraryW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    }
    if (g_hNvml && !g_nvmlDevice) {
        p_nvmlInit = (PFN_nvmlInit)GetProcAddress(g_hNvml, "nvmlInit_v2");
        if (!p_nvmlInit) p_nvmlInit = (PFN_nvmlInit)GetProcAddress(g_hNvml, "nvmlInit");
        p_nvmlShutdown = (PFN_nvmlShutdown)GetProcAddress(g_hNvml, "nvmlShutdown");
        p_nvmlDeviceGetHandleByIndex = (PFN_nvmlDeviceGetHandleByIndex)GetProcAddress(g_hNvml, "nvmlDeviceGetHandleByIndex_v2");
        if (!p_nvmlDeviceGetHandleByIndex) p_nvmlDeviceGetHandleByIndex = (PFN_nvmlDeviceGetHandleByIndex)GetProcAddress(g_hNvml, "nvmlDeviceGetHandleByIndex");
        p_nvmlDeviceGetTemperature = (PFN_nvmlDeviceGetTemperature)GetProcAddress(g_hNvml, "nvmlDeviceGetTemperature");
        p_nvmlDeviceGetUtilizationRates = (PFN_nvmlDeviceGetUtilizationRates)GetProcAddress(g_hNvml, "nvmlDeviceGetUtilizationRates");

        if (p_nvmlInit && p_nvmlDeviceGetHandleByIndex && p_nvmlDeviceGetTemperature && p_nvmlDeviceGetUtilizationRates) {
            if (p_nvmlInit() == 0) {
                p_nvmlDeviceGetHandleByIndex(0, &g_nvmlDevice);
            }
        }
    }

    // 2. Initialize AMD ADL
    if (!g_hAdl) {
        g_hAdl = LoadLibraryW(L"atiadlxx.dll");
        if (!g_hAdl) g_hAdl = LoadLibraryW(L"atiadlxy.dll");
        if (g_hAdl) {
            p_ADL_Main_Control_Create = (PFN_ADL_Main_Control_Create)GetProcAddress(g_hAdl, "ADL_Main_Control_Create");
            p_ADL_Main_Control_Destroy = (PFN_ADL_Main_Control_Destroy)GetProcAddress(g_hAdl, "ADL_Main_Control_Destroy");
            p_ADL_Overdrive5_Temperature_Get = (PFN_ADL_Overdrive5_Temperature_Get)GetProcAddress(g_hAdl, "ADL_Overdrive5_Temperature_Get");
            if (p_ADL_Main_Control_Create && p_ADL_Overdrive5_Temperature_Get) {
                p_ADL_Main_Control_Create(ADL_Main_Memory_Alloc, 1);
            }
        }
    }

    // 3. Initialize Intel IGCL (ControlAPI.dll / igcl.dll)
    if (!g_hIgcl) {
        g_hIgcl = LoadLibraryW(L"ControlAPI.dll");
        if (!g_hIgcl) g_hIgcl = LoadLibraryW(L"igcl.dll");
        // We load it, but fetching precise temp requires complex structs. Will use stub fallback or PDH thermal.
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
        PdhAddEnglishCounterW(g_pdhQuery, L"\\GPU Engine(*)\\Utilization Percentage", 0, &g_counterGpu);
        PdhCollectQueryData(g_pdhQuery);
    }

    InitGpuMonitor();

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
            double maxDownSpeed = 0.0;
            double maxUpSpeed = 0.0;
            double totalDownSpeed = 0.0;
            double totalUpSpeed = 0.0;
            BOOL interfaceFound = FALSE;

            for (DWORD i = 0; i < pIfTable->dwNumEntries; i++) {
                if (pIfTable->table[i].dwType != MIB_IF_TYPE_LOOPBACK) {
                    const char* desc = (const char*)pIfTable->table[i].bDescr;
                    if (strstr(desc, "Miniport") || strstr(desc, "miniport") || strstr(desc, "Loopback") || 
                        strstr(desc, "Virtual") || strstr(desc, "virtual") || strstr(desc, "Teredo") || 
                        strstr(desc, "Pseudo") || strstr(desc, "Kernel") || strstr(desc, "Filter") || 
                        strstr(desc, "ISATAP") || pIfTable->table[i].dwPhysAddrLen != 6) {
                        continue;
                    }
                    if (g_netInterfaceIndex == (DWORD)-1 || pIfTable->table[i].dwIndex == g_netInterfaceIndex) {
                        if (pIfTable->table[i].dwOperStatus == MIB_IF_OPER_STATUS_OPERATIONAL) {
                            interfaceFound = TRUE;
                            DWORD64 curIn = pIfTable->table[i].dwInOctets;
                            DWORD64 curOut = pIfTable->table[i].dwOutOctets;

                            DWORD64 prevIn = curIn;
                            DWORD64 prevOut = curOut;
                            BOOL foundPrev = FALSE;
                            for (int j = 0; j < g_numIfaceStates; j++) {
                                if (g_ifaceStates[j].dwIndex == pIfTable->table[i].dwIndex) {
                                    prevIn = g_ifaceStates[j].inOctets;
                                    prevOut = g_ifaceStates[j].outOctets;
                                    g_ifaceStates[j].inOctets = curIn;
                                    g_ifaceStates[j].outOctets = curOut;
                                    foundPrev = TRUE;
                                    break;
                                }
                            }
                            if (!foundPrev && g_numIfaceStates < MAX_INTERFACES) {
                                g_ifaceStates[g_numIfaceStates].dwIndex = pIfTable->table[i].dwIndex;
                                g_ifaceStates[g_numIfaceStates].inOctets = curIn;
                                g_ifaceStates[g_numIfaceStates].outOctets = curOut;
                                g_numIfaceStates++;
                            }

                            if (g_netInitialized && foundPrev) {
                                double down = (curIn >= prevIn) ? (curIn - prevIn) / dt : ((0x100000000ULL - prevIn + curIn) / dt);
                                double up = (curOut >= prevOut) ? (curOut - prevOut) / dt : ((0x100000000ULL - prevOut + curOut) / dt);
                                if (down > maxDownSpeed) maxDownSpeed = down;
                                if (up > maxUpSpeed) maxUpSpeed = up;
                                totalDownSpeed += down;
                                totalUpSpeed += up;
                            }
                        }
                    }
                }
            }
            if (g_netInitialized && interfaceFound) {
                double finalDown = (g_netInterfaceIndex == (DWORD)-1) ? maxDownSpeed : totalDownSpeed;
                double finalUp = (g_netInterfaceIndex == (DWORD)-1) ? maxUpSpeed : totalUpSpeed;
                FormatNetSpeed(finalUp, g_strUp, 64, L'\x25B2');
                FormatNetSpeed(finalDown, g_strDown, 64, L'\x25BC');
            } else if (!interfaceFound && g_netInterfaceIndex != (DWORD)-1) {
                FormatNetSpeed(0.0, g_strUp, 64, L'\x25B2');
                FormatNetSpeed(0.0, g_strDown, 64, L'\x25BC');
            }
            if (!g_netInitialized) g_netInitialized = TRUE;
        }
        free(pIfTable);
    }

    // 2. CPU, RAM & GPU Usage + Temperature (Synchronized with NVIDIA Drivers & Task Manager)
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
        }
    }

    // 2. CPU, RAM & GPU Usage + Temperature (Synchronized with Drivers & Task Manager)
    BOOL gotNvml = FALSE;
    BOOL gotTemp = FALSE;
    if (!g_hNvml && !g_hAdl && !g_hIgcl) {
        InitGpuMonitor();
    }
    
    // Attempt NVIDIA NVML Temperature & Usage
    if (g_nvmlDevice && p_nvmlDeviceGetUtilizationRates && p_nvmlDeviceGetTemperature) {
        NVML_UTILIZATION_RATES rates;
        unsigned int temp = 0;
        if (p_nvmlDeviceGetUtilizationRates(g_nvmlDevice, &rates) == 0) {
            swprintf_s(g_strGpuUse, 64, L"GPU %4.1f%%", (double)rates.gpu);
            gotNvml = TRUE;
        }
        if (p_nvmlDeviceGetTemperature(g_nvmlDevice, 0, &temp) == 0) {
            swprintf_s(g_strGpuTmp, 64, L"TMP %3u\u00B0C", temp);
            gotTemp = TRUE;
        }
    }

    // Attempt AMD ADL Temperature
    if (!gotTemp && g_isAmd && g_hAdl && p_ADL_Overdrive5_Temperature_Get) {
        ADLTemperature adlTemp = {0};
        adlTemp.iSize = sizeof(ADLTemperature);
        // We use adapter 0 as a rough fallback if multi-gpu, ideally we'd map DXGI index to ADL index, but 0 is safe for standard setups
        if (p_ADL_Overdrive5_Temperature_Get(0, 0, &adlTemp) == 0) {
            swprintf_s(g_strGpuTmp, 64, L"TMP %3d\u00B0C", adlTemp.iTemperature / 1000);
            gotTemp = TRUE;
        }
    }

    if (!gotTemp) {
        swprintf_s(g_strGpuTmp, 64, L"TMP   N/A");
    }

    // Fallback to PDH for Usage (AMD / Intel / Default)
    if (!gotNvml && g_counterGpu && g_pdhQuery) {
        DWORD bufferSize = 0, itemCount = 0;
        PDH_STATUS status = PdhGetFormattedCounterArrayW(g_counterGpu, PDH_FMT_DOUBLE, &bufferSize, &itemCount, NULL);
        if (status == PDH_MORE_DATA && bufferSize > 0) {
            PDH_FMT_COUNTERVALUE_ITEM_W* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)malloc(bufferSize);
            if (items && PdhGetFormattedCounterArrayW(g_counterGpu, PDH_FMT_DOUBLE, &bufferSize, &itemCount, items) == ERROR_SUCCESS) {
                wchar_t targetLuid[64] = {0};
                if (g_gpuAdapterIndex != (DWORD)-1 && (g_gpuAdapterLuidHigh != 0 || g_gpuAdapterLuidLow != 0)) {
                    swprintf_s(targetLuid, 64, L"luid_0x%08lx_0x%08lx", g_gpuAdapterLuidHigh, g_gpuAdapterLuidLow);
                }
                double totalGpu = 0.0, maxEngine = 0.0;
                for (DWORD i = 0; i < itemCount; i++) {
                    wchar_t lowerName[256];
                    wcscpy_s(lowerName, 256, items[i].szName);
                    _wcslwr_s(lowerName, 256);
                    if (targetLuid[0] != 0 && !wcsstr(lowerName, targetLuid)) {
                        continue; // Exact match for Intel/AMD or multi-GPU selections
                    }
                    double val = items[i].FmtValue.doubleValue;
                    if (val > maxEngine) maxEngine = val;
                    if (wcsstr(lowerName, L"engtype_3d") || wcsstr(lowerName, L"engtype_compute")) {
                        totalGpu += val;
                    }
                }
                if (maxEngine > totalGpu) totalGpu = maxEngine;
                if (totalGpu > 100.0) totalGpu = 100.0;
                swprintf_s(g_strGpuUse, 64, L"GPU %4.1f%%", totalGpu);
                if (!gotTemp) swprintf_s(g_strGpuTmp, 64, L"TMP   N/A");
            }
            if (items) free(items);
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
    swprintf_s(buf, 32, L"%d", g_showGpu);
    WritePrivateProfileStringW(L"Settings", L"ShowGpu", buf, path);
    swprintf_s(buf, 32, L"%d", g_bgMode);
    WritePrivateProfileStringW(L"Settings", L"BgMode", buf, path);
    swprintf_s(buf, 32, L"%d", g_cpuMetric);
    WritePrivateProfileStringW(L"Settings", L"CpuMetric", buf, path);
    swprintf_s(buf, 32, L"%d", g_netUnit);
    WritePrivateProfileStringW(L"Settings", L"NetUnit", buf, path);
    swprintf_s(buf, 32, L"%lu", g_netInterfaceIndex);
    WritePrivateProfileStringW(L"Settings", L"NetInterfaceIndex", buf, path);
    WritePrivateProfileStringW(L"Settings", L"NetInterfaceDesc", (g_netInterfaceIndex == (DWORD)-1 || wcslen(g_netInterfaceDesc) == 0) ? L"ALL" : g_netInterfaceDesc, path);
    swprintf_s(buf, 32, L"%lu", g_gpuAdapterIndex);
    WritePrivateProfileStringW(L"Settings", L"GpuAdapterIndex", buf, path);
    WritePrivateProfileStringW(L"Settings", L"GpuAdapterDesc", (g_gpuAdapterIndex == (DWORD)-1 || wcslen(g_gpuAdapterDesc) == 0) ? L"ALL" : g_gpuAdapterDesc, path);
    swprintf_s(buf, 32, L"%d", g_lockPosition);
    WritePrivateProfileStringW(L"Settings", L"LockPosition", buf, path);
    swprintf_s(buf, 32, L"%d", g_customPos);
    WritePrivateProfileStringW(L"Settings", L"CustomPos", buf, path);
    swprintf_s(buf, 32, L"%d", g_customXOffset);
    WritePrivateProfileStringW(L"Settings", L"CustomXOffset", buf, path);
    swprintf_s(buf, 32, L"%d", g_customYOffset);
    WritePrivateProfileStringW(L"Settings", L"CustomYOffset", buf, path);

    // Explicitly flush INI cache to disk immediately to guarantee persistence across reboots
    WritePrivateProfileStringW(NULL, NULL, NULL, path);
}

void LoadConfig() {
    wchar_t path[MAX_PATH];
    GetConfigPath(path, MAX_PATH);

    g_showNet = GetPrivateProfileIntW(L"Settings", L"ShowNet", 1, path);
    g_showCpuRam = GetPrivateProfileIntW(L"Settings", L"ShowCpuRam", 1, path);
    g_showGpu = GetPrivateProfileIntW(L"Settings", L"ShowGpu", 1, path);
    g_bgMode = GetPrivateProfileIntW(L"Settings", L"BgMode", 1, path);
    g_cpuMetric = GetPrivateProfileIntW(L"Settings", L"CpuMetric", 0, path);
    g_netUnit = GetPrivateProfileIntW(L"Settings", L"NetUnit", 0, path);
    g_lockPosition = GetPrivateProfileIntW(L"Settings", L"LockPosition", 0, path);
    g_customPos = GetPrivateProfileIntW(L"Settings", L"CustomPos", 0, path);
    g_customXOffset = GetPrivateProfileIntW(L"Settings", L"CustomXOffset", 0, path);
    g_customYOffset = GetPrivateProfileIntW(L"Settings", L"CustomYOffset", 0, path);

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

    if (GetPrivateProfileStringW(L"Settings", L"GpuAdapterIndex", L"-1", buf, 64, path) > 0) {
        g_gpuAdapterIndex = (DWORD)_wtoi64(buf);
    }
    wchar_t gpuDescBuf[128] = {0};
    GetPrivateProfileStringW(L"Settings", L"GpuAdapterDesc", L"ALL", gpuDescBuf, 128, path);
    wcscpy_s(g_gpuAdapterDesc, 128, gpuDescBuf);
    RefreshSelectedGpu();
}

int CalculateWidth() {
    int w = 6;
    if (g_showNet) w += 80;
    if (g_showCpuRam) w += 78;
    if (g_showGpu) w += 78;
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
        curX += 80;
    }
    if (g_showCpuRam) {
        TextOutW(hdcMem, curX, 1, g_strCpu, (int)wcslen(g_strCpu));
        TextOutW(hdcMem, curX, 17, g_strRam, (int)wcslen(g_strRam));
        curX += 78;
    }
    if (g_showGpu) {
        TextOutW(hdcMem, curX, 1, g_strGpuUse, (int)wcslen(g_strGpuUse));
        TextOutW(hdcMem, curX, 17, g_strGpuTmp, (int)wcslen(g_strGpuTmp));
        curX += 78;
    }

    SelectObject(hdcMem, hOldFont);

    if (g_isMoveMode) {
        HPEN hPen = CreatePen(PS_DOT, 1, RGB(255, 0, 0));
        HPEN hOldPen = (HPEN)SelectObject(hdcMem, hPen);
        HBRUSH hOldBrush = (HBRUSH)SelectObject(hdcMem, GetStockObject(NULL_BRUSH));
        Rectangle(hdcMem, 0, 0, w, h);
        SelectObject(hdcMem, hOldBrush);
        SelectObject(hdcMem, hOldPen);
        DeleteObject(hPen);
    }

    int totalPixels = w * h;
    if (g_bgMode == 0) {
        for (int i = 0; i < totalPixels; i++) {
            DWORD c = pPixels[i];
            BYTE r = (c >> 16) & 0xFF;
            BYTE g = (c >> 8) & 0xFF;
            BYTE b = c & 0xFF;
            BYTE brightness = (BYTE)(((DWORD)r * 299 + (DWORD)g * 587 + (DWORD)b * 114) / 1000);

            if (r == 255 && g == 0 && b == 0) {
                pPixels[i] = 0xFFFF0000; // Preserve red border with full alpha
            } else if (brightness == 0) {
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
            if (r == 255 && g == 0 && b == 0) {
                pPixels[i] = 0xFFFF0000; // Preserve red border with full alpha
            } else if (r == 0 && g == 0 && b == 0) {
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

// Dynamically find the primary or secondary taskbar that contains the specified X position
HWND GetTargetTaskbar(int x, int w, RECT* outTrayRect) {
    HWND hwndTray = FindWindowW(L"Shell_TrayWnd", NULL);
    RECT primaryRect = {0};
    if (hwndTray && GetWindowRect(hwndTray, &primaryRect)) {
        int center = x + (w / 2);
        if (center >= primaryRect.left && center <= primaryRect.right) {
            if (outTrayRect) *outTrayRect = primaryRect;
            return hwndTray;
        }
    }

    // Check all secondary monitor taskbars on multi-monitor desktop setups
    HWND hwndSec = NULL;
    while ((hwndSec = FindWindowExW(NULL, hwndSec, L"Shell_SecondaryTrayWnd", NULL)) != NULL) {
        RECT secRect = {0};
        if (GetWindowRect(hwndSec, &secRect)) {
            int center = x + (w / 2);
            if (center >= secRect.left && center <= secRect.right) {
                if (outTrayRect) *outTrayRect = secRect;
                return hwndSec;
            }
        }
    }

    // Default fallback to primary taskbar if position is uninitialized or out of bounds
    if (hwndTray && GetWindowRect(hwndTray, &primaryRect)) {
        if (outTrayRect) *outTrayRect = primaryRect;
        return hwndTray;
    }

    return NULL;
}

// Function retained with same name for compatibility, but now positions the window freely based on custom coordinates.
void RepositionOnTaskbar(HWND hwnd) {
    if (g_isDragging || g_isMenuOpen) return;

    int w = CalculateWidth();
    int h = 34;
    
    int x = 0;
    int y = 0;

    if (g_customPos) {
        x = g_customXOffset;
        y = g_customYOffset;
    } else {
        // First run fallback: Center of the primary screen and force Move Mode
        RECT workArea = {0};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
        x = workArea.left + ((workArea.right - workArea.left) - w) / 2;
        y = workArea.top + ((workArea.bottom - workArea.top) - h) / 2;
        
        if (!g_isMoveMode) {
            g_isMoveMode = TRUE;
            LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
            SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_TRANSPARENT);
        }
    }

    // Keep it owned by the taskbar on its current screen so the taskbar NEVER covers it!
    RECT trayRect = {0};
    HWND hwndTray = GetTargetTaskbar(x, w, &trayRect);
    if (!hwndTray) hwndTray = FindWindowW(L"Shell_TrayWnd", NULL);
    
    if (hwndTray && GetWindowLongPtr(hwnd, GWLP_HWNDPARENT) != (LONG_PTR)hwndTray) {
        SetWindowLongPtr(hwnd, GWLP_HWNDPARENT, (LONG_PTR)hwndTray);
    }

    // Gentle positioning to avoid breaking Exclusive Fullscreen on any monitor
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
    RenderWindow(hwnd);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            InitStats();
            SetTimer(hwnd, 1, 1000, NULL); // Metric interval: 1s
            
            NOTIFYICONDATAW nid = {0};
            nid.cbSize = sizeof(NOTIFYICONDATAW);
            nid.hWnd = hwnd;
            nid.uID = 1;
            nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            nid.uCallbackMessage = WM_TRAYICON;
            nid.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(1));
            if (!nid.hIcon) nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
            wcscpy_s(nid.szTip, 128, L"NavTask Monitor");
            Shell_NotifyIconW(NIM_ADD, &nid);
            return 0;
        }

        case WM_TIMER:
            if (wParam == 1) {
                UpdateStats();
                if (!IsWindowVisible(hwnd)) {
                    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                }
                if (g_isDragging || g_isMenuOpen) {
                    RenderWindow(hwnd);
                } else {
                    RepositionOnTaskbar(hwnd); 
                }
            } else if (wParam == 2) {
                if (g_isDragging) return 0;
                if (IsFullscreenOverlayActive(hwnd)) {
                    if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
                    return 0;
                } else {
                    if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                    RECT winRect;
                    GetWindowRect(hwnd, &winRect);
                    HWND hwndTray = GetTargetTaskbar(winRect.left, CalculateWidth(), NULL);
                    if (hwndTray && GetWindowLongPtr(hwnd, GWLP_HWNDPARENT) != (LONG_PTR)hwndTray) {
                        SetWindowLongPtr(hwnd, GWLP_HWNDPARENT, (LONG_PTR)hwndTray);
                    }
                    
                    // ONLY force aggressive Z-order toggle if the foreground window is the Taskbar, Start Menu, or Desktop!
                    // This prevents tearing other monitors' games out of Exclusive Fullscreen mode (FSE).
                    HWND hForeground = GetForegroundWindow();
                    BOOL isShell = FALSE;
                    if (hForeground) {
                        wchar_t className[256] = {0};
                        GetClassNameW(hForeground, className, 256);
                        if (wcscmp(className, L"Shell_TrayWnd") == 0 || 
                            wcscmp(className, L"Windows.UI.Core.CoreWindow") == 0 || 
                            wcscmp(className, L"Xaml_WindowedPopupClass") == 0 ||
                            wcscmp(className, L"Progman") == 0 || 
                            wcscmp(className, L"WorkerW") == 0) {
                            isShell = TRUE;
                        }
                    }
                    
                    if (isShell) {
                        // Toggle NOTOPMOST -> TOPMOST to force Windows DWM to raise NavTask above Taskbar and Start Menu XAML overlays!
                        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
                        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
                    } else {
                        // Gentle TOPMOST assertion for normal apps/games. Doesn't trigger DWM band reallocation, saving FSE!
                        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
                    }
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
            break;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            if (g_isMoveMode) {
                POINT pt;
                GetCursorPos(&pt);
                RECT rc;
                GetWindowRect(hwnd, &rc);
                g_isDragging = TRUE;
                g_dragStartMouse = pt;
                g_dragStartWinX = rc.left;
                g_dragStartWinY = rc.top;
                SetCapture(hwnd);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (g_isDragging) {
                POINT pt;
                GetCursorPos(&pt);
                int deltaX = pt.x - g_dragStartMouse.x;
                int deltaY = pt.y - g_dragStartMouse.y;
                int newX = g_dragStartWinX + deltaX;
                int newY = g_dragStartWinY + deltaY;
                int w = CalculateWidth();
                int h = 34;

                // Move freely without Taskbar binding
                SetWindowPos(hwnd, HWND_TOPMOST, newX, newY, w, h, SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
            }
            return 0;
        }

        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED: {
            if (g_isDragging) {
                g_isDragging = FALSE;
                if (msg == WM_LBUTTONUP) ReleaseCapture();

                if (g_isMoveMode) {
                    g_isMoveMode = FALSE;
                    // Restore intangibility!
                    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
                    SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle | WS_EX_TRANSPARENT);
                    RenderWindow(hwnd);
                }

                RECT winRect;
                GetWindowRect(hwnd, &winRect);
                if (winRect.left != g_dragStartWinX || winRect.top != g_dragStartWinY || !g_customPos) {
                    // Store absolute desktop virtual coordinates to persist positions on ANY monitor!
                    g_customXOffset = winRect.left;
                    g_customYOffset = winRect.top;
                    g_customPos = TRUE;
                    SaveConfig();
                    RepositionOnTaskbar(hwnd);
                }
            }
            return 0;
        }

        case WM_TRAYICON: {
            if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
                POINT pt;
                GetCursorPos(&pt);
                int menuY = pt.y;

                HMENU hMenu = CreatePopupMenu();

                HMENU hOptionsMenu = CreatePopupMenu();
                AppendMenuW(hOptionsMenu, MF_STRING, IDM_MOVE_NAVTASK, L"Move NavTask Position (Drag to move, Drop to lock)");
                AppendMenuW(hOptionsMenu, MF_STRING | (g_customPos ? MF_ENABLED : MF_GRAYED), IDM_RESET_POSITION,      L"Move to Default Position");
                AppendMenuW(hOptionsMenu, MF_SEPARATOR, 0, NULL);
                AppendMenuW(hOptionsMenu, MF_STRING | (IsStartupEnabled() ? MF_CHECKED : MF_UNCHECKED), IDM_STARTUP, L"Start automatically with Windows");
                AppendMenuW(hOptionsMenu, MF_SEPARATOR, 0, NULL);
                AppendMenuW(hOptionsMenu, MF_STRING, IDM_EXIT, L"Exit NavTask Monitor");

                AppendMenuW(hMenu, MF_STRING, IDM_ABOUT, L"\u2139 About NavTask Monitor");
                AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hOptionsMenu, L"Options");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

            AppendMenuW(hMenu, MF_STRING | (g_showNet ? MF_CHECKED : MF_UNCHECKED), IDM_TOGGLE_NET, L"NavTask: Network Speed (\u25B2/\u25BC)");
            AppendMenuW(hMenu, MF_STRING | (g_showCpuRam ? MF_CHECKED : MF_UNCHECKED), IDM_TOGGLE_CPURAM, L"NavTask: CPU & RAM Usage");
            AppendMenuW(hMenu, MF_STRING | (g_showGpu ? MF_CHECKED : MF_UNCHECKED), IDM_TOGGLE_GPU, L"NavTask: GPU Usage & Temperature (\u00B0C)");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

            // Network Interfaces Submenu (Filtered strictly to Task Manager physical hardware cards)
            HMENU hNetMenu = CreatePopupMenu();
            AppendMenuW(hNetMenu, MF_STRING | (g_netInterfaceIndex == (DWORD)-1 ? MF_CHECKED : MF_UNCHECKED), IDM_NET_ALL, L"All Interfaces (Automatic)");
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
            
            HMENU hGpuMenu = CreatePopupMenu();
            AppendMenuW(hGpuMenu, MF_STRING | (g_gpuAdapterIndex == (DWORD)-1 ? MF_CHECKED : MF_UNCHECKED), IDM_GPU_ALL, L"Default GPU");
            AppendMenuW(hGpuMenu, MF_SEPARATOR, 0, NULL);

            IDXGIFactory1* pFactory = NULL;
            if (CreateDXGIFactory1(&IID_IDXGIFactory1, (void**)&pFactory) == S_OK) {
                IDXGIAdapter1* pAdapter = NULL;
                LUID seenLuids[20];
                int seenCount = 0;
                for (UINT i = 0; pFactory->lpVtbl->EnumAdapters1(pFactory, i, &pAdapter) != DXGI_ERROR_NOT_FOUND && i < 20; ++i) {
                    DXGI_ADAPTER_DESC1 desc;
                    pAdapter->lpVtbl->GetDesc1(pAdapter, &desc);
                    
                    BOOL duplicate = FALSE;
                    for (int j = 0; j < seenCount; j++) {
                        if (seenLuids[j].LowPart == desc.AdapterLuid.LowPart && seenLuids[j].HighPart == desc.AdapterLuid.HighPart) {
                            duplicate = TRUE;
                            break;
                        }
                    }

                    if (!duplicate && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && wcsstr(desc.Description, L"Basic Render Driver") == NULL) {
                        seenLuids[seenCount++] = desc.AdapterLuid;
                        wchar_t wGpuItem[256];
                        swprintf_s(wGpuItem, 256, L"GPU %u: %s", i, desc.Description);
                        BOOL checked = (g_gpuAdapterIndex == i);
                        AppendMenuW(hGpuMenu, MF_STRING | (checked ? MF_CHECKED : MF_UNCHECKED), IDM_GPU_BASE + i, wGpuItem);
                    }
                    pAdapter->lpVtbl->Release(pAdapter);
                }
                pFactory->lpVtbl->Release(pFactory);
            }
            AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hGpuMenu, L"Select GPU Adapter");

            HMENU hNetUnitMenu = CreatePopupMenu();
            AppendMenuW(hNetUnitMenu, MF_STRING | (g_netUnit == 0 ? MF_CHECKED : MF_UNCHECKED), IDM_NET_UNIT_BITS,  L"Bits per second (Kbps / Mbps - Task Manager Sync)");
            AppendMenuW(hNetUnitMenu, MF_STRING | (g_netUnit == 1 ? MF_CHECKED : MF_UNCHECKED), IDM_NET_UNIT_BYTES, L"Bytes per second (KB/s / MB/s - Download Style)");
            AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hNetUnitMenu, L"Network Speed Unit");
            
            HMENU hCpuMenu = CreatePopupMenu();
            AppendMenuW(hCpuMenu, MF_STRING | (g_cpuMetric == 0 ? MF_CHECKED : MF_UNCHECKED), IDM_CPU_UTILITY, L"Utility Mode (% Processor Utility - Win 11 22H2 / 23H2)");
            AppendMenuW(hCpuMenu, MF_STRING | (g_cpuMetric == 1 ? MF_CHECKED : MF_UNCHECKED), IDM_CPU_TIME,    L"Pure Time Mode (% Processor Time - Win 11 24H2 & Later)");
            AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hCpuMenu, L"CPU Calculation Mode");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

            AppendMenuW(hMenu, MF_STRING | (g_bgMode == 0 ? MF_CHECKED : MF_UNCHECKED), IDM_BG_TRANSPARENT, L"Background: True Transparent (Per-Pixel Alpha)");
            AppendMenuW(hMenu, MF_STRING | (g_bgMode == 1 ? MF_CHECKED : MF_UNCHECKED), IDM_BG_CAPSULE,    L"Background: Dark Capsule (Dark Mode)");

            SetForegroundWindow(hwnd);
            g_isMenuOpen = TRUE;
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, menuY, 0, hwnd, NULL);
            g_isMenuOpen = FALSE;
            PostMessage(hwnd, WM_NULL, 0, 0); // Clear menu focus
            DestroyMenu(hMenu);
            }
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDM_ABOUT) {
                g_hHookMessageBox = SetWindowsHookEx(WH_CBT, CBTMessageBoxProc, NULL, GetCurrentThreadId());
                MessageBoxW(NULL, 
                    L"NavTask Monitor v10.4.1 for Windows 11\n\n"
                    L"Developed by: Mauro Carvalho\n"
                    L"Contact / Support: mauroroberto83@gmail.com\n"
                    L"License: Open-Source (Freeware / MIT)\n\n"
                    L"Key Features:\n"
                    L"\x2022 Lightweight Taskbar Telemetry (< 25MB RAM)\n"
                    L"\x2022 Native Multi-GPU Support (NVIDIA NVML & Windows DXGI/PDH)\n"
                    L"\x2022 Synchronized Network, CPU, RAM & GPU metrics with Task Manager\n"
                    L"\x2022 Customizable Taskbar Positioning & Drag-and-Lock", 
                    L"About NavTask Monitor", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
                return 0;
            } else if (id == IDM_TOGGLE_NET) {
                g_showNet = !g_showNet;
            } else if (id == IDM_TOGGLE_CPURAM) {
                g_showCpuRam = !g_showCpuRam;
            } else if (id == IDM_TOGGLE_GPU) {
                g_showGpu = !g_showGpu;
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
            } else if (id == IDM_NET_UNIT_BITS) {
                g_netUnit = 0;
            } else if (id == IDM_NET_UNIT_BYTES) {
                g_netUnit = 1;
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
            } else if (id == IDM_GPU_ALL) {
                // Modified behavior: IDM_GPU_ALL now acts as "Default GPU" which defaults to Task Manager GPU 0
                IDXGIFactory1* pFactory = NULL;
                if (CreateDXGIFactory1(&IID_IDXGIFactory1, (void**)&pFactory) == S_OK) {
                    IDXGIAdapter1* pAdapter = NULL;
                    if (pFactory->lpVtbl->EnumAdapters1(pFactory, 0, &pAdapter) == S_OK) {
                        DXGI_ADAPTER_DESC1 desc;
                        pAdapter->lpVtbl->GetDesc1(pAdapter, &desc);
                        g_gpuAdapterIndex = 0;
                        wcscpy_s(g_gpuAdapterDesc, 128, desc.Description);
                        pAdapter->lpVtbl->Release(pAdapter);
                    } else {
                        // Fallback if no GPU 0
                        g_gpuAdapterIndex = (DWORD)-1;
                        wcscpy_s(g_gpuAdapterDesc, 128, L"ALL");
                    }
                    pFactory->lpVtbl->Release(pFactory);
                }
                RefreshSelectedGpu();

            } else if (id >= IDM_GPU_BASE && id < IDM_GPU_BASE + 20) {
                DWORD idx = id - IDM_GPU_BASE;
                IDXGIFactory1* pFactory = NULL;
                if (CreateDXGIFactory1(&IID_IDXGIFactory1, (void**)&pFactory) == S_OK) {
                    IDXGIAdapter1* pAdapter = NULL;
                    if (pFactory->lpVtbl->EnumAdapters1(pFactory, idx, &pAdapter) == S_OK) {
                        DXGI_ADAPTER_DESC1 desc;
                        pAdapter->lpVtbl->GetDesc1(pAdapter, &desc);
                        g_gpuAdapterIndex = idx;
                        wcscpy_s(g_gpuAdapterDesc, 128, desc.Description);
                        pAdapter->lpVtbl->Release(pAdapter);
                        RefreshSelectedGpu();
                    }
                    pFactory->lpVtbl->Release(pFactory);
                }
            } else if (id == IDM_STARTUP) {
                BOOL newState = !IsStartupEnabled();
                SetStartup(newState);
                wchar_t cfgPath[MAX_PATH];
                GetConfigPath(cfgPath, MAX_PATH);
                WritePrivateProfileStringW(L"Settings", L"StartupInitialized", L"1", cfgPath);
                WritePrivateProfileStringW(NULL, NULL, NULL, cfgPath);
                return 0;
            } else if (id == IDM_MOVE_NAVTASK) {
                g_isMoveMode = TRUE;
                LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
                SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_TRANSPARENT);
                RenderWindow(hwnd);
                return 0;
            } else if (id == IDM_RESET_POSITION) {
                g_customPos = FALSE;
                g_customXOffset = 0;
            } else if (id == IDM_EXIT) {
                DestroyWindow(hwnd);
                return 0;
            }
            SaveConfig();
            RepositionOnTaskbar(hwnd);
            return 0;
        }

        case WM_DESTROY: {
            NOTIFYICONDATAW nid = {0};
            nid.cbSize = sizeof(NOTIFYICONDATAW);
            nid.hWnd = hwnd;
            nid.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &nid);

            if (g_pdhQuery) PdhCloseQuery(g_pdhQuery);
            if (g_hFont) DeleteObject(g_hFont);
            PostQuitMessage(0);
            return 0;
        }
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
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
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
