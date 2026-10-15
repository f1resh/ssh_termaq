<#
.SYNOPSIS
  Builds SSHTermaq (web UI + native Release) and assembles a distributable folder and zip.

.DESCRIPTION
  Starts by showing the version (set in CMakeLists.txt) and the build id, and waits for Enter so the version can
  be changed first. -NoPrompt skips the wait; so does running without a console (input redirected, e.g. CI).

.EXAMPLE
  ./scripts/build.ps1            # UI + native Release + dist/SSHTermaq-<ver>-win-x64 (+ .zip)
  ./scripts/build.ps1 -SkipUi    # reuse ui/dist
  ./scripts/build.ps1 -Portable  # also drop portable.flag so the app keeps its data next to the exe
  ./scripts/build.ps1 -Tests     # run the native and UI test suites first
  ./scripts/build.ps1 -Installer # also build dist/SSHTermaq-<ver>-setup.exe (Inno Setup 6)
  ./scripts/build.ps1 -Sign      # sign SSHTermaq.exe, ssh.dll and the installer (scripts/sign.ps1)
  ./scripts/build.ps1 -NoPrompt  # do not wait for Enter at the version check
#>
param(
    [switch]$SkipUi,
    [switch]$Portable,
    [switch]$Tests,
    [switch]$NoZip,
    [switch]$Installer,
    [switch]$Sign,
    [switch]$NoPrompt
)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
Set-Location $root

$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) { $cmake = (Get-Command cmake -ErrorAction Stop).Source }

# ---- version --------------------------------------------------------------------------------------
# The version is set only in CMakeLists.txt; the build number (commit count) and the commit come from git, as in
# cmake/BuildInfo.cmake, which stamps them into the executable.
function Get-BuildId {
    $ErrorActionPreference = 'Continue'
    $m = Select-String -Path CMakeLists.txt -Pattern 'project\(SSHTermaq VERSION ([0-9]+\.[0-9]+\.[0-9]+)[ )]'
    if (-not $m) { throw 'CMakeLists.txt: project(SSHTermaq VERSION x.y.z ...) not found' }
    $id = [ordered]@{ Version = $m.Matches[0].Groups[1].Value; Number = 0; Commit = 'unknown'; Dirty = $false }
    if (Get-Command git -ErrorAction SilentlyContinue) {
        $count = git rev-list --count HEAD 2>$null
        if ($LASTEXITCODE -eq 0) {
            $id.Number = [int]$count
            $id.Commit = git rev-parse --short=7 HEAD 2>$null
            $id.Dirty = [bool](git --no-optional-locks status --porcelain 2>$null)
            if ($id.Dirty) { $id.Commit += '-dirty' }
        }
    }
    [pscustomobject]$id
}

$id = Get-BuildId
Write-Host "SSHTermaq $($id.Version) (build $($id.Number), $($id.Commit))" -ForegroundColor Cyan
if (-not $NoPrompt -and -not [Console]::IsInputRedirected) {
    Write-Host "The version is set in CMakeLists.txt: project(SSHTermaq VERSION $($id.Version) ...)." -ForegroundColor Yellow
    Write-Host 'If this release needs a new version, change it there now (x.y.z) and save the file.' -ForegroundColor Yellow
    if ($id.Dirty) { Write-Host 'There are uncommitted changes: the build will be marked "-dirty".' -ForegroundColor Yellow }
    Read-Host 'Press Enter to build, or Ctrl+C to stop' | Out-Null
    $id = Get-BuildId   # the version may just have been changed
    Write-Host "Building SSHTermaq $($id.Version) (build $($id.Number), $($id.Commit))" -ForegroundColor Cyan
}
$version = $id.Version

if ($Installer) {   # find Inno Setup before the long build, not after it
    $iscc = @((Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source,
              "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
              "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") |
        Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
    if (-not $iscc) { throw 'Inno Setup 6 (ISCC.exe) not found; install it with: winget install JRSoftware.InnoSetup' }
}
$signScript = Join-Path $PSScriptRoot 'sign.ps1'
if ($Sign) { & $signScript -Check }   # configuration and certificate, before the long build

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
& $cmake --preset vs -B build/release "-DST_BUILD_TESTS=$testsFlag" | Out-Host
if ($LASTEXITCODE) { throw 'CMake configure failed' }
& $cmake --build build/release --config Release --target SSHTermaq -- /nologo /v:minimal /m | Out-Host
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
$name = "SSHTermaq-$version-win-x64"
$dist = Join-Path $root "dist/$name"
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force $dist | Out-Null

$bin = 'build/release/Release'
Copy-Item "$bin/SSHTermaq.exe" $dist      # the web UI is embedded in the executable
Get-ChildItem $bin -Filter *.dll | Copy-Item -Destination $dist
if ($Portable) { New-Item -ItemType File (Join-Path $dist 'portable.flag') | Out-Null }
if ($Sign) { & $signScript (Join-Path $dist 'SSHTermaq.exe') @((Get-ChildItem $dist -Filter *.dll).FullName) }

$title = "SSHTermaq $version (build $($id.Number), $($id.Commit))"
@"
$title
$('=' * $title.Length)

Modern tabbed SSH / Telnet / Rlogin / Raw / Serial client for Windows with PuTTY session import.

Run SSHTermaq.exe (keep ssh.dll next to it). No Visual C++ Redistributable or other runtime is needed
except the Microsoft Edge WebView2 Runtime, which is built into Windows 11.
Data is stored in %APPDATA%\SSHTermaq, or next to the program when a file named
"portable.flag" (or a folder named "data") exists in this folder.

PuTTY-compatible command line:  SSHTermaq.exe [-ssh|-telnet|-rlogin|-raw|-serial] [user@]host
  [-P port] [-l user] [-pw password] [-i key.ppk] [-load "session"] [-L/-R/-D spec] [-A] [-X] [-C] ...
"@ | Set-Content -Encoding utf8 (Join-Path $dist 'README.txt')

@"
SSHTermaq includes the following third-party components:

libssh          LGPL-2.1   https://www.libssh.org/          (dynamically linked: ssh.dll)
OpenSSL         Apache-2.0 https://www.openssl.org/         (statically linked into SSHTermaq.exe and ssh.dll)
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

# ---- installer ------------------------------------------------------------------------------------
if ($Installer) {
    $isccArgs = @('/Q', "/DAppVersion=$version", "/DAppFileVersion=$version.$($id.Number)")
    if ($Sign) {
        # Inno Setup signs the uninstaller and setup.exe itself, through sign.ps1 ($q = quote, $f = the file).
        $signCmd = '$q' + (Get-Process -Id $PID).Path + '$q -NoProfile -ExecutionPolicy Bypass -File $q' + $signScript + '$q $f'
        $isccArgs += @('/DSignToolName=sshtermaq', "/Ssshtermaq=$signCmd")
    }
    & $iscc @isccArgs scripts/installer.iss | Out-Host
    if ($LASTEXITCODE) { throw 'Installer build failed' }
    Write-Host "Created $(Join-Path $root "dist/SSHTermaq-$version-setup.exe")" -ForegroundColor Green
}
