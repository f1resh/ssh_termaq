// Tests for sockets, proxies, line discipline and the Telnet / Rlogin / Raw transports.
#include "TestNet.h"
#include "TestServers.h"
#include "TestTransport.h"

#include "net/Proxy.h"
#include "net/Socket.h"
#include "session/LineDiscipline.h"
#include "transport/RawTransport.h"
#include "transport/RloginTransport.h"
#include "transport/TelnetTransport.h"

#include <catch2/catch_test_macros.hpp>

#include <thread>

using namespace st;
using namespace st::test;
using namespace std::chrono_literals;

namespace {

struct Ldisc {
    std::string remote, term;
    LineDiscipline ld;
    Ldisc(TriState echo, TriState edit, LdiscModes modes)
        : ld(echo, edit, modes, {[this](const std::string& s) { remote += s; }, [this](const std::string& s) { term += s; }}) {}
};

SessionConfig tcpSession(Protocol p, int port) {
    SessionConfig s = makeDefaultSession();
    s.protocol = p;
    s.host = "127.0.0.1";
    s.port = port;
    s.username = "alice";
    s.connection.keepaliveSeconds = 0;
    return s;
}

TransportContext context(const SessionConfig& s, RecordingListener& l) {
    TransportContext c;
    c.connId = "t#1";
    c.session = s;
    c.cols = 100;
    c.rows = 30;
    c.listener = &l;
    return c;
}

const std::string IAC = "\xff";

}  // namespace

/* ---- line discipline --------------------------------------------------------------------------- */

TEST_CASE("ldisc: forced off passes input straight through", "[net][ldisc]") {
    Ldisc l(TriState::Off, TriState::Off, {true, true});
    l.ld.input("abc\r");
    REQUIRE(l.remote == "abc\r");
    REQUIRE(l.term.empty());
}

TEST_CASE("ldisc: auto follows the transport modes", "[net][ldisc]") {
    Ldisc l(TriState::Auto, TriState::Auto, {false, false});
    REQUIRE_FALSE(l.ld.echoing());
    l.ld.setTransportModes({true, true});
    REQUIRE(l.ld.echoing());
    REQUIRE(l.ld.editing());
}

TEST_CASE("ldisc: line editing buffers until Enter and handles erase keys", "[net][ldisc]") {
    Ldisc l(TriState::On, TriState::On, {false, false});
    l.ld.input("helo");
    REQUIRE(l.remote.empty());
    l.ld.input("\x7f" "lo world");
    l.ld.input("\x17");  // ^W erases "world"
    l.ld.input("there\r");
    REQUIRE(l.remote.rfind("hello there", 0) == 0);
    REQUIRE(l.term.find("\b \b") != std::string::npos);
}

TEST_CASE("ldisc: backspace removes a whole UTF-8 character", "[net][ldisc]") {
    Ldisc l(TriState::On, TriState::On, {false, false});
    l.ld.input("a\xC3\xA9");  // a é
    l.ld.input("\x7f\r");
    REQUIRE(l.remote.rfind("a", 0) == 0);
    REQUIRE(l.remote.find("\xC3") == std::string::npos);
}

TEST_CASE("ldisc: ^U kills the line, ^C is sent immediately", "[net][ldisc]") {
    Ldisc l(TriState::On, TriState::On, {false, false});
    l.ld.input("secret\x15ok\r");
    REQUIRE(l.remote.rfind("ok", 0) == 0);
    l.remote.clear();
    l.ld.input("abc\x03");
    REQUIRE(l.remote.find('\x03') != std::string::npos);
}

TEST_CASE("ldisc: echo without editing", "[net][ldisc]") {
    Ldisc l(TriState::On, TriState::Off, {false, false});
    l.ld.input("hi\r");
    REQUIRE(l.remote == "hi\r");
    REQUIRE(l.term.find("hi") != std::string::npos);
}

/* ---- proxy helpers ------------------------------------------------------------------------------ */

TEST_CASE("proxy: exclusions and localhost", "[net][proxy]") {
    ProxyConfig p;
    p.type = ProxyType::Socks5;
    p.excludeHosts = "*.internal, 10.*  example.org";
    REQUIRE(proxyExcludes(p, "db.internal"));
    REQUIRE(proxyExcludes(p, "10.1.2.3"));
    REQUIRE(proxyExcludes(p, "EXAMPLE.org"));
    REQUIRE_FALSE(proxyExcludes(p, "example.com"));
    REQUIRE(proxyExcludes(p, "localhost"));
    REQUIRE(proxyExcludes(p, "127.0.0.1"));
    p.proxyLocalhost = true;
    REQUIRE_FALSE(proxyExcludes(p, "127.0.0.1"));
}

TEST_CASE("proxy: telnet/command template expansion", "[net][proxy]") {
    ProxyConfig p;
    p.host = "proxy.example";
    p.port = 8080;
    p.username = "bob";
    p.password = "pw";
    REQUIRE(expandProxyTemplate("connect %host %port\\n", p, "target", 22) == "connect target 22\n");
    REQUIRE(expandProxyTemplate("%user:%pass@%proxyhost:%proxyport %%", p, "t", 1) == "bob:pw@proxy.example:8080 %");
    REQUIRE(expandProxyTemplate("\\x41\\t\\\\", p, "t", 1) == "A\t\\");
}

TEST_CASE("sockets: connect refused, pair and listen", "[net]") {
    int port = 0;
    { auto l = net::listenLoopback(0, port); }  // closed again: nothing listens there now
    std::string err;
    ConnectOptions o;
    o.timeoutMs = 3000;
    SOCKET s = connectTcp("127.0.0.1", port, o, err);
    REQUIRE(s == INVALID_SOCKET);
    REQUIRE_FALSE(err.empty());

    SOCKET a = INVALID_SOCKET, b = INVALID_SOCKET;
    REQUIRE(makeSocketPair(a, b, err));
    REQUIRE(sendAll(a, "pair"));
    char buf[8] = {};
    REQUIRE(recv(b, buf, 4, 0) == 4);
    REQUIRE(std::string(buf, 4) == "pair");
    closeSocket(a);
    closeSocket(b);

    int bound = 0;
    SOCKET l = listenTcp("", 0, AddressFamily::IPv4, err, &bound);
    REQUIRE(l != INVALID_SOCKET);
    REQUIRE(bound > 0);
    closeSocket(l);
}

TEST_CASE("proxy: SOCKS5 handshake is byte-exact", "[net][proxy]") {
    TestScriptServer fakeProxy;
    RecordingListener l;
    SessionConfig s = tcpSession(Protocol::Raw, 2323);
    s.host = "target.example";
    s.proxy.type = ProxyType::Socks5;
    s.proxy.host = "127.0.0.1";
    s.proxy.port = fakeProxy.port();
    s.proxy.username = "u";
    s.proxy.password = "p";
    auto t = makeRawTransport(context(s, l));
    t->start();
    REQUIRE(fakeProxy.waitForConnection(3000));
    // Greeting: version 5, offering no-auth and username/password.
    REQUIRE(fakeProxy.waitForReceived(std::string("\x05", 1), 3000));
    std::this_thread::sleep_for(100ms);
    const std::string greeting = fakeProxy.received();
    REQUIRE(greeting[0] == 5);
    fakeProxy.send(std::string("\x05\x02", 2));  // choose username/password
    REQUIRE(fakeProxy.waitForReceived(std::string("\x01\x01u\x01p", 5), 3000));
    fakeProxy.send(std::string("\x01\x00", 2));  // auth ok
    // CONNECT request with the domain name (DNS at proxy).
    const std::string req = std::string("\x05\x01\x00\x03", 4) + char(14) + "target.example" + std::string("\x09\x13", 2);
    REQUIRE(fakeProxy.waitForReceived(req, 3000));
    fakeProxy.send(std::string("\x05\x00\x00\x01\x7f\x00\x00\x01\x00\x00", 10));
    REQUIRE(l.waitConnected());
    fakeProxy.send("hello through socks");
    REQUIRE(l.waitData("hello through socks"));
    t->write("ping");
    REQUIRE(fakeProxy.waitForReceived("ping", 3000));
}

/* ---- raw / telnet / rlogin -------------------------------------------------------------------- */

TEST_CASE("raw: bytes pass through both ways", "[net][raw]") {
    TestScriptServer srv;
    RecordingListener l;
    auto t = makeRawTransport(context(tcpSession(Protocol::Raw, srv.port()), l));
    REQUIRE(t->defaultLdisc().echo);
    t->start();
    REQUIRE(l.waitConnected());
    REQUIRE(srv.waitForConnection(3000));
    srv.send(std::string("\x00\xff\r\nok", 6));
    REQUIRE(l.waitData("ok"));
    REQUIRE(l.data.substr(0, 2) == std::string("\x00\xff", 2));
    t->write("abc\r");
    REQUIRE(srv.waitForReceived("abc\r", 3000));
    srv.closeClient();
    REQUIRE(l.waitClosed());
    REQUIRE(l.closeCount == 1);
}

TEST_CASE("telnet: active negotiation, TTYPE, NAWS, ECHO and IAC escaping", "[net][telnet]") {
    TestScriptServer srv;
    RecordingListener l;
    SessionConfig s = tcpSession(Protocol::Telnet, srv.port());
    s.connection.terminalType = "xterm-256color";
    auto t = makeTelnetTransport(context(s, l));
    t->start();
    REQUIRE(srv.waitForConnection(3000));
    // Client opens with WILL NAWS / WILL TTYPE / DO ECHO ...
    REQUIRE(srv.waitForReceived(IAC + "\xfb\x1f", 3000));  // WILL NAWS
    REQUIRE(srv.waitForReceived(IAC + "\xfb\x18", 3000));  // WILL TTYPE
    REQUIRE(srv.waitForReceived(IAC + "\xfd\x01", 3000));  // DO ECHO
    // Server: DO NAWS -> client sends the window size (100 x 30).
    srv.send(IAC + "\xfd\x1f");
    REQUIRE(srv.waitForReceived(IAC + "\xfa\x1f" + std::string("\x00\x64\x00\x1e", 4) + IAC + "\xf0", 3000));
    // Server: DO TTYPE + SB TTYPE SEND -> IS xterm-256color.
    srv.send(IAC + "\xfd\x18" + IAC + "\xfa\x18\x01" + IAC + "\xf0");
    REQUIRE(srv.waitForReceived(IAC + "\xfa\x18" + std::string("\x00", 1) + "xterm-256color" + IAC + "\xf0", 3000));
    // Server: WILL ECHO -> local echo off.
    srv.send(IAC + "\xfb\x01");
    REQUIRE(l.waitFor([&] { return !l.ldisc.empty() && !l.ldisc.back().echo; }));
    // Data with an escaped 0xFF and CR NUL.
    srv.send("x" + IAC + IAC + "y\r" + std::string("\x00", 1) + "z");
    REQUIRE(l.waitData("z"));
    REQUIRE(l.data.find("x\xffy\rz") != std::string::npos);
    // Resize -> NAWS again; user 0xFF is doubled; Enter -> CR LF.
    t->resize(120, 40, 0, 0);
    REQUIRE(srv.waitForReceived(std::string("\x00\x78\x00\x28", 4), 3000));
    t->write("a\xff" "b\r");
    REQUIRE(srv.waitForReceived("a" + IAC + IAC + "b\r\n", 3000));
    // Special: Are You There.
    t->sendSpecial("telnet:ayt");
    REQUIRE(srv.waitForReceived(IAC + "\xf6", 3000));
}

TEST_CASE("telnet: unsupported options are refused", "[net][telnet]") {
    TestScriptServer srv;
    RecordingListener l;
    SessionConfig s = tcpSession(Protocol::Telnet, srv.port());
    s.telnet.passive = true;
    auto t = makeTelnetTransport(context(s, l));
    t->start();
    REQUIRE(srv.waitForConnection(3000));
    srv.send(IAC + "\xfd\x22" + IAC + "\xfb\x05");  // DO LINEMODE, WILL STATUS
    REQUIRE(srv.waitForReceived(IAC + "\xfc\x22", 3000));  // WONT LINEMODE
    REQUIRE(srv.waitForReceived(IAC + "\xfe\x05", 3000));  // DONT STATUS
}

TEST_CASE("rlogin: handshake and window size on request", "[net][rlogin]") {
    TestScriptServer srv;
    RecordingListener l;
    SessionConfig s = tcpSession(Protocol::Rlogin, srv.port());
    s.rlogin.localUsername = "localbob";
    s.connection.terminalType = "vt100";
    s.connection.terminalSpeed = "9600,9600";
    auto t = makeRloginTransport(context(s, l));
    t->start();
    REQUIRE(srv.waitForConnection(3000));
    const std::string hs = std::string("\0", 1) + "localbob" + std::string("\0", 1) + "alice" + std::string("\0", 1) + "vt100/9600" +
                           std::string("\0", 1);
    REQUIRE(srv.waitForReceived(hs, 3000));
    srv.send(std::string("\0", 1) + "Welcome");
    REQUIRE(l.waitData("Welcome"));
    REQUIRE(l.data.find('\0') == std::string::npos);  // the ack byte is swallowed
}
