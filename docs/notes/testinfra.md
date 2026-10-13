# testinfra — notes

Owner of `native/tests/support/**`, `native/tools/testserver/**`, `scripts/**`.

## Status (resume checklist)

- [ ] TestEchoServer / TestScriptServer (Winsock, one poll thread each)
- [ ] TestSshServer core: bind, ephemeral host key, accept thread, per-connection thread, events log, stop()
- [ ] Auth: password, publickey (authorized lines, signature state), keyboard-interactive (multi prompt, echo), banner
- [ ] Session channel: pty-req, env, window-change, shell commands, exec, signal, x11-req, auth-agent-req
- [ ] SFTP subsystem (own SFTP v3 implementation, confined to sftpRoot)
- [ ] direct-tcpip, tcpip-forward / cancel-tcpip-forward (forwarded-tcpip channels)
- [ ] Self-tests (sc_test_support) with plink / ssh / psftp / sftp
- [ ] sc_testserver tool
- [ ] scripts/build.ps1, scripts/dev.ps1

## Requests

(none yet)
