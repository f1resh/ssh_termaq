; Inno Setup script for SSHTerm (compile with Inno Setup 6: ISCC.exe scripts\installer.iss)
; Expects the folder produced by scripts\build.ps1 (dist\SSHTerm-<version>-win-x64).

#define AppVersion "0.1.0"
#define SourceDir "..\dist\SSHTerm-" + AppVersion + "-win-x64"

[Setup]
AppId={{6C8B2E0B-6F5E-4C1E-9E3B-3C2E9D3A7F21}
AppName=SSHTerm
AppVersion={#AppVersion}
AppPublisher=SSHTerm
DefaultDirName={autopf}\SSHTerm
DefaultGroupName=SSHTerm
UninstallDisplayIcon={app}\SSHTerm.exe
OutputDir=..\dist
OutputBaseFilename=SSHTerm-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequiredOverridesAllowed=dialog
WizardStyle=modern

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "portable.flag"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{group}\SSHTerm"; Filename: "{app}\SSHTerm.exe"
Name: "{autodesktop}\SSHTerm"; Filename: "{app}\SSHTerm.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; Flags: unchecked

[Run]
Filename: "{app}\SSHTerm.exe"; Description: "Launch SSHTerm"; Flags: nowait postinstall skipifsilent
