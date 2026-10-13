#include "TestNet.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace sc::test::net {

void ensureWinsock() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) throw std::runtime_error("WSAStartup failed");
        // Never WSACleanup: test servers may live in static objects destroyed after main().
    });
}

Socket listenLoopback(int port, int& boundPort) {
    ensureWinsock();
    Socket s(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!s) throw std::runtime_error("socket() failed: " + std::to_string(WSAGetLastError()));
    // Exclusive use: never share a port with another (stale) listener.
    BOOL on = TRUE;
    setsockopt(s.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof(on));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<u_short>(port));
    if (::bind(s.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
        throw std::runtime_error("bind(127.0.0.1:" + std::to_string(port) + ") failed: " + std::to_string(WSAGetLastError()));
    if (::listen(s.get(), SOMAXCONN) != 0) throw std::runtime_error("listen() failed: " + std::to_string(WSAGetLastError()));
    boundPort = localPort(s.get());
    return s;
}

int localPort(SOCKET s) {
    sockaddr_storage ss{};
    int len = sizeof(ss);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&ss), &len) != 0) return 0;
    if (ss.ss_family == AF_INET) return ntohs(reinterpret_cast<const sockaddr_in*>(&ss)->sin_port);
    if (ss.ss_family == AF_INET6) return ntohs(reinterpret_cast<const sockaddr_in6*>(&ss)->sin6_port);
    return 0;
}

std::pair<std::string, int> peerAddress(SOCKET s) {
    sockaddr_storage ss{};
    int len = sizeof(ss);
    if (getpeername(s, reinterpret_cast<sockaddr*>(&ss), &len) != 0) return {"127.0.0.1", 0};
    char host[NI_MAXHOST] = {};
    char serv[NI_MAXSERV] = {};
    if (getnameinfo(reinterpret_cast<const sockaddr*>(&ss), len, host, sizeof(host), serv, sizeof(serv),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0)
        return {"127.0.0.1", 0};
    return {host, std::atoi(serv)};
}

bool setNonBlocking(SOCKET s, bool on) {
    u_long mode = on ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
}

bool wouldBlock() {
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEINTR;
}

bool waitReadable(SOCKET s, int timeoutMs) {
    WSAPOLLFD p{};
    p.fd = s;
    p.events = POLLRDNORM;
    const int rc = WSAPoll(&p, 1, timeoutMs);
    return rc > 0;
}

bool sendAll(SOCKET s, std::string_view data) {
    while (!data.empty()) {
        const int chunk = static_cast<int>(std::min<size_t>(data.size(), 1 << 20));
        const int n = ::send(s, data.data(), chunk, 0);
        if (n == SOCKET_ERROR) {
            if (wouldBlock()) {
                WSAPOLLFD p{};
                p.fd = s;
                p.events = POLLWRNORM;
                WSAPoll(&p, 1, 100);
                continue;
            }
            return false;
        }
        data.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

Socket acceptNonBlocking(SOCKET listener) {
    SOCKET c = ::accept(listener, nullptr, nullptr);
    return Socket(c);
}

Socket connectTcp(const std::string& host, int port, int timeoutMs) {
    ensureWinsock();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &res) != 0 || !res) return {};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    // IPv4 first: test listeners bind 127.0.0.1 only, and a refused connect to ::1 costs ~2 s on Windows.
    std::vector<addrinfo*> candidates;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) candidates.push_back(ai);
    std::stable_partition(candidates.begin(), candidates.end(), [](const addrinfo* ai) { return ai->ai_family == AF_INET; });
    Socket result;
    for (addrinfo* ai : candidates) {
        if (result) break;
        Socket s(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (!s) continue;
        setNonBlocking(s.get(), true);
        if (::connect(s.get(), ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            result = std::move(s);
            break;
        }
        if (WSAGetLastError() != WSAEWOULDBLOCK) continue;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) break;
        WSAPOLLFD p{};
        p.fd = s.get();
        p.events = POLLWRNORM;
        if (WSAPoll(&p, 1, static_cast<int>(left.count())) <= 0) continue;
        int err = 0;
        int len = sizeof(err);
        getsockopt(s.get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
        if (err == 0 && (p.revents & POLLWRNORM)) result = std::move(s);
    }
    freeaddrinfo(res);
    if (result) {
        setNonBlocking(result.get(), false);
        BOOL nodelay = TRUE;
        setsockopt(result.get(), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    }
    return result;
}

}  // namespace sc::test::net
