// The socket helpers of TestNet.h that run on the platform layer; the raw ones are in
// native/tests/platform/<os>/support.
#include "TestNet.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace st::test::net {

void ensureNetInit() {
    // Never torn down: test servers may live in static objects destroyed after main().
    static sys::NetInit* init = new sys::NetInit();
    (void)init;
}

Socket listenLoopback(int port, int& boundPort) {
    ensureNetInit();
    std::string error;
    // Exclusive use: never share a port with another (stale) listener (sys::listenTcp sees to that per OS).
    Socket s(sys::listenTcp("127.0.0.1", port, sys::IpFamily::IPv4, error, &boundPort));
    if (!s) throw std::runtime_error("listen on 127.0.0.1:" + std::to_string(port) + " failed: " + error);
    return s;
}

int localPort(Handle s) { return sys::localPort(s); }

std::pair<std::string, int> peerAddress(Handle s) {
    auto peer = sys::peerAddress(s);
    if (!peer) return {"127.0.0.1", 0};
    return *peer;
}

bool setNonBlocking(Handle s, bool on) {
    sys::setNonBlocking(s, on);
    return true;
}

bool wouldBlock() { return sys::lastNetError() == sys::NetErr::WouldBlock; }

bool waitReadable(Handle s, int timeoutMs) { return sys::waitReadable(s, timeoutMs) > 0; }

bool sendAll(Handle s, std::string_view data) { return sys::sendAll(s, data); }

Socket acceptNonBlocking(Handle listener) { return Socket(sys::acceptConnection(listener, false)); }

Socket connectTcp(const std::string& host, int port, int timeoutMs) {
    ensureNetInit();
    std::string error;
    std::vector<std::string> addrs = sys::resolveHost(host, sys::IpFamily::Auto, error, nullptr, timeoutMs);
    // IPv4 first: test listeners bind 127.0.0.1 only, and a refused connect to ::1 can cost seconds.
    std::stable_partition(addrs.begin(), addrs.end(), [](const std::string& a) { return a.find(':') == std::string::npos; });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (const auto& a : addrs) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) break;
        sys::ConnectOptions o;
        o.timeoutMs = static_cast<int>(left.count());
        Socket s(sys::connectTcp(a, port, o, error));  // blocking, TCP_NODELAY
        if (s) return s;
    }
    return {};
}

Socket acceptWithin(Handle listener, int timeoutMs) {
    if (!waitReadable(listener, timeoutMs)) return {};
    return Socket(sys::acceptConnection(listener, false));
}

int recvWithin(Handle s, char* buf, int n, int timeoutMs) {
    if (!waitReadable(s, timeoutMs)) return -1;
    const long got = sys::recvSome(s, buf, static_cast<std::size_t>(n));
    return got < 0 ? -1 : static_cast<int>(got);
}

bool sendUrgent(Handle s, unsigned char byte) {
    const char b = static_cast<char>(byte);
    return sys::sendSome(s, &b, 1, true) == 1;
}

void shutdownSend(Handle s) { sys::shutdownWrite(s); }

}  // namespace st::test::net
