; Inno Setup script for the Arbiter, the same shape as Subspace Edition's:
; per-user install (no UAC), everything from build\Arbiter\ (the exe plus the
; Qt DLLs and plugins windeployqt gathered there).
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppName=Arbiter
AppVersion={#AppVersion}
AppPublisher=WM8Q
AppPublisherURL=https://github.com/avantol/Subspace-Arbiter
AppSupportURL=https://github.com/avantol/Subspace-Arbiter
AppUpdatesURL=https://github.com/avantol/Subspace-Arbiter/releases
DefaultDirName={localappdata}\Programs\Arbiter
DefaultGroupName=Arbiter
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=Output
OutputBaseFilename=Arbiter_{#AppVersion}_win64
Compression=lzma2
SolidCompression=yes
UninstallDisplayIcon={app}\Arbiter.exe
WizardStyle=modern
LicenseFile=..\LICENSE

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon"; GroupDescription: "Additional icons:"
Name: "startup"; Description: "Start the Arbiter when Windows starts (for unattended stations)"; GroupDescription: "Start-up:"; Flags: unchecked

[Files]
Source: "..\build\Arbiter\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Arbiter"; Filename: "{app}\Arbiter.exe"
Name: "{group}\Uninstall Arbiter"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Arbiter"; Filename: "{app}\Arbiter.exe"; Tasks: desktopicon
Name: "{userstartup}\Arbiter"; Filename: "{app}\Arbiter.exe"; Tasks: startup

[Run]
Filename: "{app}\Arbiter.exe"; Description: "Start the Arbiter now"; Flags: nowait postinstall skipifsilent
