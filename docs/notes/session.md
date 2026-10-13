# session module notes

Owner files: `native/src/session/{SessionManager,Charset,SessionLog}.*`, `native/src/transport/TransportFactory.*`,
`native/tests/session/**`. Library `sc_session`, tests `sc_test_session`.

## Status

- [x] Charset: name -> code page (aliases), streaming Decoder (UTF-8 / single-byte / built-in ISO tables / DBCS / GB18030), encodeFromUtf8, binaryStringToBytes
- [x] SessionLog: expandPath, dirs, append/overwrite/ask, header, printable/all/ssh-debug, flush, stateful stripEscapes, localOutput (added)
- [x] SessionManager: open/validate, starter thread (touch, log open, start), data batching (8 ms / 64 KiB), status ordering, ldisc input, prompts, event log ring, closeAll, reaper, bridge handlers (builds; tests pending)
- [x] TransportFactory (done in scaffold)
- [ ] Tests (todo)

## Requests

(none yet)
