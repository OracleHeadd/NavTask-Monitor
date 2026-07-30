# NavTask Monitor for Windows 11

An ultra-lightweight performance telemetry utility engineered specifically for Windows 11 and Windows 10. Built natively in C using Win32 API, DXGI, NVML, and the Windows Performance Data Helper (PDH) kernel engine. Operates with zero external dependency runtimes and a minimal footprint of under 25 MB RAM.

**Good for dual-monitor setups, since NavTask is designed so that it never disappears from the screen.** Even though it is always displayed on your screen, it features a true "click-through" design that doesn't interfere with your workflow, and can be easily moved and positioned anywhere you prefer.

###### Works well with Windows 11 25H2 features or earlier versions. Not tested on small screens. ######

![Windows 11 Compatible](https://img.shields.io/badge/Windows_11-Compatible-0078D4?style=for-the-badge&logo=windows)
![Windows 10 Compatible](https://img.shields.io/badge/Windows_10-Compatible-0078D4?style=for-the-badge&logo=windows)
![C Win32 API](https://img.shields.io/badge/Language-C%20(Win32)-00599C?style=for-the-badge&logo=c)
![RAM Footprint](https://img.shields.io/badge/RAM-%3C_25_MB-00C853?style=for-the-badge)
![License MIT](https://img.shields.io/badge/License-MIT-purple?style=for-the-badge)

---

## Developer & Authorship
- Developer & Creator: Mauro Carvalho
- Support & Feedback: mauroroberto83@gmail.com
- License: Open-Source (MIT License)

---

## Technical Features

### 1. Unobtrusive Taskbar Integration & System Tray Architecture
- Invisible "Click-Through" Architecture: The widget acts as a true transparent overlay. It rests natively on top of the taskbar without blocking mouse interactions, allowing users to seamlessly interact with underlying taskbar items while telemetry remains visible.
- System Tray (Notification Area) Management: All settings and configurations have been migrated to a dedicated system tray icon to ensure the main interface remains purely visual and non-obstructive.
- Visual Move Mode: Positioning customization is triggered via the system tray context menu. This enters a dedicated "Move Mode", highlighting the widget with a dotted bounding box and temporarily intercepting mouse clicks for drag-and-drop alignment across any monitor workspace.
- Centered First-Run Initialization: On initial execution, the widget automatically spawns at the mathematical center of the primary display and immediately engages Move Mode, intuitively prompting the user to drag and lock it into their preferred taskbar location.
- Fail-Safe Monitor Recovery: Engineered with fail-safe logic for multi-monitor environments. If a secondary monitor is disconnected, users can instantly recover the widget using the "Reset Position" option in the system tray, which summons it back to the center of the primary display.

### 2. Multi-GPU Monitoring Engine (NVIDIA / AMD / Intel)
- Incorporates dynamic runtime binding with NVIDIA NVML (NVIDIA Management Library) for instantaneous hardware GPU utilization percentage and core temperature monitoring without driver overhead.
- Implements Windows DXGI (DirectX Graphics Infrastructure) and PDH kernel mapping as an automated hardware fallback and primary driver for AMD Radeon, Intel Arc, and integrated AMD/Intel graphics processors.
- Includes a dedicated multi-GPU selection context menu allowing immediate switching across active physical display adapters (e.g., dedicated discrete GPUs vs integrated processors).
- Synchronizes telemetry rates with Windows Task Manager performance engine measurements.

### 3. Task Manager Synchronized CPU, RAM & Network Telemetry
- Network Interfaces: Automatically filters out virtual WAN miniports, Teredo tunnels, and software loopback devices, displaying only physical Ethernet and Wi-Fi hardware network adapters.
- Network Speed Scaling: Supports instant switching between Bits per second (Kbps / Mbps, aligned with Windows Task Manager) and Bytes per second (KB/s / MB/s, standard transfer cadence).
- Dual CPU Calculation Architectures: Offers real-time switching between Utility Mode (% Processor Utility, matching standard Windows 11 22H2/23H2 Task Manager calculations) and Pure Time Mode (% Processor Time, matching Windows 11 24H2+ and technical instrumentation tools like HWiNFO64 and MSI Afterburner).
- Memory Utilization: Reports real-time operating system committed memory consumption in Gigabytes at a precise 1000ms polling cycle.

### 4. Interface Aesthetics & Rendering Modes
- Dark Capsule Mode (Default): High-contrast rounded dark mode profile optimized for clean readability across both bright and vivid dynamic taskbar wallpapers.
- True Transparent Mode: Leverages native alpha bit masking to fuse digital typography directly onto custom taskbar themes while retaining full mouse click-through functionality and interaction responsiveness.
- Monochromatized Consolas Monospace Typography: Engineered for absolute vertical column symmetry across upper and lower telemetry lines without numeric jitter during rapid utilization spikes.
- Menu Anchor Protection: Utilizes Win32 bottom-alignment bounding protocols to guarantee upward context menu projection from the taskbar without truncation or edge cutoff.

---

### Setup Installer (NavTask_Setup_v10.5.exe)
- Compiled via the modern Inno Setup 6 engine with full WizardStyle implementation.
- Dual Installation Scopes: Offers interactive initial selection between "Install for me only" (deploys to user local profile without Administrator elevation requirements) and "Install for all users" (system-wide deployment to Program Files).
- Registers standard system uninstallation procedures natively within Windows Settings and Add/Remove Programs (appwiz.cpl).
- Configures optional automatic system startup persistence through standard Windows registry run keys during installation.

---

## Building from Source

To compile the application binary and installer directly from source on Windows utilizing Mingw64 / GCC and Inno Setup:

```powershell
# 1. Compile Windows resource binary metadata (author info & copyright)
windres navtask.rc -O coff -o navtask.res

# 2. Compile the standalone portable executable with required DXGI/NVML kernel linking flags
gcc -O2 -mwindows navtask.c navtask.res -o Release/NavTask_Portable_v10.5.exe -liphlpapi -lpdh -lgdi32 -luser32 -lshell32 -ldxgi -luuid -Wall

# 3. Compile the modern setup installation wizard using Inno Setup Compiler
iscc NavTask_Setup.iss
```

---
Created and maintained by Mauro Carvalho under the MIT License.
