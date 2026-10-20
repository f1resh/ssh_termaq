; Inno Setup 6 script for SSHTermaq. Built by scripts\windows\build.ps1 -Installer, which passes the version from
; CMakeLists.txt. By hand, after scripts\windows\build.ps1: ISCC.exe /DAppVersion=x.y.z scripts\windows\installer.iss
; Expects the folder produced by scripts\windows\build.ps1 (dist\SSHTermaq-<version>-win-x64).

#ifndef AppVersion
  #error Pass the version from CMakeLists.txt: ISCC.exe /DAppVersion=x.y.z scripts\windows\installer.iss (or use scripts\windows\build.ps1 -Installer)
#endif
#define SourceDir "..\..\dist\SSHTermaq-" + AppVersion + "-win-x64"

[Setup]
AppId={{6C8B2E0B-6F5E-4C1E-9E3B-3C2E9D3A7F21}
AppName=SSHTermaq
AppVersion={#AppVersion}
#ifdef AppFileVersion
VersionInfoVersion={#AppFileVersion}
#endif
#ifdef SignToolName
; Set by scripts\windows\build.ps1 -Sign (ISCC /DSignToolName=... /S<name>=<command>): signs the uninstaller and setup.exe.
SignTool={#SignToolName}
SignedUninstaller=yes
#endif
AppPublisher=SSHTermaq
DefaultDirName={autopf}\SSHTermaq
DefaultGroupName=SSHTermaq
UninstallDisplayIcon={app}\SSHTermaq.exe
OutputDir=..\..\dist
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
