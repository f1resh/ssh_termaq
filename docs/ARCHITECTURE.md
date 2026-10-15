# SSHTermaq — Architecture & contributor contract

A modern, tabbed Windows SSH/Telnet/Rlogin/Raw/Serial client with PuTTY's feature set plus PuTTY session import.

```
SSHTermaq.exe (C++20, Win32)
├─ app/        WinMain, main window (custom title bar), WebView2 host, CLI, single instance, dialogs
├─ bridge/     JSON message router  <── WebView2 postMessage ──>  UI (React + xterm.js, ui/)
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
│              conflicts, resume, move), find, synchronize, edit with auto-upload, remote drag source
├─ localfs/    local disks for the file manager ("This PC", listings, IFileOperation copy/move/delete, watchers)
└─ dnd/        native OLE drag & drop: virtual-file data object (in the MTA), drop source with internal drops
```

Linking: everything is static, including the C/C++ runtime (`/MT`), except libssh, which ships as `ssh.dll`
(LGPL: replaceable) with its own static runtime. Custom vcpkg triplet `triplets/x64-windows-st.cmake`. Because
`ssh.dll` has a separate heap, memory allocated by libssh must be released with libssh functions
(`ssh_string_free_char`, `ssh_key_free`, `sftp_attributes_free`, ...), never with `free()`.

Dependencies (vcpkg manifest `vcpkg.json`, installed into `vcpkg_installed/`): libssh 0.12 (OpenSSL 3, zlib),
OpenSSL 3.6, zlib, nlohmann-json 3.12, WebView2 SDK, WIL, Catch2 3. libssh sources (for reference while
implementing) are in `vcpkg_installed/vcpkg/blds/libssh/src/libssh-0-e5f4972781.clean/`.

UI: `ui/` — Vite + React 19 + TypeScript 5.9 + zustand + xterm.js 5.5 (+ fit/webgl/search/web-links/unicode11
addons) + lucide-react icons. The built UI (`ui/dist`) is embedded in `SSHTermaq.exe` as resources
(`cmake/EmbedUi.cmake` generates the resource script) and served from memory for `https://app.sshtermaq.local/` by
WebView2's `WebResourceRequested` (`app/UiResources.h`, `MainWindow::serveUiResource`); there is no `ui` folder on
disk (dev: `--dev-url http://localhost:5173`).

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
2. **Only edit files you own** (table below). Need something from another module? Use its declared API; if it's
   missing, write the need into `docs/notes/<your-module>.md` ("Requests") and work around it.
3. Adding a field to SessionConfig/AppSettings requires updating BOTH `types.ts`/`defaults.ts` AND `Types.h`.
   Only the owners listed below may do that (C++ types: owner `host`; TS types: owner `ui-dialogs`) — other
   modules file a request.
4. All strings crossing the bridge are UTF-8 JSON. Paths are UTF-8 (convert with `widen()/narrow()`).
5. Never block the UI thread (no network, no disk-heavy work, no waiting on prompts). Use `Bridge::handleAsync`
   or your own threads.
6. Never log secrets (passwords, passphrases, private keys) to the event log, app log or session log.
7. Tests must only use localhost. The user's real PuTTY registry may be **read** (never written) — tests that
   read it must not print hostnames/usernames, only counts.
8. Include `core/Win.h` instead of `<windows.h>`/`<winsock2.h>`.
9. No new third-party dependencies without a note in `docs/notes/` (vcpkg/npm installs are coordinated).

### Bridge
Channel = `namespace:method` from `API_SCHEMA`. Invoke args are positional JSON arrays (optional trailing args
may be absent/null). Events are pushed with `bridge.emit("ns:onX", Json::array({...callback args}))`.
Every invoke/send channel in `API_SCHEMA` must have exactly one C++ handler (owner in the table).

### Threading
UI thread = Win32 message loop + WebView2 + bridge handlers. Each connection has its own I/O thread (owned by
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
| `sftp` | `native/src/sftp/**`, `native/src/localfs/**`, `native/src/dnd/**`, `native/tests/{sftp,localfs,dnd}/**`, `ui/src/components/files/**`, `ui/src/bridge/mock/{sftp,local}.ts` |
| `testinfra` | `native/tests/support/**`, `scripts/**` |
| `ui-shell` | `ui/src/App.tsx`, `ui/src/components/{titlebar,sidebar,layout,palette,welcome,statusbar,quickconnect}/**`, `ui/src/commands/**`, `ui/src/theme/**`, `ui/src/store/**` (keep exported APIs), `ui/src/bridge/mock/{index,core}.ts` |
| `ui-terminal` | `ui/src/terminal/**`, `ui/src/bridge/mock/conn.ts` |
| `ui-dialogs` | `ui/src/components/dialogs/**`, `ui/src/shared/types.ts` + `defaults.ts` (additions), `ui/src/bridge/mock/{importer,keys}.ts` |

Bridge handler ownership: `conn:*`, `prompts:*` → session · `importer:*`, `keys:*`, `knownHosts:importOpenSsh` →
keys-import · `sftp:*`, `local:*` → sftp · everything else (`sessions`, `credentials`, `settings`, `knownHosts` list/remove,
`serial`, `dialog`, `app`, `window`) → host. The host's composition root calls `registerConnHandlers`,
`registerImportHandlers`, `SftpService::registerHandlers`, `LocalFsService::registerHandlers` and its own core
handlers, and calls `dnd::init(window)` once the main window exists (the UI thread runs `OleInitialize`).

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
- `TransferEngine` — one queue per remote session (1–6 workers), recursive planning, conflict policy per request
  (ask → `sftp:onConflict` / `sftp:resolveConflict`, one question per request at a time, "apply to all"), resume
  (`.st-partial` files; SFTP offset writes), move, synchronize deletes, throttled `sftp:onProgress`.
- `FindSync` — glob search (`sftp:find` → `sftp:onFind` batches) and the pure synchronize comparison
  (`compareTrees`) with local / remote walkers. `EditWatcher` logic lives in `SftpService` (temp copy polled every
  500 ms, upload after a quiet period; a changed server copy asks first).
- `RemoteDragSource` + `dnd/` — drag out to Explorer: virtual files (`CFSTR_FILEDESCRIPTORW` / `CFSTR_FILECONTENTS`)
  from a data object created in the MTA, so Explorer's reads run on RPC threads, never on the UI thread; drops back
  on our own window are cancelled by the drop source and handed to the page as "internal" (pane-to-pane copy).

Tests: `native/tests/sftp` (transfers, conflicts, resume, move, find, sync, edit, SCP against real GNU `ls` and
OpenSSH `scp` run through Git for Windows' `sh.exe` by the test server, drag out into a real shell drop target),
`native/tests/localfs`, `native/tests/dnd`. `st_testserver --sftp off --exec-shell git` serves an SCP-only server.

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
Libraries: `st_core st_bridge st_store st_dialogs st_net st_transport st_keys st_ssh st_session st_putty st_dnd st_localfs st_sftp st_app`,
executable `SSHTermaq`. Sources are globbed — new `.cpp` files in your folders are picked up automatically.
Tests: `native/tests/<module>/*.cpp` → `st_test_<module>`; `native/tests/support` (TestSshServer, TestEchoServer,
TestScriptServer, TempDir) is linked into every test exe. `ST_FIXTURES_DIR` points at `native/tests/fixtures`.
If the build fails in a file you don't own, another agent is mid-edit: wait a minute and retry; don't touch it.

Reference tools available locally: PuTTY 0.83 (`C:\Program Files\PuTTY\{putty,plink,pscp,psftp,puttygen,pageant}.exe`),
OpenSSH client tools (`C:\Windows\System32\OpenSSH\ssh-keygen.exe`, `ssh.exe`). `plink -batch` is a handy reference
client against the test SSH server (e.g. to validate PPK files we write).

## PuTTY feature parity checklist

Status as of 0.1.0 (all items implemented and covered by native/UI tests or end-to-end checks).
Known gaps: GSSAPI, SSH-1, SUPDUP, bare ssh-connection, SSH certificates, connection sharing, X11 cookie
substitution, ML-KEM key exchange (disabled: libssh client issue).

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
- [x] SSH: agent forwarding, X11 forwarding, port forwarding L/R/D (+accept from other hosts, IPv4/IPv6), PTY allocation on/off
- [x] Serial: line, speed, data/stop bits, parity, flow control; Telnet: active/passive, keyboard specials, Return=NEWLINE; Rlogin: local username
- [x] Special commands menu (Break, EOF, IGNORE, signals, telnet AYT/Synch/EC/EL/GA/NOP/AO/IP/SUSP/EOR/EOF...)
- [x] Context menu: event log, new/duplicate/restart session, saved sessions, change settings, copy all, clear scrollback, reset terminal, full screen
- [x] PSFTP/PSCP → SFTP side panel + file manager tab (WinSCP-style): local + server panes, SFTP with SCP fallback, copy/move (F5/F6, drag & drop both ways incl. Explorer), conflicts (ask/overwrite/newer/skip/resume/rename), resume, edit with auto-upload, synchronize, find, bookmarks, chmod; PuTTYgen → keygen/convert/inspect; Pageant → agent support
- [x] Import: PuTTY registry sessions + Default Settings + SSH host keys, .reg files, export to .reg; ~/.ssh/config; OpenSSH known_hosts; JSON backup
- [x] CLI: `-ssh -telnet -rlogin -raw -serial [user@]host -P -l -pw -i -load -L -R -D -A -a -X -x -C -4 -6 -N -m -sercfg`, `sftp://` / `scp://` URLs
- [x] Modern: tabs (reorder, colours, rename, title from terminal / session name / host), split panes, broadcast input, command palette, quick connect, sidebar (folders, search, drag & drop), app themes + terminal schemes, Mica, settings UI, custom keybindings, reconnect, workspace restore, find, zoom, portable mode
