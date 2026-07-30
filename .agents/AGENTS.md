
# NavTask Compilation Rules
- To compile the C binaries, use: gcc -O2 -mwindows navtask.c navtask.res -o Release/NavTask_Portable_v10.4.1.exe -liphlpapi -lpdh -lgdi32 -luser32 -lshell32 -ldxgi -luuid -Wall (and also output to NavTask.exe in root).
- The Inno Setup Compiler is located at: "C:\Users\mauro\AppData\Local\Programs\Inno Setup 6\ISCC.exe". Use this full path to compile the installer: & "C:\Users\mauro\AppData\Local\Programs\Inno Setup 6\ISCC.exe" NavTask_Setup.iss
