// Winsock helpers: resolution (cancellable GetAddrInfoExW), connect with timeout/cancel (staggered parallel
// attempts across addresses, RFC 8305 style), listeners, socket pairs and small utilities.
#include "net/Socket.h"

#include "core/Strings.h"

#include <mstcpip.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>

namespace sc {

namespace {

using Clock = std::chrono::steady_clock;

/** Delay before starting a parallel attempt to the next address while earlier ones are still pending. */
constexpr int kAttemptStaggerMs = 250;
/** Upper bound of a single wait slice (cancel polling granularity). */
constexpr int kSliceMs = 100;

struct ResolvedAddr {
    sockaddr_storage addr{};
    int len = 0;
    int family = AF_UNSPEC;
};

struct AddrInfoExDeleter {
    void operator()(ADDRINFOEXW* p) const {
        if (p) FreeAddrInfoExW(p);
    }
};
using AddrInfoExPtr = std::unique_ptr<ADDRINFOEXW, AddrInfoExDeleter>;

struct HandleCloser {
    void operator()(HANDLE h) const {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

int toAf(AddressFamily f) {
    switch (f) {
        case AddressFamily::IPv4: return AF_INET;
        case AddressFamily::IPv6: return AF_INET6;
        default: return AF_UNSPEC;
    }
}

std::string familySuffix(AddressFamily f) {
    switch (f) {
        case AddressFamily::IPv4: return " (IPv4)";
        case AddressFamily::IPv6: return " (IPv6)";
        default: return {};
    }
}

/** Numeric host of a sockaddr (no brackets, no port). */
std::string numericHost(const sockaddr* sa, int len) {
    wchar_t host[NI_MAXHOST] = {};
    if (GetNameInfoW(sa, len, host, NI_MAXHOST, nullptr, 0, NI_NUMERICHOST) != 0) return {};
    return narrow(host);
}

int sockaddrPort(const sockaddr* sa) {
    if (sa->sa_family == AF_INET) return ntohs(reinterpret_cast<const sockaddr_in*>(sa)->sin_port);
    if (sa->sa_family == AF_INET6) return ntohs(reinterpret_cast<const sockaddr_in6*>(sa)->sin6_port);
    return 0;
}

std::string formatSockaddr(const sockaddr* sa, int len) {
    std::string h = numericHost(sa, len);
    if (h.empty()) return {};
    return formatHostPort(h, sockaddrPort(sa));
}

/** Socket that is not inherited by child processes (local proxy commands must not hold our sockets). */
SOCKET createSocket(int family) {
    return WSASocketW(family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
}

void noInherit(SOCKET s) {
    if (s != INVALID_SOCKET) SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
}

void setBoolOpt(SOCKET s, int level, int name, bool on) {
    BOOL v = on ? TRUE : FALSE;
    setsockopt(s, level, name, reinterpret_cast<const char*>(&v), sizeof(v));
}

bool cancelled(const std::atomic<bool>* cancel) { return cancel && cancel->load(); }

/** Milliseconds until `deadline` (clamped to [0, cap]); `cap` when there is no deadline. */
int msUntil(const std::optional<Clock::time_point>& deadline, int cap) {
    if (!deadline) return cap;
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - Clock::now()).count();
    if (left <= 0) return 0;
    return static_cast<int>(std::min<long long>(left, cap));
}

/**
 * Asynchronous GetAddrInfoExW so that resolution can be cancelled / timed out. Fills `out` in resolver order.
 */
bool resolveAddrs(const std::string& hostIn, int port, AddressFamily family, std::vector<ResolvedAddr>& out,
                  std::string& error, const std::atomic<bool>* cancel, const std::optional<Clock::time_point>& deadline) {
    out.clear();
    const std::string host = stripBrackets(hostIn);
    if (host.empty()) {
        error = "No host name specified";
        return false;
    }
    const std::wstring whost = widen(host);
    const std::wstring wport = std::to_wstring(port);

    ADDRINFOEXW hints{};
    hints.ai_family = toAf(family);
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    ADDRINFOEXW* raw = nullptr;
    UniqueHandle ev(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!ev) {
        error = "Unable to resolve host: " + lastErrorMessage();
        return false;
    }
    OVERLAPPED ov{};
    ov.hEvent = ev.get();
    HANDLE cancelHandle = nullptr;
    INT rc = GetAddrInfoExW(whost.c_str(), wport.c_str(), NS_ALL, nullptr, &hints, &raw, nullptr, &ov, nullptr,
                            &cancelHandle);
    if (rc == WSA_IO_PENDING) {
        bool aborted = false;
        bool timedOut = false;
        for (;;) {
            DWORD w = WaitForSingleObject(ev.get(), static_cast<DWORD>(msUntil(deadline, kSliceMs)));
            if (w == WAIT_OBJECT_0) break;
            if (cancelled(cancel)) aborted = true;
            if (deadline && Clock::now() >= *deadline) timedOut = true;
            if (aborted || timedOut) {
                GetAddrInfoExCancel(&cancelHandle);
                WaitForSingleObject(ev.get(), INFINITE);  // completion is signalled promptly after cancel
                break;
            }
        }
        rc = GetAddrInfoExOverlappedResult(&ov);
        if (aborted || timedOut) {
            if (raw) FreeAddrInfoExW(raw);
            error = aborted ? "Cancelled" : "Timed out resolving host \"" + host + "\"";
            return false;
        }
    }
    AddrInfoExPtr result(raw);
    if (rc != NO_ERROR) {
        error = "Unable to resolve host \"" + host + "\": " + winErrorMessage(static_cast<unsigned long>(rc));
        return false;
    }
    for (ADDRINFOEXW* ai = result.get(); ai; ai = ai->ai_next) {
        if (!ai->ai_addr || ai->ai_addrlen > sizeof(sockaddr_storage)) continue;
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) continue;
        ResolvedAddr r;
        memcpy(&r.addr, ai->ai_addr, ai->ai_addrlen);
        r.len = static_cast<int>(ai->ai_addrlen);
        r.family = ai->ai_family;
        out.push_back(r);
    }
    if (out.empty()) {
        error = "Unable to resolve host \"" + host + "\": no usable address" + familySuffix(family);
        return false;
    }
    return true;
}

/** RFC 8305 §4: interleave address families, starting with the family of the first resolver answer. */
std::vector<ResolvedAddr> interleave(const std::vector<ResolvedAddr>& in) {
    if (in.empty()) return {};
    std::vector<ResolvedAddr> first, second;
    const int firstFamily = in.front().family;
    for (const auto& a : in) (a.family == firstFamily ? first : second).push_back(a);
    std::vector<ResolvedAddr> out;
    size_t i = 0, j = 0;
    while (i < first.size() || j < second.size()) {
        if (i < first.size()) out.push_back(first[i++]);
        if (j < second.size()) out.push_back(second[j++]);
    }
    return out;
}

struct Attempt {
    SOCKET s = INVALID_SOCKET;
    ResolvedAddr addr;
    std::string text;  // numeric host (no port)
};

/** Binds `s` to a local port in [low, high] (descending, starting at *cursor). Returns the port or 0. */
int bindLocalPortRange(SOCKET s, int family, int high, int low, int& cursor) {
    for (; cursor >= low && cursor <= high; --cursor) {
        sockaddr_storage local{};
        int len = 0;
        if (family == AF_INET6) {
            auto* a = reinterpret_cast<sockaddr_in6*>(&local);
            a->sin6_family = AF_INET6;
            a->sin6_addr = in6addr_any;
            a->sin6_port = htons(static_cast<u_short>(cursor));
            len = sizeof(sockaddr_in6);
        } else {
            auto* a = reinterpret_cast<sockaddr_in*>(&local);
            a->sin_family = AF_INET;
            a->sin_addr.s_addr = htonl(INADDR_ANY);
            a->sin_port = htons(static_cast<u_short>(cursor));
            len = sizeof(sockaddr_in);
        }
        if (bind(s, reinterpret_cast<sockaddr*>(&local), len) == 0) return cursor--;
    }
    return 0;
}

void applyStreamOptions(SOCKET s, const ConnectOptions& opts) {
    setBoolOpt(s, IPPROTO_TCP, TCP_NODELAY, opts.noDelay);
    setBoolOpt(s, SOL_SOCKET, SO_KEEPALIVE, opts.keepAlive);
}

}  // namespace

/* ------------------------------------------------------------------------ */
/* WsaInit / small helpers                                                   */
/* ------------------------------------------------------------------------ */

WsaInit::WsaInit() {
    WSADATA d;
    int rc = WSAStartup(MAKEWORD(2, 2), &d);
    if (rc != 0) throw std::runtime_error("WSAStartup failed: " + winErrorMessage(static_cast<unsigned long>(rc)));
}
WsaInit::~WsaInit() { WSACleanup(); }

std::string stripBrackets(const std::string& host) {
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') return host.substr(1, host.size() - 2);
    return host;
}

bool isNumericAddress(const std::string& hostIn, int* family) {
    const std::string host = stripBrackets(hostIn);
    if (host.empty()) return false;
    in_addr a4{};
    if (inet_pton(AF_INET, host.c_str(), &a4) == 1) {
        if (family) *family = AF_INET;
        return true;
    }
    // Allow scoped literals ("fe80::1%3").
    std::string noScope = host.substr(0, host.find('%'));
    in6_addr a6{};
    if (inet_pton(AF_INET6, noScope.c_str(), &a6) == 1) {
        if (family) *family = AF_INET6;
        return true;
    }
    return false;
}

std::string formatHostPort(const std::string& hostIn, int port) {
    const std::string host = stripBrackets(hostIn);
    if (host.find(':') != std::string::npos) return "[" + host + "]:" + std::to_string(port);
    return host + ":" + std::to_string(port);
}

std::vector<std::string> resolveHost(const std::string& host, AddressFamily family, std::string& error,
                                     const std::atomic<bool>* cancel, int timeoutMs) {
    std::optional<Clock::time_point> deadline;
    if (timeoutMs > 0) deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    std::vector<ResolvedAddr> addrs;
    std::vector<std::string> out;
    if (!resolveAddrs(host, 0, family, addrs, error, cancel, deadline)) return out;
    for (const auto& a : addrs) {
        std::string h = numericHost(reinterpret_cast<const sockaddr*>(&a.addr), a.len);
        if (!h.empty() && std::find(out.begin(), out.end(), h) == out.end()) out.push_back(h);
    }
    if (out.empty()) error = "Unable to resolve host \"" + stripBrackets(host) + "\"";
    return out;
}

/* ------------------------------------------------------------------------ */
/* connectTcp                                                                */
/* ------------------------------------------------------------------------ */

SOCKET connectTcp(const std::string& host, int port, const ConnectOptions& opts, std::string& error,
                  const std::atomic<bool>* cancel, std::string* resolvedAddress) {
    error.clear();
    if (port <= 0 || port > 65535) {
        error = "Invalid port number " + std::to_string(port);
        return INVALID_SOCKET;
    }
    auto log = [&](const std::string& line) {
        if (opts.log) opts.log(line);
    };
    std::optional<Clock::time_point> deadline;
    if (opts.timeoutMs > 0) deadline = Clock::now() + std::chrono::milliseconds(opts.timeoutMs);

    if (!isNumericAddress(host)) log("Looking up host \"" + stripBrackets(host) + "\"" + familySuffix(opts.family));
    std::vector<ResolvedAddr> resolved;
    if (!resolveAddrs(host, port, opts.family, resolved, error, cancel, deadline)) return INVALID_SOCKET;
    std::vector<ResolvedAddr> queue = interleave(resolved);

    const bool wantLocalRange = opts.localPortHigh > 0 && opts.localPortLow > 0 && opts.localPortLow <= opts.localPortHigh;
    int portCursor = opts.localPortHigh;
    bool rangeExhaustedLogged = false;
    int addrInUseRetries = 0;

    std::vector<Attempt> active;
    size_t next = 0;
    std::string lastError;
    Clock::time_point lastStart{};

    auto closeAll = [&] {
        for (auto& a : active) closeSocket(a.s);
        active.clear();
    };

    for (;;) {
        if (cancelled(cancel)) {
            closeAll();
            error = "Cancelled";
            return INVALID_SOCKET;
        }
        if (deadline && Clock::now() >= *deadline) {
            closeAll();
            error = "Connection timed out";
            return INVALID_SOCKET;
        }

        // Start the next attempt when nothing is pending or the stagger delay elapsed.
        while (next < queue.size() &&
               (active.empty() || Clock::now() - lastStart >= std::chrono::milliseconds(kAttemptStaggerMs))) {
            Attempt at;
            at.addr = queue[next++];
            at.text = numericHost(reinterpret_cast<const sockaddr*>(&at.addr.addr), at.addr.len);
            log("Connecting to " + at.text + " port " + std::to_string(port));
            at.s = createSocket(at.addr.family);
            if (at.s == INVALID_SOCKET) {
                lastError = lastWsaErrorMessage();
                log("Failed to connect to " + at.text + ": " + lastError);
                continue;
            }
            if (wantLocalRange) {
                int bound = bindLocalPortRange(at.s, at.addr.family, opts.localPortHigh, opts.localPortLow, portCursor);
                if (!bound && !rangeExhaustedLogged) {
                    rangeExhaustedLogged = true;
                    log("Unable to bind a local port in range " + std::to_string(opts.localPortLow) + "-" +
                        std::to_string(opts.localPortHigh) + "; using an ephemeral port");
                }
            }
            setNonBlocking(at.s, true);
            if (connect(at.s, reinterpret_cast<const sockaddr*>(&at.addr.addr), at.addr.len) == 0) {
                active.push_back(at);  // connected immediately; picked up by select below
            } else {
                int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) {
                    active.push_back(at);
                } else {
                    lastError = winErrorMessage(static_cast<unsigned long>(err));
                    log("Failed to connect to " + at.text + ": " + lastError);
                    closeSocket(at.s);
                    continue;
                }
            }
            lastStart = Clock::now();
            break;  // at most one new attempt per iteration (stagger)
        }

        if (active.empty()) {
            if (next < queue.size()) continue;
            error = lastError.empty() ? "Unable to connect" : lastError;
            return INVALID_SOCKET;
        }

        fd_set wfds, efds;
        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        for (const auto& a : active) {
            FD_SET(a.s, &wfds);
            FD_SET(a.s, &efds);
        }
        int waitMs = msUntil(deadline, kSliceMs);
        if (next < queue.size()) {
            auto sinceStart = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - lastStart).count();
            waitMs = std::min<int>(waitMs, static_cast<int>(std::max<long long>(0, kAttemptStaggerMs - sinceStart)));
        }
        timeval tv{waitMs / 1000, (waitMs % 1000) * 1000};
        int n = select(0, nullptr, &wfds, &efds, &tv);
        if (n == SOCKET_ERROR) {
            lastError = lastWsaErrorMessage();
            closeAll();
            error = lastError;
            return INVALID_SOCKET;
        }
        if (n == 0) continue;

        for (size_t i = 0; i < active.size();) {
            Attempt& a = active[i];
            const bool writable = FD_ISSET(a.s, &wfds) != 0;
            const bool failed = FD_ISSET(a.s, &efds) != 0;
            if (!writable && !failed) {
                ++i;
                continue;
            }
            int soErr = 0;
            int optLen = sizeof(soErr);
            getsockopt(a.s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &optLen);
            if (writable && soErr == 0) {
                SOCKET winner = a.s;
                std::string text = a.text;
                a.s = INVALID_SOCKET;
                closeAll();
                setNonBlocking(winner, false);
                applyStreamOptions(winner, opts);
                if (resolvedAddress) *resolvedAddress = text;
                return winner;
            }
            if (soErr == 0) soErr = WSAECONNREFUSED;
            lastError = winErrorMessage(static_cast<unsigned long>(soErr));
            log("Failed to connect to " + a.text + ": " + lastError);
            // A privileged source port may collide with a TIME_WAIT 4-tuple: retry the address with the next port.
            if (soErr == WSAEADDRINUSE && wantLocalRange && addrInUseRetries < 8) {
                ++addrInUseRetries;
                queue.insert(queue.begin() + static_cast<std::ptrdiff_t>(next), a.addr);
            }
            closeSocket(a.s);
            active.erase(active.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Socket pair / listener                                                    */
/* ------------------------------------------------------------------------ */

bool makeSocketPair(SOCKET& a, SOCKET& b, std::string& error) {
    a = b = INVALID_SOCKET;
    int port = 0;
    SOCKET listener = listenTcp("127.0.0.1", 0, AddressFamily::IPv4, error, &port);
    if (listener == INVALID_SOCKET) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<u_short>(port));

    SOCKET client = createSocket(AF_INET);
    if (client == INVALID_SOCKET || connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error = "Socket pair: connect failed: " + lastWsaErrorMessage();
        closeSocket(client);
        closeSocket(listener);
        return false;
    }
    sockaddr_in clientLocal{};
    int clientLen = sizeof(clientLocal);
    getsockname(client, reinterpret_cast<sockaddr*>(&clientLocal), &clientLen);

    // Accept until the peer is our own client (another local process could race for the ephemeral port).
    SOCKET server = INVALID_SOCKET;
    auto deadline = Clock::now() + std::chrono::seconds(5);
    while (server == INVALID_SOCKET) {
        int left = static_cast<int>(
            std::max<long long>(0, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count()));
        if (left == 0) {
            error = "Socket pair: timed out waiting for the loopback connection";
            break;
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listener, &rfds);
        timeval tv{left / 1000, (left % 1000) * 1000};
        if (select(0, &rfds, nullptr, nullptr, &tv) <= 0) continue;
        sockaddr_in peer{};
        int peerLen = sizeof(peer);
        SOCKET s = accept(listener, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (s == INVALID_SOCKET) {
            error = "Socket pair: accept failed: " + lastWsaErrorMessage();
            break;
        }
        noInherit(s);
        if (peer.sin_addr.s_addr == clientLocal.sin_addr.s_addr && peer.sin_port == clientLocal.sin_port) {
            server = s;
        } else {
            closesocket(s);  // stranger: drop it and keep waiting
        }
    }
    closeSocket(listener);
    if (server == INVALID_SOCKET) {
        closeSocket(client);
        return false;
    }
    setBoolOpt(client, IPPROTO_TCP, TCP_NODELAY, true);
    setBoolOpt(server, IPPROTO_TCP, TCP_NODELAY, true);
    a = client;
    b = server;
    return true;
}

SOCKET listenTcp(const std::string& bindAddressIn, int port, AddressFamily family, std::string& error, int* boundPort) {
    error.clear();
    if (port < 0 || port > 65535) {
        error = "Invalid port number " + std::to_string(port);
        return INVALID_SOCKET;
    }
    const std::string bindAddress = trim(stripBrackets(bindAddressIn));

    sockaddr_storage ss{};
    int len = 0;
    int af = AF_INET;
    bool dualStack = false;
    auto setV4 = [&](ULONG addrHostOrder) {
        auto* a = reinterpret_cast<sockaddr_in*>(&ss);
        a->sin_family = AF_INET;
        a->sin_addr.s_addr = htonl(addrHostOrder);
        a->sin_port = htons(static_cast<u_short>(port));
        len = sizeof(sockaddr_in);
        af = AF_INET;
    };
    auto setV6 = [&](const in6_addr& addr) {
        auto* a = reinterpret_cast<sockaddr_in6*>(&ss);
        a->sin6_family = AF_INET6;
        a->sin6_addr = addr;
        a->sin6_port = htons(static_cast<u_short>(port));
        len = sizeof(sockaddr_in6);
        af = AF_INET6;
    };

    if (bindAddress.empty() || iequals(bindAddress, "localhost")) {
        if (family == AddressFamily::IPv6)
            setV6(in6addr_loopback);
        else
            setV4(INADDR_LOOPBACK);
    } else if (bindAddress == "*" || bindAddress == "0.0.0.0") {
        if (family == AddressFamily::IPv6)
            setV6(in6addr_any);
        else
            setV4(INADDR_ANY);
    } else if (bindAddress == "::") {
        if (family == AddressFamily::IPv4) {
            setV4(INADDR_ANY);
        } else {
            setV6(in6addr_any);
            dualStack = family == AddressFamily::Auto;  // [::] with auto accepts IPv4 too
        }
    } else {
        std::vector<ResolvedAddr> addrs;
        if (!resolveAddrs(bindAddress, port, family, addrs, error, nullptr, Clock::now() + std::chrono::seconds(10)))
            return INVALID_SOCKET;
        memcpy(&ss, &addrs.front().addr, static_cast<size_t>(addrs.front().len));
        len = addrs.front().len;
        af = addrs.front().family;
    }

    SOCKET s = createSocket(af);
    if (s == INVALID_SOCKET) {
        error = "Unable to create socket: " + lastWsaErrorMessage();
        return INVALID_SOCKET;
    }
    // Prevent other sockets from hijacking the port (Windows SO_REUSEADDR semantics are unsafe).
    setBoolOpt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, true);
    if (af == AF_INET6) setBoolOpt(s, IPPROTO_IPV6, IPV6_V6ONLY, !dualStack);

    if (bind(s, reinterpret_cast<sockaddr*>(&ss), len) != 0) {
        error = "Unable to bind to " + formatHostPort(numericHost(reinterpret_cast<sockaddr*>(&ss), len), port) + ": " +
                lastWsaErrorMessage();
        closeSocket(s);
        return INVALID_SOCKET;
    }
    if (listen(s, SOMAXCONN) != 0) {
        error = "Unable to listen: " + lastWsaErrorMessage();
        closeSocket(s);
        return INVALID_SOCKET;
    }
    if (boundPort) *boundPort = localPort(s);
    return s;
}

/* ------------------------------------------------------------------------ */
/* Misc                                                                      */
/* ------------------------------------------------------------------------ */

void closeSocket(SOCKET& s) {
    if (s != INVALID_SOCKET) closesocket(s);
    s = INVALID_SOCKET;
}

bool sendAll(SOCKET s, std::string_view data) {
    if (s == INVALID_SOCKET) return false;
    while (!data.empty()) {
        int chunk = static_cast<int>(std::min<size_t>(data.size(), 1 << 20));
        int n = send(s, data.data(), chunk, 0);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() != WSAEWOULDBLOCK) return false;
            // Non-blocking socket: wait until writable.
            fd_set wfds, efds;
            FD_ZERO(&wfds);
            FD_ZERO(&efds);
            FD_SET(s, &wfds);
            FD_SET(s, &efds);
            if (select(0, nullptr, &wfds, &efds, nullptr) == SOCKET_ERROR || FD_ISSET(s, &efds)) return false;
            continue;
        }
        data.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

void setNonBlocking(SOCKET s, bool nonBlocking) {
    u_long v = nonBlocking ? 1 : 0;
    ioctlsocket(s, FIONBIO, &v);
}

std::string peerName(SOCKET s) {
    sockaddr_storage ss{};
    int len = sizeof(ss);
    if (s == INVALID_SOCKET || getpeername(s, reinterpret_cast<sockaddr*>(&ss), &len) != 0) return {};
    return formatSockaddr(reinterpret_cast<sockaddr*>(&ss), len);
}

std::string localName(SOCKET s) {
    sockaddr_storage ss{};
    int len = sizeof(ss);
    if (s == INVALID_SOCKET || getsockname(s, reinterpret_cast<sockaddr*>(&ss), &len) != 0) return {};
    return formatSockaddr(reinterpret_cast<sockaddr*>(&ss), len);
}

int localPort(SOCKET s) {
    sockaddr_storage ss{};
    int len = sizeof(ss);
    if (s == INVALID_SOCKET || getsockname(s, reinterpret_cast<sockaddr*>(&ss), &len) != 0) return 0;
    return sockaddrPort(reinterpret_cast<sockaddr*>(&ss));
}

/* ------------------------------------------------------------------------ */
/* Waker                                                                     */
/* ------------------------------------------------------------------------ */

Waker::Waker() : event_(WSACreateEvent()) {
    if (event_ == WSA_INVALID_EVENT) throw std::runtime_error("WSACreateEvent failed: " + lastWsaErrorMessage());
}
Waker::~Waker() { WSACloseEvent(event_); }
void Waker::signal() { WSASetEvent(event_); }
void Waker::reset() { WSAResetEvent(event_); }

}  // namespace sc
