# net module notes

Owner: `net` agent. Files: `native/src/net/**`, `native/src/transport/{Telnet,Rlogin,Raw,Serial}*`,
`native/src/session/LineDiscipline.*`, `native/tests/net/**`.

## Status (resume checklist)

- [x] net/Socket.cpp — connectTcp (cancellable GetAddrInfoExW, staggered attempts, timeout/cancel), socket pair, listenTcp, Waker
- [ ] session/LineDiscipline.cpp
- [ ] net/Proxy.cpp — excludes, template, SOCKS4/4a/5, HTTP, Telnet, Local command, SSH jump
- [ ] net/SocketTransport (shared I/O loop for Telnet/Rlogin/Raw)
- [ ] RawTransport
- [ ] TelnetProtocol (pure engine) + TelnetTransport
- [ ] RloginTransport
- [ ] SerialTransport + listSerialPorts
- [ ] tests: pure (ldisc, excludes, template, telnet engine)
- [ ] tests: socket level (connectTcp, pair, listen, proxies, telnet, rlogin, raw, local command)
