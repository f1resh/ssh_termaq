# native/src/platform — the only place for OS-specific code

Everything outside this folder is portable C++20: no OS headers, no `#if` on the OS or compiler, no Win32/POSIX
types. The CTest `platform_boundaries` (`cmake/CheckPlatformBoundaries.cmake`) enforces it; files that have not moved
yet are listed in `cmake/platform_allowlist.txt`, which only shrinks.

## Layout

| Folder | Contents | Included by |
|---|---|---|
| `Platform.h`, `sys/` | **Tier 0** headers, namespace `st::sys`: errors, time, debug output, environment, files and paths, ... Plain types only; they include the standard library and other `platform/sys` headers, never `core/` | everything |
| `ui/` | **Tier 1** headers, namespace `st::ui`: host window, app shell (event loop, single instance), file dialogs, clipboard, shell, local files, drag & drop, embedded UI; may use `core/Types.h`, `core/Strings.h`, `core/Threading.h`, `core/Log.h` | app, sftp, localfs, importers |
| `detail/` | private helpers shared by the implementations (e.g. `WinApi.h`: the Windows headers, included in one place) | `platform/**` and `native/tests/platform/**` only |
| `common/<tier>/` | implementations shared by all OSes (written against `detail/`) | — |
| `win/<tier>/` | Windows implementations (Win32, Winsock, COM, WebView2) | — |
| `posix/<tier>/` | POSIX implementations shared by macOS (and any future Unix) | — |
| `mac/<tier>/` | macOS-only implementations (CoreFoundation, Security, IOKit, AppKit, WebKit; `.mm` for Objective-C++) | — |
| `<os>/main/` | the executable's entry point (Windows: `wWinMain`), which calls `st::runApp` (`app/App.h`); built into the executable, not a library | — |

The headers are the same on every OS; CMake picks the implementation folders: Windows builds `common` + `win`,
macOS builds `common` + `posix` + `mac` (`st_platform_glob` in `CMakeLists.txt`, not recursive). An implementation file
never tests the OS with `#if`: it is Windows code because it lives in `win/`.

Targets: `st_platform_sys` (tier 0, links only OS libraries) is used by `st_core` and everything above it;
`st_platform_ui` (tier 1) links `st_core` and the OS UI libraries.

## Rules

1. A function belongs here when its *implementation* differs per OS. Its header must still be OS-neutral: no
   `HANDLE`, `SOCKET`, `wchar_t`, file descriptors or Objective-C types in a signature (use `std::filesystem::path`,
   UTF-8 `std::string`, `std::intptr_t` handles or opaque classes).
2. Strings are UTF-8; paths cross module boundaries as UTF-8 strings and become `std::filesystem::path` with
   `sys::toPath` / `sys::fromPath` (`sys/Fs.h`), never `widen`/`narrow`.
3. Keep behaviour identical on Windows when moving code here: move it verbatim first, change it in a later commit.
4. A header needs an implementation for every OS whose build uses it (`win/`, and `posix/` or `mac/`) — real, or a
   clear "not supported on this OS" error. Platform tests (`native/tests/platform`) pin the behaviour all OSes share.
5. Platform code never includes module headers (bridge, store, ssh, app, ...). It reports back through callbacks the
   modules set (e.g. `ui::HostWindow`'s hooks, connected by `app/HostController`).

## Status

All of `native/src` and `native/tests` outside the platform folders is OS-neutral: `cmake/platform_allowlist.txt` is
empty. The test support's OS parts (raw socket helpers, child processes, the test SFTP server's file access, tools)
are in `native/tests/platform/<os>/support`.

macOS implementations (`posix/`, `mac/`) are being added step by step. The macOS build has the whole sys tier
(sockets wait with kqueue there: poll(2) does not report TCP urgent data on macOS; serial ports use select(2): poll(2)
does not work on devices), every module and `SSHTermaq.app`, with every test suite but `platform_ui`. Its ui tier has
the app shell with the menu bar, the host window (`HostWindowMac.mm`; bridge, dispatcher, title bar and menus:
docs/ARCHITECTURE.md, "Bridge"; in Debug builds also the automation socket for ui/scripts/app.mjs), the embedded UI,
fonts, the clipboard, file dialogs as sheets, the shell (a file no application opens goes to the one the user picks)
and the local files (`LocalFsMac.mm`): Home, Desktop, Documents, Downloads and the mounted volumes; the Trash;
copy / move through NSFileManager with Finder's "already exists" question as a sheet (Replace / Keep Both / Stop,
Apply to All; existing folders are replaced as a whole, as Finder does) and no progress window; FSEvents for folder
watching. The local pane starts in Home, which needs no permission. Drag & drop (`DragDropMac.mm`): local files go
out as file URLs, starting from the web view's latest mouse event while the button is down; our own drags dropped
back on the window are refused by the web view and reported "internal" (CSS px); files dropped from Finder are
matched by name for files:pathsForFiles, since a File has no path in WebKit.
