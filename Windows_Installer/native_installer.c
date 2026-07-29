#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <tchar.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // 1. Check if uninstaller was invoked via "-uninstall" command line parameter
    if (strstr(lpCmdLine, "-uninstall") != NULL || strstr(lpCmdLine, "/u") != NULL) {
        if (MessageBoxW(NULL, L"Are you sure you want to completely uninstall NavTask Monitor from your Windows Taskbar?", 
                        L"Uninstall NavTask - Mauro Carvalho", MB_YESNO | MB_ICONQUESTION) == IDNO) {
            return 0;
        }

        // Kill running process
        WinExec("taskkill /F /IM NavTask.exe /T", SW_HIDE);
        Sleep(600);

        // Remove Run registry key
        HKEY hKey = NULL;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
            RegDeleteValueW(hKey, L"NavTask");
            RegCloseKey(hKey);
        }

        // Remove Uninstall registry entries in Add/Remove Programs (appwiz.cpl)
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
            RegDeleteKeyW(hKey, L"NavTask");
            RegCloseKey(hKey);
        }

        // Remove installed files
        wchar_t appDir[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appDir))) {
            wchar_t cfgPath[MAX_PATH], cfgDir[MAX_PATH];
            swprintf_s(cfgPath, MAX_PATH, L"%s\\NavTask\\navtask.ini", appDir);
            swprintf_s(cfgDir, MAX_PATH, L"%s\\NavTask", appDir);
            DeleteFileW(cfgPath);
            RemoveDirectoryW(cfgDir);

            wcscat_s(appDir, MAX_PATH, L"\\Programs\\NavTask");
            wchar_t exePath[MAX_PATH], iniPath[MAX_PATH], uninstPath[MAX_PATH];
            swprintf_s(exePath, MAX_PATH, L"%s\\NavTask.exe", appDir);
            swprintf_s(iniPath, MAX_PATH, L"%s\\navtask.ini", appDir);
            swprintf_s(uninstPath, MAX_PATH, L"%s\\uninstall.exe", appDir);
            DeleteFileW(exePath);
            DeleteFileW(iniPath);
            DeleteFileW(uninstPath);
            RemoveDirectoryW(appDir);
        }

        MessageBoxW(NULL, L"NavTask Monitor has been successfully uninstalled from your PC.\nThank you for using our open-source software!", 
                    L"Uninstallation Complete", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    // 2. Standard Installation Wizard Flow
    int resp = MessageBoxW(NULL, 
        L"Welcome to NavTask Monitor v10.4 Setup for Windows 11 & 10!\n\n"
        L"Developer: Mauro Carvalho (mauroroberto83@gmail.com)\n"
        L"License: 100% Freeware (Open-Source)\n\n"
        L"Once installed, NavTask Monitor becomes an ultra-lightweight telemetry fixture on your Windows Taskbar that wakes up automatically with your computer.\n\n"
        L"\x2022 Zero desktop clutter or shortcuts\n"
        L"\x2022 To remove at any time, simply uninstall via Windows Add/Remove Programs (appwiz.cpl).\n\n"
        L"Click YES to install NavTask Monitor now.", 
        L"NavTask Monitor Setup - by Mauro Carvalho", MB_YESNO | MB_ICONINFORMATION);

    if (resp == IDNO) {
        return 0;
    }

    // Prepare destination directory: %LOCALAPPDATA%\Programs\NavTask
    wchar_t appDir[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appDir))) {
        MessageBoxW(NULL, L"Error locating user local app data directory.", L"Setup Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    wcscat_s(appDir, MAX_PATH, L"\\Programs");
    CreateDirectoryW(appDir, NULL);
    wcscat_s(appDir, MAX_PATH, L"\\NavTask");
    CreateDirectoryW(appDir, NULL);

    wchar_t destExe[MAX_PATH];
    swprintf_s(destExe, MAX_PATH, L"%s\\NavTask.exe", appDir);

    // Locate source NavTask binary
    wchar_t currentDir[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, currentDir);

    wchar_t srcExe[MAX_PATH];
    swprintf_s(srcExe, MAX_PATH, L"%s\\..\\Release\\NavTask_Portable_v10.4.exe", currentDir);
    if (GetFileAttributesW(srcExe) == INVALID_FILE_ATTRIBUTES) {
        swprintf_s(srcExe, MAX_PATH, L"%s\\..\\NavTask.exe", currentDir);
        if (GetFileAttributesW(srcExe) == INVALID_FILE_ATTRIBUTES) {
            swprintf_s(srcExe, MAX_PATH, L"%s\\NavTask.exe", currentDir);
        }
    }

    // Kill existing process before overwriting
    WinExec("taskkill /F /IM NavTask.exe /T", SW_HIDE);
    Sleep(500);

    if (!CopyFileW(srcExe, destExe, FALSE)) {
        CopyFileW(L".\\Release\\NavTask_Portable_v10.4.exe", destExe, FALSE);
    }

    if (GetFileAttributesW(destExe) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(NULL, L"Could not copy NavTask executable to target installation directory.\nPlease ensure NavTask_Portable_v10.4.exe exists in the Release folder.", 
                    L"Installation Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Register Windows Autostart Run Key (Permanent automatic wake up)
    HKEY hKey = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        wchar_t quotedPath[MAX_PATH + 4];
        swprintf_s(quotedPath, MAX_PATH + 4, L"\"%s\"", destExe);
        RegSetValueExW(hKey, L"NavTask", 0, REG_SZ, (const BYTE*)quotedPath, (DWORD)((wcslen(quotedPath) + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }

    // Register in Windows "Add/Remove Programs" (appwiz.cpl) with embedded icon
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\NavTask", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"DisplayName", 0, REG_SZ, (const BYTE*)L"NavTask Monitor (by Mauro Carvalho)", (DWORD)(wcslen(L"NavTask Monitor (by Mauro Carvalho)") + 1) * sizeof(wchar_t));
        RegSetValueExW(hKey, L"DisplayVersion", 0, REG_SZ, (const BYTE*)L"10.3.0.0", (DWORD)(wcslen(L"10.3.0.0") + 1) * sizeof(wchar_t));
        RegSetValueExW(hKey, L"Publisher", 0, REG_SZ, (const BYTE*)L"Mauro Carvalho", (DWORD)(wcslen(L"Mauro Carvalho") + 1) * sizeof(wchar_t));
        RegSetValueExW(hKey, L"DisplayIcon", 0, REG_SZ, (const BYTE*)destExe, (DWORD)(wcslen(destExe) + 1) * sizeof(wchar_t));
        
        wchar_t uninstCmd[MAX_PATH + 32];
        wchar_t mySetupPath[MAX_PATH];
        GetModuleFileNameW(NULL, mySetupPath, MAX_PATH);
        
        wchar_t uninstDest[MAX_PATH];
        swprintf_s(uninstDest, MAX_PATH, L"%s\\uninstall.exe", appDir);
        CopyFileW(mySetupPath, uninstDest, FALSE);

        swprintf_s(uninstCmd, MAX_PATH + 32, L"\"%s\" -uninstall", uninstDest);
        RegSetValueExW(hKey, L"UninstallString", 0, REG_SZ, (const BYTE*)uninstCmd, (DWORD)(wcslen(uninstCmd) + 1) * sizeof(wchar_t));
        RegSetValueExW(hKey, L"URLInfoAbout", 0, REG_SZ, (const BYTE*)L"mailto:mauroroberto83@gmail.com", (DWORD)(wcslen(L"mailto:mauroroberto83@gmail.com") + 1) * sizeof(wchar_t));
        
        DWORD noModify = 1;
        RegSetValueExW(hKey, L"NoModify", 0, REG_DWORD, (const BYTE*)&noModify, sizeof(DWORD));
        RegSetValueExW(hKey, L"NoRepair", 0, REG_DWORD, (const BYTE*)&noModify, sizeof(DWORD));

        RegCloseKey(hKey);
    }

    // Launch Installed Application Immediately
    ShellExecuteW(NULL, L"open", destExe, NULL, appDir, SW_SHOW);

    MessageBoxW(NULL, 
        L"\x2714 NavTask Monitor has been successfully installed to your Taskbar!\n\n"
        L"It is now seamlessly integrated into Windows and will run automatically whenever your computer boots.\n\n"
        L"To uninstall at any time, simply go to Windows Settings -> Add or Remove Programs (appwiz.cpl).", 
        L"Installation Complete - Mauro Carvalho", MB_OK | MB_ICONINFORMATION);

    return 0;
}

