// Winsock implementation of platform/sys/Net.h: resolution (cancellable GetAddrInfoExW), connect with timeout/cancel
// (staggered parallel attempts across addresses, RFC 8305 style), listeners, socket pairs, I/O helpers and the two
// waits (SocketWaiter: WSAEventSelect; PollWaker: a loopback socket pair, as WSAPoll and libssh only wait on sockets).
#include "platform/sys/Net.h"

#include "platform/detail/WinApi.h"
#include "platform/sys/Errors.h"
#include "platform/win/sys/WinText.h"

#include <mstcpip.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace st::sys {

namespace {

using Clock = std::chrono::steady_clock;

/** Delay before starting a parallel attempt to the next address while earlier ones are still pending. */
constexpr int kAttemptStaggerMs = 250;
/** Upper bound of a single wait slice (cancel polling granularity). */
constexpr int kSliceMs = 100;

SOCKET raw(SocketHandle s) { return static_cast<SOCKET>(s); }
SocketHandle handle(SOCKET s) { return static_cast<SocketHandle>(s); }

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

std::string trimmed(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

bool equalsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

int toAf(IpFamily f) {
    switch (f) {
        case IpFamily::IPv4: return AF_INET;
        case IpFamily::IPv6: return AF_INET6;
        default: return AF_UNSPEC;
    }
}

std::string familySuffix(IpFamily f) {
    switch (f) {
        case IpFamily::IPv4: return " (IPv4)";
        case IpFamily::IPv6: return " (IPv6)";
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

void closeRaw(SOCKET& s) {
    if (s != INVALID_SOCKET) closesocket(s);
    s = INVALID_SOCKET;
}

void setNonBlockingRaw(SOCKET s, bool nonBlocking) {
    u_long v = nonBlocking ? 1 : 0;
    ioctlsocket(s, FIONBIO, &v);
}

bool cancelled(const std::atomic<bool>* cancel) { return cancel && cancel->load(); }

/** Milliseconds until `deadline` (clamped to [0, cap]); `cap` when there is no deadline. */
int msUntil(const std::optional<Clock::time_point>& deadline, int cap) {
    if (!deadline) return cap;
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - Clock::now()).count();
    if (left <= 0) return 0;
    return static_cast<int>(std::min<long long>(left, cap));
}

/** Numeric literal check that reports the Winsock address family. */
bool numericFamily(const std::string& hostIn, int* family) {
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

/**
 * Asynchronous GetAddrInfoExW so that resolution can be cancelled / timed out. Fills `out` in resolver order.
 */
bool resolveAddrs(const std::string& hostIn, int port, IpFamily family, std::vector<ResolvedAddr>& out,
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

    ADDRINFOEXW* rawResult = nullptr;
    UniqueHandle ev(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!ev) {
        error = "Unable to resolve host: " + lastErrorMessage();
        return false;
    }
    OVERLAPPED ov{};
    ov.hEvent = ev.get();
    HANDLE cancelHandle = nullptr;
    INT rc = GetAddrInfoExW(whost.c_str(), wport.c_str(), NS_ALL, nullptr, &hints, &rawResult, nullptr, &ov, nullptr,
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
            if (rawResult) FreeAddrInfoExW(rawResult);
            error = aborted ? "Cancelled" : "Timed out resolving host \"" + host + "\"";
            return false;
        }
    }
    AddrInfoExPtr result(rawResult);
    if (rc != NO_ERROR) {
        error = "Unable to resolve host \"" + host + "\": " + errorMessage(static_cast<unsigned long>(rc));
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
/* NetInit / addresses                                                       */
/* ------------------------------------------------------------------------ */

NetInit::NetInit() {
    WSADATA d;
    int rc = WSAStartup(MAKEWORD(2, 2), &d);
    if (rc != 0) throw std::runtime_error("WSAStartup failed: " + errorMessage(static_cast<unsigned long>(rc)));
}
NetInit::~NetInit() { WSACleanup(); }

bool isNumericAddress(const std::string& host, IpFamily* family) {
    int af = 0;
    if (!numericFamily(host, &af)) return false;
    if (family) *family = af == AF_INET6 ? IpFamily::IPv6 : IpFamily::IPv4;
    return true;
}

std::string addressBytes(const std::string& hostIn) {
    const std::string host = stripBrackets(hostIn);
    in_addr a4{};
    if (inet_pton(AF_INET, host.c_str(), &a4) == 1) return std::string(reinterpret_cast<const char*>(&a4), 4);
    in6_addr a6{};
    if (inet_pton(AF_INET6, host.substr(0, host.find('%')).c_str(), &a6) == 1)
        return std::string(reinterpret_cast<const char*>(&a6), 16);
    return {};
}

std::string formatAddressBytes(std::string_view bytes) {
    char buf[INET6_ADDRSTRLEN] = {};
    if (bytes.size() == 4) {
        in_addr a4{};
        memcpy(&a4, bytes.data(), 4);
        return inet_ntop(AF_INET, &a4, buf, sizeof buf) ? std::string(buf) : std::string();
    }
    if (bytes.size() == 16) {
        in6_addr a6{};
        memcpy(&a6, bytes.data(), 16);
        return inet_ntop(AF_INET6, &a6, buf, sizeof buf) ? std::string(buf) : std::string();
    }
    return {};
}

std::vector<std::string> resolveHost(const std::string& host, IpFamily family, std::string& error,
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

SocketHandle connectTcp(const std::string& host, int port, const ConnectOptions& opts, std::string& error,
                        const std::atomic<bool>* cancel, std::string* resolvedAddress) {
    error.clear();
    if (port <= 0 || port > 65535) {
        error = "Invalid port number " + std::to_string(port);
        return kInvalidSocket;
    }
    auto log = [&](const std::string& line) {
        if (opts.log) opts.log(line);
    };
    std::optional<Clock::time_point> deadline;
    if (opts.timeoutMs > 0) deadline = Clock::now() + std::chrono::milliseconds(opts.timeoutMs);

    if (!isNumericAddress(host)) log("Looking up host \"" + stripBrackets(host) + "\"" + familySuffix(opts.family));
    std::vector<ResolvedAddr> resolved;
    if (!resolveAddrs(host, port, opts.family, resolved, error, cancel, deadline)) return kInvalidSocket;
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
        for (auto& a : active) closeRaw(a.s);
        active.clear();
    };

    for (;;) {
        if (cancelled(cancel)) {
            closeAll();
            error = "Cancelled";
            return kInvalidSocket;
        }
        if (deadline && Clock::now() >= *deadline) {
            closeAll();
            error = "Connection timed out";
            return kInvalidSocket;
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
                lastError = lastSocketErrorMessage();
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
            setNonBlockingRaw(at.s, true);
            if (connect(at.s, reinterpret_cast<const sockaddr*>(&at.addr.addr), at.addr.len) == 0) {
                active.push_back(at);  // connected immediately; picked up by select below
            } else {
                int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) {
                    active.push_back(at);
                } else {
                    lastError = errorMessage(static_cast<unsigned long>(err));
                    log("Failed to connect to " + at.text + ": " + lastError);
                    closeRaw(at.s);
                    continue;
                }
            }
            lastStart = Clock::now();
            break;  // at most one new attempt per iteration (stagger)
        }

        if (active.empty()) {
            if (next < queue.size()) continue;
            error = lastError.empty() ? "Unable to connect" : lastError;
            return kInvalidSocket;
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
            lastError = lastSocketErrorMessage();
            closeAll();
            error = lastError;
            return kInvalidSocket;
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
                setNonBlockingRaw(winner, false);
                applyStreamOptions(winner, opts);
                if (resolvedAddress) *resolvedAddress = text;
                return handle(winner);
            }
            if (soErr == 0) soErr = WSAECONNREFUSED;
            lastError = errorMessage(static_cast<unsigned long>(soErr));
            log("Failed to connect to " + a.text + ": " + lastError);
            // A privileged source port may collide with a TIME_WAIT 4-tuple: retry the address with the next port.
            if (soErr == WSAEADDRINUSE && wantLocalRange && addrInUseRetries < 8) {
                ++addrInUseRetries;
                queue.insert(queue.begin() + static_cast<std::ptrdiff_t>(next), a.addr);
            }
            closeRaw(a.s);
            active.erase(active.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Socket pair / listener                                                    */
/* ------------------------------------------------------------------------ */

SocketHandle connectUnix(const std::string&, std::string& error) {
    error = "Unix-domain sockets are not used on Windows";
    return kInvalidSocket;
}

std::string localHostName() {
    char buf[256] = {};
    return ::gethostname(buf, static_cast<int>(sizeof buf - 1)) == 0 ? std::string(buf) : std::string();
}

bool makeSocketPair(SocketHandle& aOut, SocketHandle& bOut, std::string& error) {
    aOut = bOut = kInvalidSocket;
    int port = 0;
    SOCKET listener = raw(listenTcp("127.0.0.1", 0, IpFamily::IPv4, error, &port));
    if (listener == INVALID_SOCKET) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<u_short>(port));

    SOCKET client = createSocket(AF_INET);
    if (client == INVALID_SOCKET || connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error = "Socket pair: connect failed: " + lastSocketErrorMessage();
        closeRaw(client);
        closeRaw(listener);
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
            error = "Socket pair: accept failed: " + lastSocketErrorMessage();
            break;
        }
        noInherit(s);
        if (peer.sin_addr.s_addr == clientLocal.sin_addr.s_addr && peer.sin_port == clientLocal.sin_port) {
            server = s;
        } else {
            closesocket(s);  // stranger: drop it and keep waiting
        }
    }
    closeRaw(listener);
    if (server == INVALID_SOCKET) {
        closeRaw(client);
        return false;
    }
    setBoolOpt(client, IPPROTO_TCP, TCP_NODELAY, true);
    setBoolOpt(server, IPPROTO_TCP, TCP_NODELAY, true);
    aOut = handle(client);
    bOut = handle(server);
    return true;
}

SocketHandle listenTcp(const std::string& bindAddressIn, int port, IpFamily family, std::string& error, int* boundPort) {
    error.clear();
    if (port < 0 || port > 65535) {
        error = "Invalid port number " + std::to_string(port);
        return kInvalidSocket;
    }
    const std::string bindAddress = trimmed(stripBrackets(bindAddressIn));

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

    if (bindAddress.empty() || equalsNoCase(bindAddress, "localhost")) {
        if (family == IpFamily::IPv6)
            setV6(in6addr_loopback);
        else
            setV4(INADDR_LOOPBACK);
    } else if (bindAddress == "*" || bindAddress == "0.0.0.0") {
        if (family == IpFamily::IPv6)
            setV6(in6addr_any);
        else
            setV4(INADDR_ANY);
    } else if (bindAddress == "::") {
        if (family == IpFamily::IPv4) {
            setV4(INADDR_ANY);
        } else {
            setV6(in6addr_any);
            dualStack = family == IpFamily::Auto;  // [::] with auto accepts IPv4 too
        }
    } else {
        std::vector<ResolvedAddr> addrs;
        if (!resolveAddrs(bindAddress, port, family, addrs, error, nullptr, Clock::now() + std::chrono::seconds(10)))
            return kInvalidSocket;
        memcpy(&ss, &addrs.front().addr, static_cast<size_t>(addrs.front().len));
        len = addrs.front().len;
        af = addrs.front().family;
    }

    SOCKET s = createSocket(af);
    if (s == INVALID_SOCKET) {
        error = "Unable to create socket: " + lastSocketErrorMessage();
        return kInvalidSocket;
    }
    // Prevent other sockets from hijacking the port (Windows SO_REUSEADDR semantics are unsafe).
    setBoolOpt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, true);
    if (af == AF_INET6) setBoolOpt(s, IPPROTO_IPV6, IPV6_V6ONLY, !dualStack);

    if (bind(s, reinterpret_cast<sockaddr*>(&ss), len) != 0) {
        error = "Unable to bind to " + formatHostPort(numericHost(reinterpret_cast<sockaddr*>(&ss), len), port) + ": " +
                lastSocketErrorMessage();
        closeRaw(s);
        return kInvalidSocket;
    }
    if (listen(s, SOMAXCONN) != 0) {
        error = "Unable to listen: " + lastSocketErrorMessage();
        closeRaw(s);
        return kInvalidSocket;
    }
    if (boundPort) *boundPort = localPort(handle(s));
    return handle(s);
}

SocketHandle acceptConnection(SocketHandle listener, bool noDelay) {
    sockaddr_storage peer{};
    int len = sizeof(peer);
    SOCKET c = accept(raw(listener), reinterpret_cast<sockaddr*>(&peer), &len);
    if (c == INVALID_SOCKET) return kInvalidSocket;
    noInherit(c);
    if (noDelay) setBoolOpt(c, IPPROTO_TCP, TCP_NODELAY, true);
    return handle(c);
}

/* ------------------------------------------------------------------------ */
/* I/O                                                                       */
/* ------------------------------------------------------------------------ */

NetErr lastNetError() {
    const int err = WSAGetLastError();
    if (err == 0) return NetErr::None;
    return err == WSAEWOULDBLOCK ? NetErr::WouldBlock : NetErr::Other;
}

long sendSome(SocketHandle s, const void* data, std::size_t n, bool urgent) {
    const int chunk = static_cast<int>(std::min<std::size_t>(n, INT_MAX));
    const int sent = send(raw(s), static_cast<const char*>(data), chunk, urgent ? MSG_OOB : 0);
    return sent == SOCKET_ERROR ? -1 : sent;
}

bool sendAll(SocketHandle h, std::string_view data) {
    const SOCKET s = raw(h);
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

long recvSome(SocketHandle s, void* buf, std::size_t n) {
    const int want = static_cast<int>(std::min<std::size_t>(n, INT_MAX));
    const int got = recv(raw(s), static_cast<char*>(buf), want, 0);
    return got == SOCKET_ERROR ? -1 : got;
}

int recvUrgentByte(SocketHandle s) {
    char b = 0;
    return recv(raw(s), &b, 1, MSG_OOB) == 1 ? static_cast<unsigned char>(b) : -1;
}

int waitReadable(SocketHandle s, int timeoutMs) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(raw(s), &rfds);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    const int n = select(0, &rfds, nullptr, nullptr, &tv);
    if (n == SOCKET_ERROR) return -1;
    return n > 0 ? 1 : 0;
}

void setNonBlocking(SocketHandle s, bool nonBlocking) { setNonBlockingRaw(raw(s), nonBlocking); }

void shutdownWrite(SocketHandle s) { shutdown(raw(s), SD_SEND); }

void shutdownBoth(SocketHandle s) { shutdown(raw(s), SD_BOTH); }

void closeSocket(SocketHandle& s) {
    if (s != kInvalidSocket) closesocket(raw(s));
    s = kInvalidSocket;
}

void interruptSocket(SocketHandle& s) { closeSocket(s); }

std::string peerName(SocketHandle s) {
    sockaddr_storage ss{};
    int len = sizeof(ss);
    if (s == kInvalidSocket || getpeername(raw(s), reinterpret_cast<sockaddr*>(&ss), &len) != 0) return {};
    return formatSockaddr(reinterpret_cast<sockaddr*>(&ss), len);
}

std::string localName(SocketHandle s) {
    sockaddr_storage ss{};
    int len = sizeof(ss);
    if (s == kInvalidSocket || getsockname(raw(s), reinterpret_cast<sockaddr*>(&ss), &len) != 0) return {};
    return formatSockaddr(reinterpret_cast<sockaddr*>(&ss), len);
}

std::optional<std::pair<std::string, int>> peerAddress(SocketHandle s) {
    sockaddr_storage ss{};
    int len = sizeof(ss);
    if (s == kInvalidSocket || getpeername(raw(s), reinterpret_cast<sockaddr*>(&ss), &len) != 0) return std::nullopt;
    std::string host = numericHost(reinterpret_cast<sockaddr*>(&ss), len);
    if (host.empty()) return std::nullopt;
    return std::make_pair(host, sockaddrPort(reinterpret_cast<sockaddr*>(&ss)));
}

int localPort(SocketHandle s) {
    sockaddr_storage ss{};
    int len = sizeof(ss);
    if (s == kInvalidSocket || getsockname(raw(s), reinterpret_cast<sockaddr*>(&ss), &len) != 0) return 0;
    return sockaddrPort(reinterpret_cast<sockaddr*>(&ss));
}

/* ------------------------------------------------------------------------ */
/* SocketWaiter: WSAEventSelect on the socket + a manual-reset wake event    */
/* ------------------------------------------------------------------------ */

struct SocketWaiter::Impl {
    WSAEVENT wake = WSA_INVALID_EVENT;
    WSAEVENT sockEvent = WSA_INVALID_EVENT;
    SOCKET sock = INVALID_SOCKET;
};

SocketWaiter::SocketWaiter() : impl_(std::make_unique<Impl>()) {
    impl_->wake = WSACreateEvent();
    if (impl_->wake == WSA_INVALID_EVENT) throw std::runtime_error("WSACreateEvent failed: " + lastSocketErrorMessage());
}

SocketWaiter::~SocketWaiter() {
    detach();
    WSACloseEvent(impl_->wake);
}

void SocketWaiter::wake() { WSASetEvent(impl_->wake); }

bool SocketWaiter::attach(SocketHandle s, bool urgentInline, std::string& error) {
    impl_->sock = raw(s);
    impl_->sockEvent = WSACreateEvent();
    BOOL inlineOob = urgentInline ? TRUE : FALSE;
    setsockopt(impl_->sock, SOL_SOCKET, SO_OOBINLINE, reinterpret_cast<const char*>(&inlineOob), sizeof(inlineOob));
    // WSAEventSelect also makes the socket non-blocking.
    if (impl_->sockEvent == WSA_INVALID_EVENT ||
        WSAEventSelect(impl_->sock, impl_->sockEvent, FD_READ | FD_WRITE | FD_OOB | FD_CLOSE) == SOCKET_ERROR) {
        error = lastSocketErrorMessage();
        return false;
    }
    return true;
}

void SocketWaiter::detach() {
    if (impl_->sockEvent != WSA_INVALID_EVENT) {
        WSACloseEvent(impl_->sockEvent);
        impl_->sockEvent = WSA_INVALID_EVENT;
    }
    impl_->sock = INVALID_SOCKET;
}

void SocketWaiter::wantWrite(bool) {}  // FD_WRITE is signalled by Winsock after a send that would have blocked

bool SocketWaiter::wait(int timeoutMs, Events& ev, std::string& error) {
    ev = Events{};
    const DWORD timeout = timeoutMs < 0 ? WSA_INFINITE : static_cast<DWORD>(timeoutMs);
    WSAEVENT events[2] = {impl_->sockEvent, impl_->wake};
    const DWORD rc = WSAWaitForMultipleEvents(2, events, FALSE, timeout, FALSE);
    if (rc == WSA_WAIT_FAILED) {
        error = lastSocketErrorMessage();
        return false;
    }
    if (rc == WSA_WAIT_TIMEOUT) {
        ev.timedOut = true;
        return true;
    }
    if (rc == WSA_WAIT_EVENT_0 + 1) {
        WSAResetEvent(impl_->wake);
        ev.woken = true;
    }
    WSANETWORKEVENTS ne{};
    if (WSAEnumNetworkEvents(impl_->sock, impl_->sockEvent, &ne) == SOCKET_ERROR) {
        error = lastSocketErrorMessage();
        return false;
    }
    ev.urgent = (ne.lNetworkEvents & FD_OOB) != 0;
    ev.readable = (ne.lNetworkEvents & FD_READ) != 0;
    ev.writable = (ne.lNetworkEvents & FD_WRITE) != 0;
    if (ne.lNetworkEvents & FD_CLOSE) {
        ev.closed = true;
        const int err = ne.iErrorCode[FD_CLOSE_BIT];
        if (err != 0) ev.closeError = errorMessage(static_cast<unsigned long>(err));
    }
    return true;
}

const short kPollIn = POLLIN;
const short kPollOut = POLLOUT;

/* ------------------------------------------------------------------------ */
/* PollWaker: loopback socket pair (WSAPoll / libssh can only wait on sockets) */
/* ------------------------------------------------------------------------ */

bool PollWaker::open(std::string& error) {
    SocketHandle a = kInvalidSocket, b = kInvalidSocket;
    if (!makeSocketPair(a, b, error)) return false;
    setNonBlocking(a, true);
    setNonBlocking(b, true);
    std::lock_guard lock(mutex_);
    recv_ = a;
    send_ = b;
    return true;
}

void PollWaker::signal() {
    std::lock_guard lock(mutex_);
    if (send_ == kInvalidSocket) return;
    const char byte = 1;
    // A full buffer (WSAEWOULDBLOCK) means a wake-up is already pending, which is all we need.
    ::send(raw(send_), &byte, 1, 0);
}

void PollWaker::drain() {
    if (recv_ == kInvalidSocket) return;
    char buf[256];
    while (::recv(raw(recv_), buf, sizeof buf, 0) > 0) {
    }
}

void PollWaker::close() {
    std::lock_guard lock(mutex_);
    closeSocket(send_);
    closeSocket(recv_);
}

}  // namespace st::sys
