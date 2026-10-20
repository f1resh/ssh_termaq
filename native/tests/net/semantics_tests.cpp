// How the socket transports behave at the edges of a TCP stream: partial writes to a slow reader, FIN and RST from
// the peer, urgent data (Telnet Synch, rlogin control bytes), many concurrent writers, many open sockets, the local
// proxy command. These pin down what every OS's socket layer has to reproduce (platform/sys/Net.h); they were written
// against the original Winsock implementation.
#include "TestNet.h"
#include "TestTransport.h"

#include "platform/Platform.h"
#include "transport/RawTransport.h"
#include "transport/RloginTransport.h"
#include "transport/TelnetTransport.h"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <thread>
#include <vector>

using namespace st;
using namespace st::test;
using namespace std::chrono_literals;

namespace {

SessionConfig sessionFor(Protocol p, int port) {
    SessionConfig s = makeDefaultSession();
    s.protocol = p;
    s.host = "127.0.0.1";
    s.port = port;
    s.username = "alice";
    s.connection.keepaliveSeconds = 0;
    return s;
}

TransportContext contextFor(const SessionConfig& s, RecordingListener& l) {
    TransportContext c;
    c.connId = "edge#1";
    c.session = s;
    c.cols = 100;
    c.rows = 30;
    c.listener = &l;
    return c;
}

/** The far end of the connection: a loopback listener the transport connects to. */
struct Peer {
    net::Socket listener;
    net::Socket conn;
    int port = 0;

    Peer() { listener = net::listenLoopback(0, port); }

    bool accept() {
        conn = net::acceptWithin(listener.get(), 5000);
        return conn.valid();
    }
    /** Reads until `n` bytes arrived or `timeoutMs` passed. */
    std::string readExactly(size_t n, int timeoutMs) {
        std::string out;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        std::vector<char> buf(65536);
        while (out.size() < n && std::chrono::steady_clock::now() < deadline) {
            const int got = net::recvWithin(conn.get(), buf.data(), static_cast<int>(buf.size()), 200);
            if (got == 0) break;
            if (got > 0) out.append(buf.data(), static_cast<size_t>(got));
        }
        return out;
    }
    /** Reads until the transport half-closes (FIN); false on timeout. */
    bool readUntilEof(std::string& out, int timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        char buf[4096];
        while (std::chrono::steady_clock::now() < deadline) {
            const int got = net::recvWithin(conn.get(), buf, sizeof buf, 200);
            if (got == 0) return true;
            if (got > 0) out.append(buf, static_cast<size_t>(got));
        }
        return false;
    }
    /** Discards whatever arrived within `ms` (protocol negotiation). */
    std::string drain(int ms) {
        std::string out;
        char buf[4096];
        int got;
        while ((got = net::recvWithin(conn.get(), buf, sizeof buf, ms)) > 0) out.append(buf, static_cast<size_t>(got));
        return out;
    }
};

/** CPU seconds this process uses while the transports sit idle for `wait`. */
double idleCpu(std::chrono::milliseconds wait) {
    const double before = net::processCpuSeconds();
    std::this_thread::sleep_for(wait);
    return net::processCpuSeconds() - before;
}

std::string pattern(size_t n) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 131 + (i >> 13)) & 0xFF);
    return s;
}

bool isWindows() { return std::string(sys::osName()) == "win32"; }

}  // namespace

TEST_CASE("edge: a large write to a slow reader arrives complete and in order", "[net][edge]") {
    Peer peer;
    net::setReceiveBuffer(peer.listener.get(), 4096);
    RecordingListener l;
    auto t = makeRawTransport(contextFor(sessionFor(Protocol::Raw, peer.port), l));
    t->start();
    REQUIRE(peer.accept());
    REQUIRE(l.waitConnected());
    const std::string big = pattern(8u << 20);
    t->write(big);
    std::this_thread::sleep_for(300ms);  // the socket buffers are full: the transport has to wait for room
    const std::string got = peer.readExactly(big.size(), 30000);
    REQUIRE(got.size() == big.size());
    REQUIRE(got == big);
    REQUIRE_FALSE(l.closed);
}

TEST_CASE("edge: an idle connection does not use the CPU", "[net][edge]") {
    Peer peer;
    RecordingListener l;
    auto t = makeRawTransport(contextFor(sessionFor(Protocol::Raw, peer.port), l));
    t->start();
    REQUIRE(peer.accept());
    REQUIRE(l.waitConnected());
    t->write("one write, then quiet");
    REQUIRE(peer.readExactly(21, 3000) == "one write, then quiet");
    CHECK(idleCpu(1500ms) < 0.5);
}

TEST_CASE("edge: data followed by FIN is delivered completely, then a clean close", "[net][edge]") {
    Peer peer;
    RecordingListener l;
    auto t = makeRawTransport(contextFor(sessionFor(Protocol::Raw, peer.port), l));
    t->start();
    REQUIRE(peer.accept());
    REQUIRE(l.waitConnected());
    const std::string payload = pattern(1u << 20);
    REQUIRE(net::sendAll(peer.conn.get(), payload));
    net::shutdownSend(peer.conn.get());
    REQUIRE(l.waitClosed(15s));
    REQUIRE(l.data.size() == payload.size());
    REQUIRE(l.data == payload);
    REQUIRE(l.info.clean);
    REQUIRE_FALSE(l.info.error);
    REQUIRE(l.closeCount == 1);
}

TEST_CASE("edge: a connection reset is an error, after the data that came before it", "[net][edge]") {
    Peer peer;
    RecordingListener l;
    auto t = makeRawTransport(contextFor(sessionFor(Protocol::Raw, peer.port), l));
    t->start();
    REQUIRE(peer.accept());
    REQUIRE(l.waitConnected());
    REQUIRE(net::sendAll(peer.conn.get(), "before the reset"));
    REQUIRE(l.waitData("before the reset"));
    net::abortiveClose(peer.conn);
    REQUIRE(l.waitClosed());
    REQUIRE(l.info.error);
    REQUIRE_FALSE(l.info.clean);
    // Writing into the dead connection must not crash the process or report a second close.
    t->write(std::string(256 * 1024, 'y'));
    std::this_thread::sleep_for(300ms);
    REQUIRE(l.closeCount == 1);
}

TEST_CASE("edge: writing while the peer resets the connection fails cleanly", "[net][edge]") {
    Peer peer;
    RecordingListener l;
    auto t = makeRawTransport(contextFor(sessionFor(Protocol::Raw, peer.port), l));
    t->start();
    REQUIRE(peer.accept());
    REQUIRE(l.waitConnected());
    net::abortiveClose(peer.conn);
    for (int i = 0; i < 20; ++i) t->write(std::string(64 * 1024, 'z'));  // races the transport noticing the reset
    REQUIRE(l.waitClosed());
    std::this_thread::sleep_for(200ms);
    REQUIRE(l.closeCount == 1);
}

TEST_CASE("edge: Raw EOF half-closes the sending side and keeps receiving", "[net][edge]") {
    Peer peer;
    RecordingListener l;
    auto t = makeRawTransport(contextFor(sessionFor(Protocol::Raw, peer.port), l));
    t->start();
    REQUIRE(peer.accept());
    REQUIRE(l.waitConnected());
    t->write("last words");
    t->sendSpecial("eof");
    std::string got;
    REQUIRE(peer.readUntilEof(got, 5000));
    REQUIRE(got == "last words");
    REQUIRE(net::sendAll(peer.conn.get(), "a reply after EOF"));
    REQUIRE(l.waitData("a reply after EOF"));
    REQUIRE_FALSE(l.closed);
}

TEST_CASE("edge: Telnet Synch sends IAC normally and the DM byte as urgent data", "[net][edge][telnet]") {
    Peer peer;
    RecordingListener l;
    auto t = makeTelnetTransport(contextFor(sessionFor(Protocol::Telnet, peer.port), l));
    t->start();
    REQUIRE(peer.accept());
    REQUIRE(l.waitConnected());
    peer.drain(300);  // the transport's option negotiation
    t->sendSpecial("telnet:synch");
    REQUIRE(net::recvUrgent(peer.conn.get(), 5000) == 0xF2);  // DM
    REQUIRE(peer.drain(300).find('\xFF') != std::string::npos);  // IAC in the normal stream
}

TEST_CASE("edge: Telnet reads urgent data inline, once, without spinning", "[net][edge][telnet]") {
    Peer peer;
    RecordingListener l;
    auto t = makeTelnetTransport(contextFor(sessionFor(Protocol::Telnet, peer.port), l));
    t->start();
    REQUIRE(peer.accept());
    REQUIRE(l.waitConnected());
    peer.drain(300);
    REQUIRE(net::sendAll(peer.conn.get(), std::string("a\xFF", 2)));  // "a", IAC
    REQUIRE(net::sendUrgent(peer.conn.get(), 0xF2));                     // DM as urgent data (Synch)
    REQUIRE(net::sendAll(peer.conn.get(), "b"));
    REQUIRE(l.waitData("b"));
    std::this_thread::sleep_for(200ms);
    REQUIRE(l.data == "ab");  // IAC DM is consumed, nothing is delivered twice
    CHECK(idleCpu(1000ms) < 0.4);
}

TEST_CASE("edge: rlogin's urgent 0x80 asks for the window size, exactly once", "[net][edge][rlogin]") {
    Peer peer;
    RecordingListener l;
    SessionConfig s = sessionFor(Protocol::Rlogin, peer.port);
    s.rlogin.localUsername = "localbob";
    s.connection.terminalType = "vt100";
    s.connection.terminalSpeed = "9600,9600";
    auto t = makeRloginTransport(contextFor(s, l));
    t->start();
    REQUIRE(peer.accept());
    const std::string hs = std::string("\0localbob\0alice\0vt100/9600\0", 27);
    REQUIRE(peer.readExactly(hs.size(), 5000) == hs);
    REQUIRE(net::sendAll(peer.conn.get(), std::string("\0x", 2)));  // ack byte, then data
    REQUIRE(l.waitData("x"));
    REQUIRE(net::sendUrgent(peer.conn.get(), 0x80));
    const std::string expected("\xFF\xFFss\x00\x1E\x00\x64\x00\x00\x00\x00", 12);  // 30 rows, 100 columns
    REQUIRE(peer.readExactly(expected.size(), 5000) == expected);
    REQUIRE(peer.drain(500).empty());  // asked once, answered once
    REQUIRE(l.data == "x");            // the control byte is not terminal data
    CHECK(idleCpu(1000ms) < 0.4);
}

TEST_CASE("edge: writes from several threads all arrive, each thread's in order", "[net][edge]") {
    Peer peer;
    RecordingListener l;
    auto t = makeRawTransport(contextFor(sessionFor(Protocol::Raw, peer.port), l));
    t->start();
    REQUIRE(peer.accept());
    REQUIRE(l.waitConnected());
    constexpr int kThreads = 4, kWrites = 2000;
    std::vector<std::thread> writers;
    for (int w = 0; w < kThreads; ++w) {
        writers.emplace_back([&t, w] {
            for (int i = 0; i < kWrites; ++i) t->write(std::to_string(w) + ":" + std::to_string(i) + "\n");
        });
    }
    for (auto& w : writers) w.join();
    std::map<int, int> next;
    std::string buffer;
    int lines = 0;
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    char buf[65536];
    while (lines < kThreads * kWrites && std::chrono::steady_clock::now() < deadline) {
        const int got = net::recvWithin(peer.conn.get(), buf, sizeof buf, 200);
        if (got <= 0) continue;
        buffer.append(buf, static_cast<size_t>(got));
        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            const std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            const size_t colon = line.find(':');
            REQUIRE(colon != std::string::npos);
            const int w = std::stoi(line.substr(0, colon)), i = std::stoi(line.substr(colon + 1));
            REQUIRE(i == next[w]);
            next[w] = i + 1;
            ++lines;
        }
    }
    REQUIRE(lines == kThreads * kWrites);
}

TEST_CASE("edge: connections work while many other sockets are open", "[net][edge]") {
    std::vector<net::Socket> others;
    for (int i = 0; i < 1100; ++i) {
        int unused = 0;
        others.push_back(net::listenLoopback(0, unused));
    }
    Peer peer;
    RecordingListener l;
    auto t = makeRawTransport(contextFor(sessionFor(Protocol::Raw, peer.port), l));
    t->start();
    REQUIRE(peer.accept());
    REQUIRE(l.waitConnected());
    t->write("ping");
    REQUIRE(peer.readExactly(4, 5000) == "ping");
    REQUIRE(net::sendAll(peer.conn.get(), "pong"));
    REQUIRE(l.waitData("pong"));
}

TEST_CASE("edge: a proxy command's output reaches the session, its errors the event log", "[net][edge][proxy]") {
    RecordingListener l;
    SessionConfig s = sessionFor(Protocol::Raw, 22);
    s.host = "target.example";
    s.proxy.type = ProxyType::Command;
    // stderr first, and a second before exiting: the relay stops logging once the session has closed.
    s.proxy.localCommand = isWindows() ? "echo oops 1>&2& echo hello from proxy& ping -n 2 127.0.0.1 >nul"
                                       : "echo oops 1>&2; echo hello from proxy; sleep 1";
    auto t = makeRawTransport(contextFor(s, l));
    t->start();
    REQUIRE(l.waitData("hello from proxy"));
    REQUIRE(l.waitClosed());  // the command exited: its end of the stream closed
    REQUIRE(l.info.clean);
    REQUIRE(l.allLogs().find("proxy: oops") != std::string::npos);
}

TEST_CASE("edge: closing the session ends a running proxy command promptly", "[net][edge][proxy]") {
    RecordingListener l;
    SessionConfig s = sessionFor(Protocol::Raw, 22);
    s.host = "target.example";
    s.proxy.type = ProxyType::Command;
    s.proxy.localCommand = isWindows() ? "echo started& ping -n 60 127.0.0.1 >nul" : "echo started; sleep 60";
    auto t = makeRawTransport(contextFor(s, l));
    t->start();
    REQUIRE(l.waitData("started"));
    const auto begin = std::chrono::steady_clock::now();
    t->close();
    REQUIRE(l.waitClosed(5s));
    t.reset();  // joins the transport thread
    REQUIRE(std::chrono::steady_clock::now() - begin < 5s);
}
