; Official Inno Setup Configuration Script for NavTask Monitor
; Created by: Mauro Carvalho
; Standard Modern Setup Wizard for Windows 10 & 11

#define MyAppName "NavTask Monitor"
#define MyAppVersion "10.3"
#define MyAppPublisher "Mauro Carvalho"
#define MyAppURL "mailto:mauroroberto83@gmail.com"
#define MyAppExeName "NavTask.exe"

[Setup]
; AppId uniquely identifies this software in Windows Registry
AppId={{5D8E4210-9F24-4B3A-8D15-NAVTASK2026}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} v{#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
LicenseFile=..\LICENSE
OutputDir=..\Release
OutputBaseFilename=NavTask_Setup_v10.7
SetupIconFile=..\navtask.ico
UninstallDisplayIcon={app}\navtask.ico
AppMutex=NavTask_SingleInstance_Mutex
CloseApplications=yes
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "startupicon"; Description: "Start NavTask automatically when Windows starts (Recommended)"; GroupDescription: "Windows Boot Option:"

[Files]
Source: "..\Release\NavTask_Portable_v10.7.exe"; DestDir: "{app}"; DestName: "{#MyAppExeName}"; Flags: ignoreversion
Source: "..\navtask.ico"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\navtask.ico"; Comment: "Monitor de Performance da Barra de Tarefas do Windows"
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\navtask.ico"; Comment: "Monitor de Performance da Barra de Tarefas do Windows"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"; IconFilename: "{app}\navtask.ico"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\navtask.ico"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "NavTask"; ValueData: """{app}\{#MyAppExeName}"""; Flags: uninsdeletevalue; Tasks: startupicon
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueName: "NavTask"; Flags: dontcreatekey uninsdeletevalue

[UninstallDelete]
Type: files; Name: "{app}\navtask.ini"
Type: files; Name: "{localappdata}\NavTask\navtask.ini"
Type: filesandordirs; Name: "{localappdata}\NavTask"
Type: filesandordirs; Name: "{app}"
Type: filesandordirs; Name: "{group}"
Type: files; Name: "{autodesktop}\{#MyAppName}.lnk"
Type: files; Name: "{autoprograms}\{#MyAppName}.lnk"
Type: files; Name: "{userstartup}\{#MyAppName}.lnk"

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch NavTask Monitor onto Taskbar now"; Flags: nowait postinstall skipifsilent
