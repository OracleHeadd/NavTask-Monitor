# NavTask Monitor for Windows 11

An ultra-lightweight performance monitor exclusively designed for Windows 11. Built natively in **C (Win32 API & PDH Kernel Engine)** with **Zero Dependencies**, consuming less than **7 MB of RAM** and practically zero CPU overhead.

![Windows 11 Native](https://img.shields.io/badge/Windows_11-Compatible-0078D4?style=for-the-badge&logo=windows)
![C Win32 API](https://img.shields.io/badge/Language-C%20(Win32)-00599C?style=for-the-badge&logo=c)
![RAM Footprint](https://img.shields.io/badge/RAM-%3C_7_MB-00C853?style=for-the-badge)
![License MIT](https://img.shields.io/badge/License-MIT-purple?style=for-the-badge)

---

##  Developer & Authorship
- **Created by:** [Mauro Carvalho](mailto:mauroroberto83@gmail.com)
- **Email / Support / Feedback:** `mauroroberto83@gmail.com`
- **License:** Open-Source (MIT License)

---

##  Features & Innovation

1. ** Live Windows 11 Weather & Widget Radar:**
   - Adapts seamlessly to modern Windows 11 *XAML Islands*.
   - Reads real-time telemetry from the Windows Registry (`TaskbarDa` & `TaskbarAl`) every second. If you toggle the Weather/Widgets button ON or OFF in Windows Settings, NavTask instantly recognizes the change and slides across the taskbar autonomously to maintain perfect visual balance.

2. ** Exact Task Manager Synchronization (PDH Engine):**
   - **Dual CPU Calculation Mode (NEW):** Features an instant right-click menu selector right under Network Interfaces to switch between **Utility Mode (`% Processor Utility`)** for exact synchronization with Windows 11 (21H2 / 22H2 / 23H2) Task Manager, and **Pure Time Mode (`% Processor Time`)** for modern Windows 11 24H2+ and pro hardware monitors (HWiNFO64 / MSI Afterburner).
   - Monitors real-time Network Traffic (Download ▲ / Upload ▼), Memory Usage (GB), and Physical Disk Activity (Read / Write MB/s) at a sharp 1000ms cadence.

3. ** Hardware Adapter Filtration:**
   - Eliminates useless Windows virtual WAN miniports, Teredo tunnels, and loopbacks from the interface selection menu. Displays only active physical Ethernet and Wi-Fi adapters with valid MAC addresses.

4. ** Per-Pixel Transparent & Dark Capsule Aesthetics:**
   - **True Transparent:** Seamlessly fuses numbers onto your Windows taskbar wallpaper/color with 100% mouse-click responsiveness via custom Alpha=1 bit masking.
   - **Dark Capsule Mode:** Elegant rounded dark mode pill for high contrast against light or vivid taskbars.
   - **Zero Menu Cutoff:** Uses Win32 `TPM_BOTTOMALIGN` technology anchored to the taskbar lip, guaranteeing the context menu always shoots upward cleanly without overlapping or truncation.

---

##  Download & Installation (100% Free)

Visit the **Releases** page to download the package that best suits your computer:

### 1.  Setup Installer (Recommended for standard users)
Download **`NavTask_Setup_v10.3.exe`**
- Interactive Windows installation wizard powered by Inno Setup.
- Installs cleanly to User space (No Admin rights or UAC prompts required).
- Optional automated startup checkmark so NavTask wakes up automatically on computer boot.
- Registers in Windows *Add/Remove Programs* for convenient 1-click uninstallation.

### 2.  Standalone Portable Binary (No install needed)
Download **`NavTask_Portable_v10.3.exe`**
- Zero installation required. Just drop it anywhere in your personal documents or desktop and double-click to run!
- Includes a built-in **"Start with Windows"** switch directly inside the right-click context menu using native Windows Registry autostart integration.

---

###  Note on Windows Defender SmartScreen & Smart App Control
Because NavTask Monitor is an independently distributed free open-source utility and does not employ a paid commercial Authenticode digital certificate ($300+/year), Windows 11 may initially display a protective pop-up when executing new builds:
- **Windows SmartScreen (Blue dialog):** Simply click **"More info" ➔ "Run anyway"**.
- **Smart App Control (Windows Sandbox / Strict Mode):** If Smart App Control blocks execution in high-security regimes or Windows Sandbox, right-click the downloaded `.exe` file ➔ select **Properties** ➔ check the **[ ✓ ] Unblock** checkbox at the very bottom of the General tab, and click **OK**. You can also build directly from source in seconds!

---

##  Building from Source

To compile NavTask natively from scratch on Windows using Mingw64 / GCC:

```powershell
# 1. Compile resource binary metadata (author info & copyright)
windres navtask.rc -O coff -o navtask.res

# 2. Compile and link optimized C binary with Win32/PDH kernel libraries
gcc -O2 -mwindows navtask.c navtask.res -o Release/NavTask_Portable_v10.3.exe -liphlpapi -lpdh -lgdi32 -luser32 -lshell32 -Wall
```

---
*Created and maintained by **Mauro Carvalho** under the MIT License.*
