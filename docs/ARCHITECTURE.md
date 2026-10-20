# SSHTermaq — Architecture & contributor contract

A modern, tabbed SSH/Telnet/Rlogin/Raw/Serial client with PuTTY's feature set plus PuTTY session import. It runs on
Windows; the macOS port (Intel and Apple Silicon) builds on the same code, with everything OS-specific in
`native/src/platform` (see [platform/README.md](../native/src/platform/README.md)).

```
SSHTermaq (C++20; Windows: Win32 + WebView2; macOS: AppKit + WKWebView)
├─ app/        composition root, HostController (window policy: allowed origins, close confirmation, placement),
│              core bridge handlers, CLI, embedded UI table, build info (the window, WebView2 host, WinMain,
│              single instance and dialogs: platform/ui)
├─ bridge/     JSON message router  <── web view messages ──>  UI (React + xterm.js, ui/)
├─ store/      sessions.json, settings.json, state.json, known_hosts.json, credentials.json (DPAPI)
├─ session/    SessionManager: connections, charset, local echo/edit (ldisc), session logging, prompts
├─ transport/  Telnet, Rlogin, Raw, Serial  (+ TransportFactory)
├─ net/        Winsock helpers, proxies (SOCKS4/4a/5, HTTP, Telnet, local command, SSH jump)
├─ ssh/        libssh transport: auth (agent/Pageant, keys, kbd-int, password), host keys, PTY,
│              port forwarding L/R/D, X11, agent forwarding, SFTP task posting
├─ keys/       key loading (OpenSSH/PEM/PPK v2/v3), PPK read/write, keygen (PuTTYgen equivalent)
├─ putty/      PuTTY registry/.reg import + export, PuTTY host key conversion
├─ importers/  ~/.ssh/config, JSON backup, OpenSSH known_hosts, import apply logic + bridge handlers
├─ sftp/       file manager backend on SSH connections: RemoteFs over SFTP or SCP, transfer engine (queue,
│              conflicts, resume, move), find, synchronize, edit with auto-upload
├─ localfs/    local:* bridge handlers for the file manager (the OS side: platform/ui/LocalFs)
└─ platform/   OS-specific code only (platform/README.md): sys/ (files, sockets, ...), ui/ (dialogs, shell,
               drag & drop of local files with internal drops)
```

Linking: everything is static, including the C/C++ runtime (`/MT`), except libssh, which ships as `ssh.dll`
(LGPL: replaceable) with its own static runtime. Custom vcpkg triplet `triplets/x64-windows-st.cmake`. Because
`ssh.dll` has a separate heap, memory allocated by libssh must be released with libssh functions
(`ssh_string_free_char`, `ssh_key_free`, `sftp_attributes_free`, ...), never with `free()`.

Dependencies (vcpkg manifest `vcpkg.json`, installed into `vcpkg_installed/`): libssh 0.12 (OpenSSL 3, zlib),
OpenSSL 3.6, zlib, nlohmann-json 3.12, WebView2 SDK (Windows only), Catch2 3. libssh sources (for reference while
implementing) are in `vcpkg_installed/vcpkg/blds/libssh/src/libssh-0-e5f4972781.clean/`.

UI: `ui/` — Vite + React 19 + TypeScript 5.9 + zustand + xterm.js 5.5 (+ fit/webgl/search/web-links/unicode11
addons) + lucide-react icons. The built UI (`ui/dist`) is embedded in the executable and served from memory
(`cmake/EmbedUi.cmake`, `app/UiResources.h`, `app/HostController`): on Windows as resources of `SSHTermaq.exe`, served
for `https://app.sshtermaq.local/` by WebView2's `WebResourceRequested` (`HostWindowWin::serveUiResource`); on macOS
as two sections of `SSHTermaq.app`'s executable (`-sectcreate`), served for `sshtermaq://app/` by a
`WKURLSchemeHandler` (`HostWindowMac`). There is no `ui` folder on disk (dev: `--dev-url http://localhost:5173`).

Version: set only in `CMakeLists.txt` (`project(SSHTermaq VERSION x.y.z)`). On every build `cmake/BuildInfo.cmake`
writes `build/<preset>/generated/st_build_info.h` (version, build number = commit count, short commit hash with
`-dirty`), used by `native/res/windows/app.rc` (file version `x.y.z.<build>`) and `app/BuildInfo.h`, which adds the link time
from the PE header and feeds `app:info`, the startup log line and `--version`. The manifest is generated from
`native/res/windows/app.manifest.in` at configure time. Details: [BUILDING.md](BUILDING.md#9-versions-and-build-ids).

## Contracts (read before writing code)

| Contract | File(s) |
|---|---|
| Domain types (TS) | `ui/src/shared/types.ts` |
| Defaults / normalisation (TS) | `ui/src/shared/defaults.ts` |
| Bridge API + wire format | `ui/src/shared/ipc.ts` (API_SCHEMA lists every channel) |
| Domain types (C++ mirror) | `native/src/core/Types.h` — JSON names/enums/defaults MUST match the TS files |
| Bridge (C++) | `native/src/bridge/Bridge.h` |
| Transport interface + threading rules | `native/src/transport/Transport.h` |
| Prompts | `native/src/session/Prompter.h`, `prompts::*` helpers in `core/Types.h` |
| Module public headers | each module's `.h` in `native/src/**` (stubs exist for all of them) |
| UI stores | `ui/src/store/workspace.ts`, `ui/src/store/ui.ts`, `ui/src/store/data.ts` |
| Terminal handles | `ui/src/terminal/registry.ts` |
| UI primitives + tokens | `ui/src/components/ui/*`, `ui/src/styles/global.css` (CSS variables only, no hard-coded colours) |

Rules:
1. **Public signatures in contract headers / TS contract files are frozen.** You may *add* functions, members,
   fields, files. If you truly must change an existing signature, keep the old one working and note it in
   `docs/notes/<module>.md`.
   *Recorded exception, the platform split (2026):* every declaration that carried an OS type moved into
   `native/src/platform` or changed to an OS-neutral type, without keeping the old form: sockets are
   `sys::SocketHandle` (was `SOCKET`), native windows `void*` handles (was `HWND`), `runApp` takes UTF-8 arguments
   (was `wchar_t** argv`), `widen()`/`narrow()` are private to `platform/win`, `core/Win.h` is gone, and
   `MainWindow`, `Dialogs`, `LocalFs`, `DragDrop` and the Winsock / serial / agent / proxy-command code live in
   `platform/` (the window's policy in `app/HostController`). The wire format and `core/Types.h` did not change.
2. **Only edit files you own** (table below). Need something from another module? Use its declared API; if it's
   missing, write the need into `docs/notes/<your-module>.md` ("Requests") and work around it.
3. Adding a field to SessionConfig/AppSettings requires updating BOTH `types.ts`/`defaults.ts` AND `Types.h`.
   Only the owners listed below may do that (C++ types: owner `host`; TS types: owner `ui-dialogs`) — other
   modules file a request.
4. All strings crossing the bridge are UTF-8 JSON. Paths are UTF-8 strings; convert them with `sys::toPath()` /
   `sys::fromPath()` (`platform/sys/Fs.h`), never through `std::wstring` or `path::string()`.
5. Never block the UI thread (no network, no disk-heavy work, no waiting on prompts). Use `Bridge::handleAsync`
   or your own threads.
6. Never log secrets (passwords, passphrases, private keys) to the event log, app log or session log.
7. Tests must only use localhost. The user's real PuTTY registry may be **read** (never written) — tests that
   read it must not print hostnames/usernames, only counts.
8. **OS-specific code lives only in `native/src/platform`** (and `native/tests/platform`): OS headers, Win32 /
   POSIX / Cocoa types, `#if` on the OS or compiler, wide strings. Everything else uses the OS-neutral headers in
   `platform/sys` and `platform/ui`; CMake picks each OS's implementation folders. The CTest `platform_boundaries`
   enforces it; `cmake/platform_allowlist.txt` lists what has not moved yet (only the Windows test support, until the
   macOS port of the test servers) and may only shrink.
9. No new third-party dependencies without a note in `docs/notes/` (vcpkg/npm installs are coordinated).
10. **Layers:** `platform/sys` (tier 0) includes only the standard library and other `platform/sys` headers;
    `platform/ui` (tier 1) may also use `core/Types.h`, `core/Strings.h`, `core/Threading.h` and `core/Log.h`.
    Platform code never includes module headers (bridge, store, ssh, ...): modules call the platform, and the
    platform reports back through callbacks (e.g. `ui::HostWindow`'s hooks, connected by `app/HostController`).

### Bridge
Channel = `namespace:method` from `API_SCHEMA`. Invoke args are positional JSON arrays (optional trailing args
may be absent/null). Events are pushed with `bridge.emit("ns:onX", Json::array({...callback args}))`.
Every invoke/send channel in `API_SCHEMA` must have exactly one C++ handler (owner in the table).

**Transport per host.** Both directions carry the same JSON messages (`ui/src/shared/ipc.ts`); only the way they cross
into the page differs:

| | Page → host | Host → page |
|---|---|---|
| Windows (WebView2) | `chrome.webview.postMessage` | `PostWebMessageAsJson`, one message at a time |
| macOS (WKWebView) | `webkit.messageHandlers.st.postMessage` | `callAsyncJavaScript` with the queued messages as one JSON array string (page: `JSON.parse`); at most ~1 MB per call and 4 calls in flight, the rest waits in the host |

**macOS decision** (step B3 of the port, measured with [native/tools/hostprobe](../native/tools/hostprobe/README.md)):
`callAsyncJavaScript` moves about 225 MB/s, 4× what xterm.js can render, passes the messages as data (never as
script source) and its completions give backpressure. Rejected: `evaluateJavaScript` (as fast, but the messages
become script code to compile), a page-driven pull through `WKScriptMessageHandlerWithReply` (25% slower; every
request must be answered and a dead pull loop stops delivery silently), and a streamed `fetch` over the
`sshtermaq://` scheme (12 MB/s, seconds of latency).

**Title bar and menus per host.** `ui/src/shared/platform.ts` is the UI's one switch between the hosts.
- **Windows:** the page's title bar is a CSS `app-region` drag area with HTML caption buttons, and the app's commands
  are behind the logo button.
- **macOS:** the native traffic lights sit over the page's bar. `window:setTitleBarLayout` tells the host the bar's
  height (the lights are centred in it) and its drag areas, which the host hit-tests on mouse-down because WKWebView
  ignores `app-region`; the page keeps room for the lights (`window:onChromeInsets`). The app's commands are in the
  native menu bar (`window:setMenuBar` / `window:onMenuCommand`, `ui/src/commands/menubar.ts`), its shortcuts use ⌘
  (`MAC_KEYBINDINGS` in `ui/src/shared/defaults.ts`) and are shown as Mac symbols; Option types characters in the
  terminal unless "Use Option as Meta key" is set.

### Threading
UI thread = the host window's event loop (Windows: Win32 message loop + WebView2; macOS: the main run loop, where
`Dispatcher::post` uses a run-loop source in the common modes: the GCD main queue stops while a modal panel runs, so
nothing would reach the page) + bridge handlers. Each connection has its own I/O thread (owned by
its Transport). Prompts block the transport thread, never the UI thread. Long bridge work → `handleAsync`
(shared ThreadPool) or a dedicated thread. `Dispatcher::post` marshals to the UI thread.

### Connection data path
```
UI keystroke ─conn:write─▶ SessionManager ─▶ LineDiscipline (echo/edit) ─▶ charset encode ─▶ Transport::write
Transport::onData(bytes) ─▶ SessionLog ─▶ Decoder(charset) ─▶ batch ─conn:onData─▶ xterm.write(string)
```
connId = `<paneId>#<generation>` (the UI restarts a pane by bumping the generation).

## Module ownership

| Module (agent) | Owns |
|---|---|
| `ssh` | `native/src/ssh/**`, `native/tests/ssh/**` |
| `net` | `native/src/net/**`, `native/src/transport/{Telnet,Rlogin,Raw,Serial}*`, `native/src/session/LineDiscipline.*`, `native/tests/net/**` |
| `keys-import` | `native/src/keys/**`, `native/src/putty/**`, `native/src/importers/**`, `native/tests/{keys,putty}/**`, `native/tests/fixtures/**` |
| `host` | `native/src/app/**`, `native/src/store/**`, `native/src/core/Types.*` (additions), `native/res/**`, `native/tests/{store,app}/**`, `CMakeLists.txt` (additions only) |
| `session` | `native/src/session/{SessionManager,Charset,SessionLog}.*`, `native/src/transport/TransportFactory.*`, `native/tests/session/**` |
| `sftp` | `native/src/sftp/**`, `native/src/localfs/**`, `native/tests/sftp/**`, `ui/src/components/files/**`, `ui/src/bridge/mock/{sftp,local}.ts` |
| `platform` | `native/src/platform/**`, `native/tests/platform/**`, `cmake/CheckPlatformBoundaries.cmake`, `cmake/platform_allowlist.txt`, `triplets/**` |
| `testinfra` | `native/tests/support/**`, `scripts/**` |
| `ui-shell` | `ui/src/App.tsx`, `ui/src/components/{titlebar,sidebar,layout,palette,welcome,statusbar,quickconnect}/**`, `ui/src/commands/**`, `ui/src/theme/**`, `ui/src/store/**` (keep exported APIs), `ui/src/bridge/mock/{index,core}.ts` |
| `ui-terminal` | `ui/src/terminal/**`, `ui/src/bridge/mock/conn.ts` |
| `ui-dialogs` | `ui/src/components/dialogs/**`, `ui/src/shared/types.ts` + `defaults.ts` (additions), `ui/src/bridge/mock/{importer,keys}.ts` |

Bridge handler ownership: `conn:*`, `prompts:*` → session · `importer:*`, `keys:*`, `knownHosts:importOpenSsh` →
keys-import · `sftp:*`, `local:*` → sftp · everything else (`sessions`, `credentials`, `settings`, `knownHosts` list/remove,
`serial`, `dialog`, `app`, `window`) → host. The host's composition root calls `registerConnHandlers`,
`registerImportHandlers`, `SftpService::registerHandlers`, `LocalFsService::registerHandlers` and its own core
handlers, and calls `ui::dialogs::init` and `ui::dnd::init` once the main window exists (`ui::UiThread` sets up
the UI thread: OLE on Windows).

## File manager

An SSH terminal can also show the lighter SFTP side panel (`ui/src/components/sftp/SftpPanel.tsx`, one per tab, toggled with `pane.sftp`):
it opens its own `sftp:open` session on the terminal's connection and shares transfers and conflict questions with the
file manager through `filesStore`. A file manager tab is a workspace pane of kind `files` (always alone in its tab). It works on the connection of
the terminal it was opened from (`linkedPaneId`, no second login) or on a connection of its own: a shell-less copy
of the session (`noShell`, no tunnels / X11 / agent forwarding) opened by `ui/src/components/files/filesConn.ts`.
On that connection `sftp:open` picks the protocol (`ssh.fileProtocol`: auto = SFTP, falling back to SCP).

Native layering (`native/src/sftp`):

- `RemoteFs` — protocol-neutral remote file access. `SftpFs` (libssh sftp, pipelined `RemoteFile`) and `ScpFs`
  (POSIX `sh -c` scripts on exec channels for browsing — `ls -la` parsed by `ScpProtocol` for GNU / BSD /
  BusyBox / plain formats — and the scp protocol, one `scp -f` / `scp -t` channel per file). Every libssh call runs
  in bounded steps on the connection thread through `SshTransport::post`, so the terminal on the same connection
  stays responsive.
- `TransferEngine` — one queue per remote session (1–6 workers), recursive planning (reported as `listing` with the
  files found so far, before the copy starts; downloads onto a disk that ignores letter case number the names that
  differ only in case; uploads may leave out macOS Finder's `.DS_Store` / `._*` files), conflict policy per request
  (ask → `sftp:onConflict` / `sftp:resolveConflict`, one question per request at a time, "apply to all"), resume
  (`.st-partial` files; SFTP offset writes), move, synchronize deletes, throttled `sftp:onProgress`.
- `FindSync` — glob search (`sftp:find` → `sftp:onFind` batches) and the pure synchronize comparison
  (`compareTrees`) with local / remote walkers. `EditWatcher` logic lives in `SftpService` (temp copy polled every
  500 ms, upload after a quiet period; a changed server copy asks first).
- Drag & drop: local files are a native drag (`platform/ui/DragDrop`: Explorer / Finder take them; drops back on our
  own window are cancelled and handed to the page as "internal"); server items are the page's own drag, which only
  the two panes take, so they never leave the window (to Explorer / Finder: through the local pane or F5). Files from
  Explorer / Finder dropped on a pane or the SFTP panel come in through `files:pathsForFiles`.

Tests: `native/tests/sftp` (transfers, conflicts, resume, move, find, sync, edit, SCP against real GNU `ls` and
OpenSSH `scp` run through Git for Windows' `sh.exe` by the test server), `native/tests/platform/win/ui` (local
files, drag & drop of local files). `st_testserver --sftp off --exec-shell git` serves an SCP-only server.

## Building & testing

```powershell
# UI
cd ui; npm run typecheck; npm test; npm run build          # build -> ui/dist (embedded into the exe by the native build)
node scripts/shot.mjs --name <you> --out ../artifacts/<you>.png [--steps my-steps.mjs]   # mock-mode screenshot (headless Edge)

# Native — use YOUR OWN build dir (build/<module>) so parallel builds don't collide
$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake --preset vs -B build/<module>                      # configure once (fast; vcpkg already installed)
& $cmake --build build/<module> --config Debug --target st_<lib> -- /nologo /v:minimal /m
& $cmake --build build/<module> --config Debug --target st_test_<module> -- /nologo /v:minimal /m
build/<module>/Debug/st_test_<module>.exe                   # Catch2: add "[tag]" or a test name to filter
```
Libraries: `st_platform_sys st_core st_bridge st_platform_ui st_store st_net st_transport st_keys st_ssh st_session st_putty st_localfs st_sftp st_app`,
executable `SSHTermaq`. Sources are globbed — new `.cpp` files in your folders are picked up automatically.
Tests: `native/tests/<module>/*.cpp` → `st_test_<module>`; `native/tests/support` (TestSshServer, TestEchoServer,
TestScriptServer, TempDir) is linked into every test exe. `ST_FIXTURES_DIR` points at `native/tests/fixtures`.
If the build fails in a file you don't own, another agent is mid-edit: wait a minute and retry; don't touch it.

Reference tools available locally: PuTTY 0.83 (`C:\Program Files\PuTTY\{putty,plink,pscp,psftp,puttygen,pageant}.exe`),
OpenSSH client tools (`C:\Windows\System32\OpenSSH\ssh-keygen.exe`, `ssh.exe`). `plink -batch` is a handy reference
client against the test SSH server (e.g. to validate PPK files we write).

## PuTTY feature parity checklist

Status as of 1.0.0 (all items implemented and covered by native/UI tests or end-to-end checks).
Known gaps: GSSAPI, SSH-1, SUPDUP, bare ssh-connection, SSH certificates, connection sharing, ML-KEM key exchange
(disabled: libssh client issue).

- [x] Protocols: SSH-2, Telnet, Rlogin, Raw, Serial (SUPDUP / bare ssh-connection / SSH-1: not supported)
- [x] Saved sessions (+ folders), "Default Settings" template, duplicate session, restart session, CLI `-load`
- [x] Logging: off / printable / all output / SSH debug; placeholders &Y &M &D &T &H &P &S; overwrite/append/ask; flush
- [x] Terminal: auto-wrap, implicit CR/LF, local echo & line editing (auto/on/off), answerback (^E), bell (none/system/visual/sound), scrollback, remote title
- [x] Keyboard: Backspace ^H/^?, Home/End standard/rxvt, function key modes (ESC[n~, Linux, Xterm R6, VT400, VT100+, SCO, xterm), disable app cursor/keypad, Ctrl+Alt=AltGr
- [x] Window/appearance: font, size, cursor block/underline/bar + blink, fixed title, close on exit always/never/clean, warn on close, full screen
- [x] Translation: remote charset (UTF-8, ISO-8859-x, CP125x, KOI8, CP437/866, Shift_JIS, EUC, GBK, Big5...), CJK ambiguous width
- [x] Selection: copy-on-select, right-click paste or menu, middle click, word separators, rectangular (Alt+drag), Shift+Insert / Ctrl+Insert
- [x] Colours: schemes, bold-as-bright, PuTTY 22-colour import, custom scheme editor
- [x] Connection: keepalives, TCP_NODELAY, SO_KEEPALIVE, IPv4/IPv6/auto, logical host name, TERM, terminal speed, env vars, auto-login username
- [x] Proxy: SOCKS4/4a, SOCKS5 (+auth), HTTP CONNECT (+auth), Telnet, local command, SSH jump; exclusions; localhost; DNS at proxy
- [x] SSH: remote command, no shell, compression, cipher/kex/hostkey/mac preferences, host key cache + verification prompts (new/changed), logical host name for keys
- [x] SSH auth: Pageant + Windows OpenSSH agent, private keys (PPK v2/v3 + OpenSSH/PEM, passphrase), keyboard-interactive, password, banner, saved passwords (DPAPI)
- [x] SSH: agent forwarding, X11 forwarding (fake cookie checked per connection, real cookie from Xauthority; display
  by Unix socket - also XQuartz's launchd path - or TCP), port forwarding L/R/D (+accept from other hosts, IPv4/IPv6), PTY allocation on/off
- [x] Serial: line, speed, data/stop bits, parity, flow control; Telnet: active/passive, keyboard specials, Return=NEWLINE; Rlogin: local username
- [x] Special commands menu (Break, EOF, IGNORE, signals, telnet AYT/Synch/EC/EL/GA/NOP/AO/IP/SUSP/EOR/EOF...)
- [x] Context menu: event log, new/duplicate/restart session, saved sessions, change settings, copy all, clear scrollback, reset terminal, full screen
- [x] PSFTP/PSCP → SFTP side panel + file manager tab (WinSCP-style): local + server panes, SFTP with SCP fallback, copy/move (F5/F6, drag & drop between the panes and in from Explorer, local files out), conflicts (ask/overwrite/newer/skip/resume/rename), resume, edit with auto-upload, synchronize, find, bookmarks, chmod; PuTTYgen → keygen/convert/inspect; Pageant → agent support
- [x] Import: PuTTY registry sessions + Default Settings + SSH host keys, .reg files, export to .reg; ~/.ssh/config; OpenSSH known_hosts; JSON backup
- [x] CLI: `-ssh -telnet -rlogin -raw -serial [user@]host -P -l -pw -i -load -L -R -D -A -a -X -x -C -4 -6 -N -m -sercfg`, `sftp://` / `scp://` URLs
- [x] Modern: tabs (reorder, colours, rename, title from terminal / session name / host), split panes, broadcast input, command palette, quick connect, sidebar (folders, search, drag & drop), app themes + terminal schemes, Mica, settings UI, custom keybindings, reconnect, workspace restore, find, zoom, portable mode
