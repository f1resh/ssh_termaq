# sftp module — notes

Owner files: `native/src/sftp/**`, `native/tests/sftp/**`, `ui/src/components/sftp/**`, `ui/src/bridge/mock/sftp.ts`.

## Status (resume checklist)

- [ ] native: SftpService core (open/list/realpath/mkdir/rename/remove/chmod/close) — in progress
- [ ] native: transfers (download/upload, recursive dirs, queue max 2/session, cancel, progress, mtime)
- [ ] native: openRemote (temp download + ShellExecute)
- [ ] native: builds warnings-clean (/W4)
- [ ] tests: native/tests/sftp (util unit tests + integration vs TestSshServer)
- [ ] ui: mock (ui/src/bridge/mock/sftp.ts)
- [ ] ui: SftpPanel + css
- [ ] ui: screenshot verification

## Requests
(none yet)
