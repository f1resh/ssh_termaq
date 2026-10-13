# ssh module — notes

Owner files: `native/src/ssh/**`, `native/tests/ssh/**`.

## Status (resume checklist)

- [ ] SshUtil (RAII wrappers, libssh log routing, algorithm filtering, pty modes)
- [ ] SshAgent (Pageant WM_COPYDATA + OpenSSH named pipe client, socket-pair relay thread)
- [ ] SshClient (shared connect + host key + auth code path; used by transport and jump)
- [ ] SshTransport (I/O thread, main channel, pty/env/shell/exec/noShell, write/resize/specials/close, keepalive, post)
- [ ] Forwarding (local -L, remote -R, dynamic SOCKS4/4a/5, X11 with cookie substitution, agent forwarding)
- [ ] SshJump (jump host tunnel relay)
- [ ] tests (native/tests/ssh, sc_test_ssh)

## Design
(see below, filled in as implemented)

## Requests
(none yet)
