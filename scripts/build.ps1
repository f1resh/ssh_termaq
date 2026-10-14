<#
.SYNOPSIS
  Builds SSHTerm (web UI + native Release) and assembles a distributable folder and zip.

.EXAMPLE
  ./scripts/build.ps1            # UI + native Release + dist/SSHTerm-<ver>-win-x64 (+ .zip)
  ./scripts/build.ps1 -SkipUi    # reuse ui/dist
  ./scripts/build.ps1 -Portable  # also drop portable.flag so the app keeps its data next to the exe
  ./scripts/build.ps1 -Tests     # run the native and UI test suites first
#>
param(
    [switch]$SkipUi,
    [switch]$Portable,
    [switch]$Tests,
    [switch]$NoZip
)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
Set-Location $root

$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) { $cmake = (Get-Command cmake -ErrorAction Stop).Source }

$version = (Select-String -Path CMakeLists.txt -Pattern 'project\(SSHTerm VERSION ([0-9.]+)').Matches[0].Groups[1].Value
Write-Host "SSHTerm $version" -ForegroundColor Cyan

# ---- UI -------------------------------------------------------------------------------------------
if (-not $SkipUi) {
    Push-Location ui
    if (-not (Test-Path node_modules)) { npm install --no-audit --no-fund | Out-Host }
    if ($Tests) { npm test | Out-Host; if ($LASTEXITCODE) { throw 'UI tests failed' } }
    npm run build | Out-Host
    if ($LASTEXITCODE) { throw 'UI build failed' }
    Pop-Location
}
if (-not (Test-Path ui/dist/index.html)) { throw 'ui/dist is missing (run without -SkipUi)' }

# ---- native ---------------------------------------------------------------------------------------
$testsFlag = if ($Tests) { 'ON' } else { 'OFF' }
& $cmake --preset vs -B build/release "-DSC_BUILD_TESTS=$testsFlag" | Out-Host
if ($LASTEXITCODE) { throw 'CMake configure failed' }
& $cmake --build build/release --config Release --target SSHTerm -- /nologo /v:minimal /m | Out-Host
if ($LASTEXITCODE) { throw 'Native build failed' }
if ($Tests) {
    & $cmake --build build/release --config Release -- /nologo /v:minimal /m | Out-Host
    Get-ChildItem build/release/Release/st_test_*.exe | ForEach-Object {
        Write-Host "Running $($_.Name)"
        & $_.FullName | Select-Object -Last 2 | Out-Host
        if ($LASTEXITCODE) { throw "$($_.Name) failed" }
    }
}

# ---- assemble -------------------------------------------------------------------------------------
$name = "SSHTerm-$version-win-x64"
$dist = Join-Path $root "dist/$name"
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force $dist | Out-Null

$bin = 'build/release/Release'
Copy-Item "$bin/SSHTerm.exe" $dist      # the web UI is embedded in the executable
Get-ChildItem $bin -Filter *.dll | Copy-Item -Destination $dist
if ($Portable) { New-Item -ItemType File (Join-Path $dist 'portable.flag') | Out-Null }

@"
SSHTerm $version
====================

Modern tabbed SSH / Telnet / Rlogin / Raw / Serial client for Windows with PuTTY session import.

Run SSHTerm.exe (keep ssh.dll next to it). No Visual C++ Redistributable or other runtime is needed
except the Microsoft Edge WebView2 Runtime, which is built into Windows 11.
Data is stored in %APPDATA%\SSHTerm, or next to the program when a file named
"portable.flag" (or a folder named "data") exists in this folder.

PuTTY-compatible command line:  SSHTerm.exe [-ssh|-telnet|-rlogin|-raw|-serial] [user@]host
  [-P port] [-l user] [-pw password] [-i key.ppk] [-load "session"] [-L/-R/-D spec] [-A] [-X] [-C] ...
"@ | Set-Content -Encoding utf8 (Join-Path $dist 'README.txt')

@"
SSHTerm includes the following third-party components:

libssh          LGPL-2.1   https://www.libssh.org/          (dynamically linked: ssh.dll)
OpenSSL         Apache-2.0 https://www.openssl.org/         (statically linked into SSHTerm.exe and ssh.dll)
zlib            zlib       https://zlib.net/                (statically linked into ssh.dll)
nlohmann/json   MIT        https://github.com/nlohmann/json
WebView2 SDK    Microsoft  https://aka.ms/webview2          (static WebView2 loader)
WIL             MIT        https://github.com/microsoft/wil
xterm.js        MIT        https://xtermjs.org/
React           MIT        https://react.dev/
zustand         MIT        https://github.com/pmndrs/zustand
lucide          ISC        https://lucide.dev/

The libssh library is used under the terms of the GNU Lesser General Public License 2.1; you may replace
ssh.dll with a compatible build of libssh.
"@ | Set-Content -Encoding utf8 (Join-Path $dist 'THIRD-PARTY-NOTICES.txt')

if (-not $NoZip) {
    $zip = Join-Path $root "dist/$name.zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    Compress-Archive -Path "$dist/*" -DestinationPath $zip
    Write-Host "Created $zip" -ForegroundColor Green
}
Write-Host "Distribution folder: $dist" -ForegroundColor Green
