# SSH Crawler

A modern, tabbed SSH / Telnet / Rlogin / Raw / Serial client for Windows with PuTTY's feature set and one-click
import of your PuTTY sessions and trusted host keys.

- **Native C++20 host** (Win32 + WebView2) with a C++ backend built on libssh 0.12, OpenSSL 3 and zlib
- **Modern UI** in React + xterm.js: tabs, split panes, sidebar with folders and search, command palette, quick connect,
  14 app themes, 29 terminal color schemes, custom scheme editor, configurable keybindings, Mica/Acrylic backdrop
- **PuTTY compatible**: PuTTY-style settings for every session, PuTTY command-line options, `.ppk` keys (v2/v3),
  Pageant, and import from the PuTTY registry or `.reg` files (sessions, "Default Settings", SSH host keys)

## Features

| Area | What you get |
|---|---|
| Protocols | SSH-2, Telnet (full option negotiation), Rlogin, Raw TCP, Serial (COM ports, all line settings) |
| SSH auth | Pageant and Windows OpenSSH agent, key files (OpenSSH, PEM, PuTTY PPK v2/v3 incl. Argon2 encrypted), keyboard-interactive (2FA), password; saved passwords encrypted with DPAPI |
| Host keys | Known-hosts store with new/changed-key prompts (SHA256 + MD5), logical host names, import from PuTTY and OpenSSH `known_hosts` |
| Forwarding | Local (-L), remote (-R) and dynamic SOCKS4/4a/5 (-D) port forwarding, agent forwarding, X11 forwarding |
| Proxies | SOCKS 4/4a/5 (with auth), HTTP CONNECT, Telnet proxy, local proxy command, SSH jump host; exclusion lists |
| Terminal | PuTTY keyboard modes (Backspace, Home/End, function-key variants, app cursor/keypad), local echo/line editing, charsets (UTF-8, ISO-8859-x, CP125x, KOI8, CP437/866, Shift_JIS, GBK, Big5, ...), bell modes, answerback, find, zoom, copy-on-select, right-click paste, bracketed paste, multi-line paste warning |
| Sessions | Folders, colors, notes, duplicate, restart, broadcast input to all panes, workspace restore, reconnect, session logging (printable / all / SSH debug, PuTTY `&H &Y &M &D &T &P` placeholders) |
| Files | SFTP browser per SSH pane: upload/download (drag & drop from Explorer), rename, delete, new folder, permissions, open remotely |
| Keys | Key generator (Ed25519, RSA, ECDSA), inspect and convert between OpenSSH and PPK |
| Import/export | PuTTY registry and `.reg` files, `~/.ssh/config` (ProxyJump, forwards, ...), JSON backup; export sessions to a PuTTY `.reg` file |

### Command line (PuTTY compatible)

```
SSHCrawler.exe [-ssh|-telnet|-rlogin|-raw|-serial] [user@]host[:port] [-P port] [-l user] [-pw password]
               [-i key.ppk] [-load "Saved Session"] [-L|-R|-D spec] [-A|-a] [-X|-x] [-C] [-4|-6] [-N]
               [-m file] [-sercfg 115200,8,n,1,N] ...
SSHCrawler.exe ssh://user@host:2222   telnet://host   @"Saved Session"
```
A second invocation opens a new tab in the already running window.

### Data and portable mode

Settings, sessions and trusted host keys live in `%APPDATA%\SSH Crawler`. Put a file named `portable.flag`
next to `SSHCrawler.exe` to keep everything in a `data` folder beside the program instead.

## Not supported (yet)

GSSAPI/Kerberos authentication, SSH-1, SUPDUP, bare ssh-connection, SSH certificates, connection sharing, and
X11 cookie substitution (X11 forwarding works with X servers that don't require authentication, e.g. VcXsrv with
`-ac`). The ML-KEM post-quantum key exchanges are disabled because libssh 0.12's client side fails with them;
sntrup761, curve25519 and ECDH are used instead.

## Building

Requirements: Visual Studio 2022 (Desktop C++, includes vcpkg and CMake), Node.js 20+, WebView2 Runtime.

```powershell
# one-time: C++ dependencies (libssh, OpenSSL, zlib, nlohmann-json, WebView2, WIL, Catch2)
& "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\vcpkg\vcpkg.exe" install --triplet x64-windows --x-install-root=vcpkg_installed

# everything: UI + native Release + dist\SSHCrawler-<version>-win-x64 (+ zip)
.\scripts\build.ps1            # -Tests runs all test suites first, -Portable adds portable.flag
```

Development:

```powershell
cd ui; npm install; npm run dev              # UI with a mock backend at http://localhost:5173
cmake --preset vs                            # native Debug build in build\vs
cmake --build build/vs --config Debug
build\vs\Debug\SSHCrawler.exe --dev-url http://localhost:5173 --devtools
```

An installer can be built with Inno Setup 6 from `scripts\installer.iss` after running `build.ps1`.

## Tests

- Native (Catch2): `build\vs\Debug\sc_test_*.exe` — core, store, CLI, session manager, charsets, logging, sockets,
  proxies, Telnet/Rlogin/Raw, SSH (all auth methods incl. Pageant, host keys, shell, exec, resize, signals,
  L/R/D forwarding, jump hosts), keys (PPK v2/v3 cross-checked with PuTTY's `plink`), PuTTY import (host-key
  conversion verified against `ssh-keygen` fingerprints), SFTP.
  Integration tests run against an in-process libssh test server (`native/tests/support`), itself validated with
  PuTTY's `plink`/`psftp`. `sc_testserver.exe --port 2222` runs that server standalone for manual testing
  (user `test`, password `secret`).
- UI (Vitest): `cd ui; npm test`
- End-to-end: `node ui/scripts/app.mjs --steps <file>` drives the real app over the WebView2 DevTools protocol.

## Architecture

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
