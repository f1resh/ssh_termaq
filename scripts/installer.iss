; Inno Setup script for SSH Crawler (compile with Inno Setup 6: ISCC.exe scripts\installer.iss)
; Expects the folder produced by scripts\build.ps1 (dist\SSHCrawler-<version>-win-x64).

#define AppVersion "0.1.0"
#define SourceDir "..\dist\SSHCrawler-" + AppVersion + "-win-x64"

[Setup]
AppId={{6C8B2E0B-6F5E-4C1E-9E3B-3C2E9D3A7F21}
AppName=SSH Crawler
AppVersion={#AppVersion}
AppPublisher=SSH Crawler
DefaultDirName={autopf}\SSH Crawler
DefaultGroupName=SSH Crawler
UninstallDisplayIcon={app}\SSHCrawler.exe
OutputDir=..\dist
OutputBaseFilename=SSHCrawler-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequiredOverridesAllowed=dialog
WizardStyle=modern

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "portable.flag"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{group}\SSH Crawler"; Filename: "{app}\SSHCrawler.exe"
Name: "{autodesktop}\SSH Crawler"; Filename: "{app}\SSHCrawler.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; Flags: unchecked

[Run]
Filename: "{app}\SSHCrawler.exe"; Description: "Launch SSH Crawler"; Flags: nowait postinstall skipifsilent
