; Inno Setup script for SSHTermaq (compile with Inno Setup 6: ISCC.exe scripts\installer.iss)
; Expects the folder produced by scripts\build.ps1 (dist\SSHTermaq-<version>-win-x64).

#define AppVersion "0.1.0"
#define SourceDir "..\dist\SSHTermaq-" + AppVersion + "-win-x64"

[Setup]
AppId={{6C8B2E0B-6F5E-4C1E-9E3B-3C2E9D3A7F21}
AppName=SSHTermaq
AppVersion={#AppVersion}
AppPublisher=SSHTermaq
DefaultDirName={autopf}\SSHTermaq
DefaultGroupName=SSHTermaq
UninstallDisplayIcon={app}\SSHTermaq.exe
OutputDir=..\dist
OutputBaseFilename=SSHTermaq-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequiredOverridesAllowed=dialog
WizardStyle=modern

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "portable.flag"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{group}\SSHTermaq"; Filename: "{app}\SSHTermaq.exe"
Name: "{autodesktop}\SSHTermaq"; Filename: "{app}\SSHTermaq.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; Flags: unchecked

[Run]
Filename: "{app}\SSHTermaq.exe"; Description: "Launch SSHTermaq"; Flags: nowait postinstall skipifsilent
