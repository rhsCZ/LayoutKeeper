#define ReleaseExe "output\Win32\Release\LayoutKeeper.exe"

#ifndef AppVersion
  #define AppVersion GetStringFileInfo(SourcePath + ReleaseExe, "ProductVersion")
#endif

[Setup]
AppId={{3BFB0B03-12C5-4E24-B540-5E69F208C989}
AppName=LayoutKeeper
AppVersion={#AppVersion}
AppPublisher=rhsCZ
DefaultDirName={localappdata}\Programs\LayoutKeeper
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x86compatible
AppMutex=Local\LayoutKeeper.Instance
UninstallDisplayName=LayoutKeeper
UninstallDisplayIcon={app}\LayoutKeeper.exe
SetupIconFile=LayoutKeeper.ico
OutputDir=installer-output
OutputBaseFilename=LayoutKeeper-Setup-x86

[Files]
Source: "{#ReleaseExe}"; DestDir: "{app}"; Flags: ignoreversion

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "LayoutKeeper"; ValueData: """{app}\LayoutKeeper.exe"""; Flags: uninsdeletevalue

[Run]
Filename: "{app}\LayoutKeeper.exe"; Description: "{cm:LaunchProgram,LayoutKeeper}"; Flags: nowait postinstall skipifsilent unchecked
