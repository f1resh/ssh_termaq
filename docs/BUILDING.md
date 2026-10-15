# Building SSHTermaq on Windows

A step-by-step setup of the build environment, from a clean Windows 10/11 x64 machine to a running debug build.
For the architecture and module layout see [ARCHITECTURE.md](ARCHITECTURE.md).

## 1. What you need

| Tool | Version | Used for |
|---|---|---|
| Visual Studio 2022 | 17.14+, workload **Desktop development with C++** | MSVC v143 (C++20), Windows SDK, CMake 3.25+, vcpkg |
| vcpkg (ships with VS) | **2025-11-19 or newer** | C++ dependencies (libssh, OpenSSL, zlib, nlohmann-json, WebView2, WIL, Catch2) |
| Node.js | 22 LTS or newer (Vite needs ≥ 20.19 / ≥ 22.12) | the React UI in `ui/` |
| Microsoft Edge WebView2 Runtime | any current | running the app (built into Windows 11) |
| Git | any | cloning; vcpkg uses it; the build number and commit stamped into the app ([section 9](#9-versions-and-build-ids)) |

Optional — the native tests skip the cases that need these when they are missing:

| Tool | Expected at | Used by |
|---|---|---|
| PuTTY 0.83 | `C:\Program Files\PuTTY\` | cross-checks with `plink`, `psftp`, `pageant` |
| Git for Windows | `C:\Program Files\Git\` | SCP tests (its `sh.exe`, GNU `ls`, OpenSSH `scp`) |
| OpenSSH client | `C:\Windows\System32\OpenSSH\` | `ssh-keygen` fingerprint checks |
| Inno Setup 6 | `C:\Program Files (x86)\Inno Setup 6\` or on `PATH` | building the installer (`build.ps1 -Installer`); `winget install JRSoftware.InnoSetup` |

## 2. Install the tools

**Visual Studio.** In the Visual Studio Installer pick *Desktop development with C++* and make sure these
components are ticked (they are by default): *MSVC v143 x64/x86 build tools*, *Windows 11 SDK*,
*C++ CMake tools for Windows* and *vcpkg package manager*. Or from a terminal:

```powershell
winget install Microsoft.VisualStudio.2022.Community --override "--passive --add Microsoft.VisualStudio.Workload.NativeDesktop --add Microsoft.VisualStudio.Component.VC.CMake.Project --add Microsoft.VisualStudio.Component.Vcpkg --includeRecommended"
```

**Node.js** (pick up the new PATH by opening a new terminal afterwards):

```powershell
winget install OpenJS.NodeJS.LTS
```

## 3. Open a developer shell and check versions

Use **Developer PowerShell for VS 2022** (Start menu). It puts `cl`, `cmake`, `ninja` and `vcpkg` on PATH and sets
`VCPKG_ROOT`. All commands below run from the repository root in this shell.

```powershell
vcpkg version     # 2025-11-19 or newer
cmake --version   # 3.25 or newer
node --version    # v22.x or newer
```

If `vcpkg version` is older, update Visual Studio (Visual Studio Installer → *Update*): the port versions pinned in
`vcpkg.json` (`builtin-baseline`) do not build with older vcpkg releases.

## 4. Install the C++ dependencies (once)

```powershell
vcpkg install --triplet x64-windows-st --overlay-triplets=triplets --x-install-root=vcpkg_installed
```

This builds everything from source into `vcpkg_installed\` with the project's own triplet
(`triplets\x64-windows-st.cmake`: static libraries and static C/C++ runtime, libssh as `ssh.dll`). The first run
takes about 7 minutes on a current desktop, 5 of them OpenSSL. Results are cached in `%LOCALAPPDATA%\vcpkg\archives`, so later installs
(a fresh clone, a deleted `vcpkg_installed\`) restore in seconds. CMake does not run vcpkg itself
(`VCPKG_MANIFEST_INSTALL` is off): rerun this command whenever `vcpkg.json` or the triplet changes.

## 5. Build the UI

```powershell
cd ui
npm ci
npm run build      # type-checks, then bundles into ui\dist
cd ..
```

The native build embeds `ui\dist` into `SSHTermaq.exe`, so there is no `ui` folder next to the executable. Build the
UI first, or the app opens with "This copy of SSHTermaq was built without its user interface". After changing the UI,
run `npm run build` and then the native build again; the executable is relinked only when `ui\dist` changed.

## 6. Build and run the native app

**From the command line:**

```powershell
cmake --preset vs                                   # generates build\vs\SSHTermaq.sln
cmake --build build/vs --config Debug --parallel    # or --config Release
build\vs\Debug\SSHTermaq.exe
```

**In Visual Studio:** after `cmake --preset vs`, open `build\vs\SSHTermaq.sln`. In Solution Explorer right-click
**SSHTermaq** → *Set as Startup Project*, pick *Debug* / *x64* and press F5. The working directory is already set to
the executable's folder. Projects are grouped into `modules`, `tests` and `tools`; source files added to
`native/src/<module>/` are picked up the next time the solution regenerates (any build re-runs CMake when needed).

**UI development with hot reload:** run the Vite dev server and point the app at it, so UI edits show up without
rebuilding:

```powershell
cd ui; npm run dev                                                  # http://localhost:5173
build\vs\Debug\SSHTermaq.exe --dev-url http://localhost:5173 --devtools
```

In Visual Studio put `--dev-url http://localhost:5173 --devtools` into SSHTermaq → *Properties* → *Debugging* →
*Command Arguments*. Without the native app, http://localhost:5173 in any browser shows the UI against a mock
backend.

Pass `--data-dir <folder>` to keep test runs away from your real sessions in `%APPDATA%\SSHTermaq`.

## 7. Run the tests

The basics are below; [TESTING.md](TESTING.md) describes how the tests work, lists every test and covers filtering,
debugging in Visual Studio and the end-to-end scripts.

```powershell
ctest --test-dir build/vs -C Debug --output-on-failure   # all 12 native suites (Catch2), about a minute
build\vs\Debug\st_test_ssh.exe "[forward]"               # one suite, filtered by tag or test name
cd ui; npm test                                          # UI unit tests (Vitest)
```

The tests use only localhost: an in-process libssh server (`native/tests/support`) stands in for real hosts.
`build\vs\Debug\st_testserver.exe --port 2222` runs that server on its own for manual testing
(user `test`, password `secret`).

## 8. Release package

```powershell
.\scripts\build.ps1             # UI + native Release → dist\SSHTermaq-<version>-win-x64 and a .zip
.\scripts\build.ps1 -Tests      # run all test suites first
.\scripts\build.ps1 -Installer  # also dist\SSHTermaq-<version>-setup.exe (needs Inno Setup 6)
.\scripts\build.ps1 -Sign       # sign the exe, ssh.dll and the installer (section 10)
.\scripts\build.ps1 -NoPrompt   # skip the version check below (also skipped when input is redirected, e.g. CI)
```

The script first shows the version and build id ([section 9](#9-versions-and-build-ids)) and waits for Enter:

```text
SSHTermaq 1.0.0 (build 10, 35de5d8-dirty)
The version is set in CMakeLists.txt: project(SSHTermaq VERSION 1.0.0 ...).
If this release needs a new version, change it there now (x.y.z) and save the file.
There are uncommitted changes: the build will be marked "-dirty".
Press Enter to build, or Ctrl+C to stop:
```

If you change the version while it waits, it is read again after Enter. Other options: `-SkipUi` reuses `ui\dist`,
`-Portable` adds `portable.flag`, `-NoZip` skips the zip.

The folder holds `SSHTermaq.exe` (with the UI inside) and `ssh.dll`; nothing else is needed on the target machine
except the WebView2 Runtime. The installer script, `scripts\installer.iss`, gets the version from `build.ps1`; to
compile it by hand after `build.ps1`, pass the version yourself: `ISCC.exe /DAppVersion=1.0.0 scripts\installer.iss`.

## 9. Versions and build ids

**The version is set in one place:** the `project(SSHTermaq VERSION x.y.z ...)` line at the top of
[CMakeLists.txt](../CMakeLists.txt). Everything else follows it on the next build: the About page, the log, the
executable's file properties and manifest, the release folder and zip names, and the installer. `ui/package.json` and
`vcpkg.json` have no version.

**To release a new version:** change that line (for example `1.0.0` → `1.1.0`), commit, then run
`.\scripts\build.ps1`. A version change recompiles only `BuildInfo.cpp` and the resources.

**The build id is stamped on every build** from git, by [cmake/BuildInfo.cmake](../cmake/BuildInfo.cmake):

| Part | Example | Where it comes from |
|---|---|---|
| version | `1.0.0` | `CMakeLists.txt` |
| build number | `10` | the number of commits in HEAD (`git rev-list --count HEAD`); the 4th part of the file version, `1.0.0.10` |
| commit | `35de5d8`, `35de5d8-dirty` | short hash of HEAD; `-dirty` when the working tree has changes not committed yet (untracked files included) |
| build time | `2026-10-15 13:49:30 UTC` | the link time of the executable, read from its PE header, so every rebuild shows a new one |

Without git (a source zip, or git not installed) the build number is `0` and the commit `unknown`. The build number
counts the commits of the current branch: a release built from a clean commit gets the same id every time, but two
branches can share a number, and a shallow CI clone needs the full history (`fetch-depth: 0`) to count it.

The id is written to `build\<preset>\generated\st_build_info.h`, which only changes when the version, the commit or
the "dirty" state does, so a build without changes does not recompile or relink anything.

**Where to see it:**

| Where | What it shows |
|---|---|
| App menu › *About SSHTermaq*, and Settings › About | `Version 1.0.0 (build 10)`, *Build* `10 (35de5d8-dirty)`, *Built* in local time |
| `SSHTermaq.exe --version` (or `-V`, as in PuTTY) | a message box: `SSHTermaq 1.0.0 (build 10, 35de5d8-dirty, 2026-10-15 13:49:30 UTC)` |
| `%APPDATA%\SSHTermaq\logs\sshtermaq.log` | the same line when the app starts |
| Explorer › `SSHTermaq.exe` › Properties › Details | *File version* `1.0.0.10`, *Product version* `1.0.0 (build 10, 35de5d8-dirty)` |
| `README.txt` in the release folder | the version and build id in its title |

## 10. Code signing

Unsigned builds work, but Windows SmartScreen warns ("Windows protected your PC") before their first start.
[scripts/sign.ps1](../scripts/sign.ps1) signs with `signtool.exe` from the Windows SDK, which comes with Visual
Studio: a SHA-256 signature and an RFC 3161 timestamp, so the signature stays valid after the certificate expires.

```powershell
.\scripts\sign.ps1 -Check                  # show the configuration and the certificate; signs nothing
.\scripts\build.ps1 -Sign -Installer       # signed release folder, zip and installer
.\scripts\sign.ps1 file.exe [file.dll ...] # sign files on their own
```

With `-Sign`, `build.ps1` checks the configuration before it builds anything, signs `SSHTermaq.exe` and `ssh.dll`
before zipping them, and hands `sign.ps1` to Inno Setup as its sign tool, so `setup.exe` and the uninstaller inside
it are signed as well.

### Certificates

| Option | Cost (approx.) | Configure |
|---|---|---|
| Azure Trusted Signing | about $10 a month | `azureDlib` + `azureMetadata`; identity validation, and individuals only in some countries |
| OV certificate from a CA (Sectigo, SSL.com, DigiCert, Certum, ...) | $200–500 a year; Certum's open-source certificate is much cheaper | nothing (automatic) or `thumbprint`: the key is on a USB token (or a cloud service with a "virtual token" driver), and its certificate shows up in the Windows store |
| SignPath Foundation | free for open-source projects | signs in their CI service, not with this script |
| Self-signed | free | nothing (automatic), `thumbprint` or `pfx`: for trying the pipeline; Windows does not trust it |

None of them removes the SmartScreen warning at once, not even an EV certificate: the reputation builds up as people
download signed releases.

### Configuration

**With nothing configured** (no `signing.json`, or an empty `thumbprint`), `sign.ps1` chooses the certificate
itself, like `signtool sign /a`. It looks in `CurrentUser\My` for certificates that are valid now, have their private
key and carry the Code Signing usage (the ones `Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert` lists).
- If there is exactly one, it signs with it. A CA's USB token counts once its driver is installed and it is plugged in.
- If there are several, it lists them with their thumbprints and stops, rather than guessing: put the one you want in
  `thumbprint`.
- Certificates without any key-usage restriction are not picked, although Windows allows them for every purpose:
  apps leave such self-signed certificates in the user's store.

Otherwise put the settings in `scripts\signing.json` (not committed; start from
[scripts/signing.example.json](../scripts/signing.example.json)) or in environment variables, which win over the
file (for CI secrets). Set at most one of `thumbprint`, `pfx` or the two `azure…` values.

| `signing.json` | Environment variable | Meaning |
|---|---|---|
| `thumbprint` | `SSHTERMAQ_SIGN_THUMBPRINT` | SHA-1 thumbprint of a certificate in `CurrentUser\My` (USB tokens appear there when their driver is installed); empty: the automatic choice |
| `machineStore` | `SSHTERMAQ_SIGN_MACHINE_STORE` | `true` / `1`: look in `LocalMachine\My` instead, also for the automatic choice |
| `pfx` | `SSHTERMAQ_SIGN_PFX` | a `.pfx` file |
| — | `SSHTERMAQ_SIGN_PFX_PASSWORD` | its password: only from the environment, never in the file |
| `azureDlib` | `SSHTERMAQ_SIGN_AZURE_DLIB` | `bin\x64\Azure.CodeSigning.Dlib.dll` from the NuGet package `Microsoft.Trusted.Signing.Client` |
| `azureMetadata` | `SSHTERMAQ_SIGN_AZURE_METADATA` | a JSON file: `{"Endpoint": "https://<region>.codesigning.azure.net", "CodeSigningAccountName": "...", "CertificateProfileName": "..."}` |
| `timestampUrl` | `SSHTERMAQ_SIGN_TIMESTAMP_URL` | default `http://timestamp.digicert.com` (Azure: `http://timestamp.acs.microsoft.com`); `none` skips the timestamp, for tests only |
| — | `SSHTERMAQ_SIGN_CONFIG` | another path for `signing.json` |

The thumbprint of a certificate you have: `Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert | Format-List
Subject, Thumbprint, NotAfter`. For Azure Trusted Signing, sign in first with `az login` (or set `AZURE_CLIENT_ID`,
`AZURE_TENANT_ID` and `AZURE_CLIENT_SECRET` for a service principal). `*.pfx` and `*.p12` files are ignored by git.

### Trying it with a test certificate

```powershell
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=SSHTermaq Test' -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddMonths(1)
$env:SSHTERMAQ_SIGN_TIMESTAMP_URL = 'none'
.\scripts\build.ps1 -Sign -Installer   # picks the certificate automatically when it is your only code-signing one
Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)"   # afterwards
```

The files are signed, but Windows reports the signature as not trusted (*Explorer › Properties › Digital
Signatures*, or `Get-AuthenticodeSignature file.exe`): never ship such a build. With a real certificate,
`Get-AuthenticodeSignature` reports `Valid`, and so does `signtool verify /pa file.exe`.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `vcpkg install` fails with `Operation REMOVE_DUPLICATES not recognized` | vcpkg is too old: update Visual Studio (step 3). |
| `cmake --preset vs` fails with `Could not find toolchain file: …/vcpkg.cmake` | `CMakePresets.json` points `VCPKG_ROOT` at VS 2022 **Community**. For Professional / Enterprise add a local, uncommitted `CMakeUserPresets.json` (below). |
| `Could not find a package configuration file provided by "libssh"` | Step 4 was not run, or was run with another triplet / install root. |
| App shows "This copy of SSHTermaq was built without its user interface" | `ui\dist` was missing during the native build: build the UI (step 5), then rebuild `SSHTermaq` to embed it. |
| App asks to install the WebView2 Runtime | Install the Evergreen WebView2 Runtime (Windows 10). |
| `npm ci` warns `EBADENGINE` | Node.js is older than 20.19; install the current LTS. |

`CMakeUserPresets.json` for other Visual Studio editions (use `--preset vs-local` afterwards):

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "vs-local",
      "inherits": "vs",
      "environment": { "VCPKG_ROOT": "C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/vcpkg" }
    }
  ]
}
```
