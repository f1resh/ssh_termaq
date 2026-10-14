# Building SSHTerm on Windows

A step-by-step setup of the build environment, from a clean Windows 10/11 x64 machine to a running debug build.
For the architecture and module layout see [ARCHITECTURE.md](ARCHITECTURE.md).

## 1. What you need

| Tool | Version | Used for |
|---|---|---|
| Visual Studio 2022 | 17.14+, workload **Desktop development with C++** | MSVC v143 (C++20), Windows SDK, CMake 3.25+, vcpkg |
| vcpkg (ships with VS) | **2025-11-19 or newer** | C++ dependencies (libssh, OpenSSL, zlib, nlohmann-json, WebView2, WIL, Catch2) |
| Node.js | 22 LTS or newer (Vite needs ≥ 20.19 / ≥ 22.12) | the React UI in `ui/` |
| Microsoft Edge WebView2 Runtime | any current | running the app (built into Windows 11) |
| Git | any | cloning; vcpkg also uses it |

Optional — the native tests skip the cases that need these when they are missing:

| Tool | Expected at | Used by |
|---|---|---|
| PuTTY 0.83 | `C:\Program Files\PuTTY\` | cross-checks with `plink`, `psftp`, `pageant` |
| Git for Windows | `C:\Program Files\Git\` | SCP tests (its `sh.exe`, GNU `ls`, OpenSSH `scp`) |
| OpenSSH client | `C:\Windows\System32\OpenSSH\` | `ssh-keygen` fingerprint checks |
| Inno Setup 6 | | building the installer (`scripts\installer.iss`) |

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

The native build copies `ui\dist` next to the executable. Build the UI first, or the app opens with
"The user interface files were not found".

## 6. Build and run the native app

**From the command line:**

```powershell
cmake --preset vs                                   # generates build\vs\SSHTerm.sln
cmake --build build/vs --config Debug --parallel    # or --config Release
build\vs\Debug\SSHTerm.exe
```

**In Visual Studio:** after `cmake --preset vs`, open `build\vs\SSHTerm.sln`. In Solution Explorer right-click
**SSHTerm** → *Set as Startup Project*, pick *Debug* / *x64* and press F5. The working directory is already set to
the executable's folder. Projects are grouped into `modules`, `tests` and `tools`; source files added to
`native/src/<module>/` are picked up the next time the solution regenerates (any build re-runs CMake when needed).

**UI development with hot reload:** run the Vite dev server and point the app at it, so UI edits show up without
rebuilding:

```powershell
cd ui; npm run dev                                                  # http://localhost:5173
build\vs\Debug\SSHTerm.exe --dev-url http://localhost:5173 --devtools
```

In Visual Studio put `--dev-url http://localhost:5173 --devtools` into SSHTerm → *Properties* → *Debugging* →
*Command Arguments*. Without the native app, http://localhost:5173 in any browser shows the UI against a mock
backend.

Pass `--data-dir <folder>` to keep test runs away from your real sessions in `%APPDATA%\SSHTerm`.

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
.\scripts\build.ps1            # UI + native Release → dist\SSHTerm-<version>-win-x64 and a .zip
.\scripts\build.ps1 -Tests     # run all test suites first
```

The folder holds `SSHTerm.exe`, `ssh.dll` and `ui\`; nothing else is needed on the target machine except the
WebView2 Runtime. Compile `scripts\installer.iss` with Inno Setup 6 for an installer.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `vcpkg install` fails with `Operation REMOVE_DUPLICATES not recognized` | vcpkg is too old: update Visual Studio (step 3). |
| `cmake --preset vs` fails with `Could not find toolchain file: …/vcpkg.cmake` | `CMakePresets.json` points `VCPKG_ROOT` at VS 2022 **Community**. For Professional / Enterprise add a local, uncommitted `CMakeUserPresets.json` (below). |
| `Could not find a package configuration file provided by "libssh"` | Step 4 was not run, or was run with another triplet / install root. |
| App starts with "The user interface files were not found" | Build the UI (step 5), then rebuild `SSHTerm` so `ui\dist` is copied. |
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
