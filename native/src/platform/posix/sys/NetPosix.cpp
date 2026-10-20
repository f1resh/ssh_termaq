// Sockets on POSIX (platform/sys/Net.h): file descriptors with FD_CLOEXEC and SO_NOSIGPIPE, poll(2) everywhere
// (no select limit on descriptor numbers), getaddrinfo on a helper thread so a lookup can be cancelled, and a pipe as
// PollWaker. SocketWaiter is per OS (macOS: mac/sys/SocketWaiterMac.cpp, kqueue): poll(2) cannot see TCP urgent data
// there.
#include "platform/sys/Net.h"

#include "platform/sys/Errors.h"
#include "platform/posix/sys/NetHints.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace st::sys {

namespace {

using Clock = std::chrono::steady_clock;

/** Delay before starting a parallel attempt to the next address while earlier ones are still pending. */
constexpr int kAttemptStaggerMs = 250;
/** Upper bound of a single wait slice (cancel polling granularity). */
constexpr int kSliceMs = 100;

int fd(SocketHandle s) { return static_cast<int>(s); }

struct ResolvedAddr {
    sockaddr_storage addr{};
    socklen_t len = 0;
    int family = AF_UNSPEC;
};

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
std::string numericHost(const sockaddr* sa, socklen_t len) {
    char host[NI_MAXHOST] = {};
    if (getnameinfo(sa, len, host, sizeof host, nullptr, 0, NI_NUMERICHOST) != 0) return {};
    return host;
}

int sockaddrPort(const sockaddr* sa) {
    if (sa->sa_family == AF_INET) return ntohs(reinterpret_cast<const sockaddr_in*>(sa)->sin_port);
    if (sa->sa_family == AF_INET6) return ntohs(reinterpret_cast<const sockaddr_in6*>(sa)->sin6_port);
    return 0;
}

std::string formatSockaddr(const sockaddr* sa, socklen_t len) {
    std::string h = numericHost(sa, len);
    if (h.empty()) return {};
    return formatHostPort(h, sockaddrPort(sa));
}

void setIntOpt(int s, int level, int name, int value) { setsockopt(s, level, name, &value, sizeof value); }

/** Every socket we hold: never inherited by child processes, and a write to a closed peer is an error, not SIGPIPE. */
void prepareFd(int s) {
    fcntl(s, F_SETFD, fcntl(s, F_GETFD) | FD_CLOEXEC);
    setIntOpt(s, SOL_SOCKET, SO_NOSIGPIPE, 1);
}

int createSocket(int family) {
    const int s = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (s >= 0) prepareFd(s);
    return s;
}

void closeFd(int& s) {
    if (s >= 0) ::close(s);
    s = -1;
}

void setNonBlockingFd(int s, bool nonBlocking) {
    const int flags = fcntl(s, F_GETFL);
    if (flags < 0) return;
    fcntl(s, F_SETFL, nonBlocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}

bool cancelled(const std::atomic<bool>* cancel) { return cancel && cancel->load(); }

/** Milliseconds until `deadline` (clamped to [0, cap]); `cap` when there is no deadline. */
int msUntil(const std::optional<Clock::time_point>& deadline, int cap) {
    if (!deadline) return cap;
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - Clock::now()).count();
    if (left <= 0) return 0;
    return static_cast<int>(std::min<long long>(left, cap));
}

/** poll(2) that resumes after signals until its time is up. */
int pollFor(pollfd* fds, nfds_t n, int timeoutMs) {
    const auto start = Clock::now();
    for (;;) {
        int left = timeoutMs;
        if (timeoutMs > 0) {
            const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
            left = static_cast<int>(std::max<long long>(0, timeoutMs - spent));
        }
        const int rc = ::poll(fds, n, left);
        if (rc >= 0 || errno != EINTR) return rc;
    }
}

/** Numeric literal check that reports the address family. */
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

/** One getaddrinfo on its own thread; the caller can stop waiting for it (the thread then cleans up alone). */
struct Lookup {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    int rc = 0;
    int sysErrno = 0;
    std::vector<ResolvedAddr> addrs;
};

/** getaddrinfo with cancellation and a deadline. Fills `out` in resolver order. */
bool resolveAddrs(const std::string& hostIn, int port, IpFamily family, std::vector<ResolvedAddr>& out,
                  std::string& error, const std::atomic<bool>* cancel, const std::optional<Clock::time_point>& deadline) {
    out.clear();
    const std::string host = stripBrackets(hostIn);
    if (host.empty()) {
        error = "No host name specified";
        return false;
    }
    auto lookup = std::make_shared<Lookup>();
    const int af = toAf(family);
    std::thread([lookup, host, port, af] {
        addrinfo hints{};
        hints.ai_family = af;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = AI_ADDRCONFIG;
        if (af == AF_UNSPEC) hints.ai_flags = 0;  // keep loopback names resolvable without configured addresses
        addrinfo* result = nullptr;
        const std::string service = std::to_string(port);
        const int rc = getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
        const int err = errno;
        std::vector<ResolvedAddr> addrs;
        for (addrinfo* ai = result; ai; ai = ai->ai_next) {
            if (!ai->ai_addr || ai->ai_addrlen > sizeof(sockaddr_storage)) continue;
            if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) continue;
            ResolvedAddr r;
            memcpy(&r.addr, ai->ai_addr, ai->ai_addrlen);
            r.len = ai->ai_addrlen;
            r.family = ai->ai_family;
            addrs.push_back(r);
        }
        if (result) freeaddrinfo(result);
        std::lock_guard lock(lookup->m);
        lookup->rc = rc;
        lookup->sysErrno = err;
        lookup->addrs = std::move(addrs);
        lookup->done = true;
        lookup->cv.notify_all();
    }).detach();

    std::unique_lock lock(lookup->m);
    while (!lookup->done) {
        if (cancelled(cancel)) {
            error = "Cancelled";
            return false;
        }
        if (deadline && Clock::now() >= *deadline) {
            error = "Timed out resolving host \"" + host + "\"";
            return false;
        }
        lookup->cv.wait_for(lock, std::chrono::milliseconds(msUntil(deadline, kSliceMs)));
    }
    if (lookup->rc != 0) {
        const std::string why = lookup->rc == EAI_SYSTEM ? errorMessage(static_cast<unsigned long>(lookup->sysErrno))
                                                         : gai_strerror(lookup->rc);
        error = "Unable to resolve host \"" + host + "\": " + why;
        return false;
    }
    out = lookup->addrs;
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
    int s = -1;
    ResolvedAddr addr;
    std::string text;  // numeric host (no port)
};

/** Binds `s` to a local port in [low, high] (descending, starting at *cursor). Returns the port or 0. */
int bindLocalPortRange(int s, int family, int high, int low, int& cursor) {
    for (; cursor >= low && cursor <= high; --cursor) {
        sockaddr_storage local{};
        socklen_t len = 0;
        if (family == AF_INET6) {
            auto* a = reinterpret_cast<sockaddr_in6*>(&local);
            a->sin6_family = AF_INET6;
            a->sin6_addr = in6addr_any;
            a->sin6_port = htons(static_cast<uint16_t>(cursor));
            len = sizeof(sockaddr_in6);
        } else {
            auto* a = reinterpret_cast<sockaddr_in*>(&local);
            a->sin_family = AF_INET;
            a->sin_addr.s_addr = htonl(INADDR_ANY);
            a->sin_port = htons(static_cast<uint16_t>(cursor));
            len = sizeof(sockaddr_in);
        }
        if (::bind(s, reinterpret_cast<sockaddr*>(&local), len) == 0) return cursor--;
    }
    return 0;
}

void applyStreamOptions(int s, const ConnectOptions& opts) {
    setIntOpt(s, IPPROTO_TCP, TCP_NODELAY, opts.noDelay ? 1 : 0);
    setIntOpt(s, SOL_SOCKET, SO_KEEPALIVE, opts.keepAlive ? 1 : 0);
}

/** Non-blocking pipe with both ends close-on-exec (wake-ups). */
bool makeWakePipe(int& readEnd, int& writeEnd, std::string& error) {
    int p[2];
    if (::pipe(p) != 0) {
        error = "pipe: " + lastErrorMessage();
        return false;
    }
    for (int e : p) {
        fcntl(e, F_SETFD, FD_CLOEXEC);
        setNonBlockingFd(e, true);
    }
    readEnd = p[0];
    writeEnd = p[1];
    return true;
}

void drainPipe(int readEnd) {
    char buf[256];
    while (::read(readEnd, buf, sizeof buf) > 0) {
    }
}

}  // namespace

/* ------------------------------------------------------------------------ */
/* NetInit / addresses                                                       */
/* ------------------------------------------------------------------------ */

NetInit::NetInit() {
    // A write to a socket or pipe whose reader is gone must fail with EPIPE, not kill the process.
    ::signal(SIGPIPE, SIG_IGN);
    // macOS starts processes with 256 descriptors; sessions, forwardings and proxy commands need more.
    rlimit rl{};
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        const rlim_t want = std::min<rlim_t>(rl.rlim_max, OPEN_MAX);
        if (rl.rlim_cur < want) {
            rl.rlim_cur = want;
            setrlimit(RLIMIT_NOFILE, &rl);
        }
    }
}

NetInit::~NetInit() = default;

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
    std::string hint;  // what the OS may have done (detail::connectFailureHint)
    Clock::time_point lastStart{};

    auto closeAll = [&] {
        for (auto& a : active) closeFd(a.s);
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
            if (at.s < 0) {
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
            setNonBlockingFd(at.s, true);
            if (::connect(at.s, reinterpret_cast<const sockaddr*>(&at.addr.addr), at.addr.len) == 0 ||
                errno == EINPROGRESS || errno == EINTR) {
                active.push_back(at);  // connected or in progress; picked up by poll below
            } else {
                const int err = errno;
                lastError = lastSocketErrorMessage();
                if (hint.empty()) hint = detail::connectFailureHint(err, reinterpret_cast<const sockaddr*>(&at.addr.addr));
                log("Failed to connect to " + at.text + ": " + lastError);
                closeFd(at.s);
                continue;
            }
            lastStart = Clock::now();
            break;  // at most one new attempt per iteration (stagger)
        }

        if (active.empty()) {
            if (next < queue.size()) continue;
            error = lastError.empty() ? "Unable to connect" : lastError;
            if (!hint.empty()) error += ". " + hint;
            return kInvalidSocket;
        }

        std::vector<pollfd> fds;
        for (const auto& a : active) fds.push_back(pollfd{a.s, POLLOUT, 0});
        int waitMs = msUntil(deadline, kSliceMs);
        if (next < queue.size()) {
            auto sinceStart = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - lastStart).count();
            waitMs = std::min<int>(waitMs, static_cast<int>(std::max<long long>(0, kAttemptStaggerMs - sinceStart)));
        }
        const int n = pollFor(fds.data(), static_cast<nfds_t>(fds.size()), waitMs);
        if (n < 0) {
            lastError = lastSocketErrorMessage();
            closeAll();
            error = lastError;
            return kInvalidSocket;
        }
        if (n == 0) continue;

        for (size_t i = 0, k = 0; i < active.size(); ++k) {
            Attempt& a = active[i];
            if (fds[k].revents == 0) {
                ++i;
                continue;
            }
            int soErr = 0;
            socklen_t optLen = sizeof soErr;
            getsockopt(a.s, SOL_SOCKET, SO_ERROR, &soErr, &optLen);
            if ((fds[k].revents & POLLOUT) && soErr == 0) {
                int winner = a.s;
                std::string text = a.text;
                a.s = -1;
                closeAll();
                setNonBlockingFd(winner, false);
                applyStreamOptions(winner, opts);
                if (resolvedAddress) *resolvedAddress = text;
                return winner;
            }
            if (soErr == 0) soErr = ECONNREFUSED;
            lastError = errorMessage(static_cast<unsigned long>(soErr));
            if (hint.empty()) hint = detail::connectFailureHint(soErr, reinterpret_cast<const sockaddr*>(&a.addr.addr));
            log("Failed to connect to " + a.text + ": " + lastError);
            // A privileged source port may collide with a TIME_WAIT 4-tuple: retry the address with the next port.
            if (soErr == EADDRINUSE && wantLocalRange && addrInUseRetries < 8) {
                ++addrInUseRetries;
                queue.insert(queue.begin() + static_cast<std::ptrdiff_t>(next), a.addr);
            }
            closeFd(a.s);
            active.erase(active.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Socket pair / listener                                                    */
/* ------------------------------------------------------------------------ */

SocketHandle connectUnix(const std::string& path, std::string& error) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) {
        error = "the socket path is too long";
        return kInvalidSocket;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    const int s = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) {
        error = lastErrorMessage();
        return kInvalidSocket;
    }
    ::fcntl(s, F_SETFD, FD_CLOEXEC);
    const int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    int rc;
    do {
        rc = ::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) {
        error = lastErrorMessage();
        ::close(s);
        return kInvalidSocket;
    }
    return s;
}

std::string localHostName() {
    char buf[256] = {};
    return ::gethostname(buf, sizeof buf - 1) == 0 ? std::string(buf) : std::string();
}

bool makeSocketPair(SocketHandle& aOut, SocketHandle& bOut, std::string& error) {
    aOut = bOut = kInvalidSocket;
    int p[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, p) != 0) {
        error = "Socket pair: " + lastSocketErrorMessage();
        return false;
    }
    for (int s : p) prepareFd(s);
    aOut = p[0];
    bOut = p[1];
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
    socklen_t len = 0;
    int af = AF_INET;
    bool dualStack = false;
    auto setV4 = [&](in_addr_t addrHostOrder) {
        auto* a = reinterpret_cast<sockaddr_in*>(&ss);
        a->sin_family = AF_INET;
        a->sin_addr.s_addr = htonl(addrHostOrder);
        a->sin_port = htons(static_cast<uint16_t>(port));
        len = sizeof(sockaddr_in);
        af = AF_INET;
    };
    auto setV6 = [&](const in6_addr& addr) {
        auto* a = reinterpret_cast<sockaddr_in6*>(&ss);
        a->sin6_family = AF_INET6;
        a->sin6_addr = addr;
        a->sin6_port = htons(static_cast<uint16_t>(port));
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

    int s = createSocket(af);
    if (s < 0) {
        error = "Unable to create socket: " + lastSocketErrorMessage();
        return kInvalidSocket;
    }
    // POSIX SO_REUSEADDR only lets a listener reuse a port whose old connections linger in TIME_WAIT (unlike
    // Windows', it does not let another socket take a port in use).
    setIntOpt(s, SOL_SOCKET, SO_REUSEADDR, 1);
    if (af == AF_INET6) setIntOpt(s, IPPROTO_IPV6, IPV6_V6ONLY, dualStack ? 0 : 1);

    if (::bind(s, reinterpret_cast<sockaddr*>(&ss), len) != 0) {
        error = "Unable to bind to " + formatHostPort(numericHost(reinterpret_cast<sockaddr*>(&ss), len), port) + ": " +
                lastSocketErrorMessage();
        closeFd(s);
        return kInvalidSocket;
    }
    if (::listen(s, SOMAXCONN) != 0) {
        error = "Unable to listen: " + lastSocketErrorMessage();
        closeFd(s);
        return kInvalidSocket;
    }
    if (boundPort) *boundPort = localPort(s);
    return s;
}

SocketHandle acceptConnection(SocketHandle listener, bool noDelay) {
    sockaddr_storage peer{};
    socklen_t len = sizeof peer;
    int c;
    do {
        c = ::accept(fd(listener), reinterpret_cast<sockaddr*>(&peer), &len);
    } while (c < 0 && errno == EINTR);
    if (c < 0) return kInvalidSocket;
    prepareFd(c);
    if (noDelay) setIntOpt(c, IPPROTO_TCP, TCP_NODELAY, 1);
    return c;
}

/* ------------------------------------------------------------------------ */
/* I/O                                                                       */
/* ------------------------------------------------------------------------ */

NetErr lastNetError() {
    const int err = errno;
    if (err == 0) return NetErr::None;
    return err == EAGAIN || err == EWOULDBLOCK ? NetErr::WouldBlock : NetErr::Other;
}

long sendSome(SocketHandle s, const void* data, std::size_t n, bool urgent) {
    for (;;) {
        const ssize_t sent = ::send(fd(s), data, n, urgent ? MSG_OOB : 0);
        if (sent >= 0 || errno != EINTR) return static_cast<long>(sent);
    }
}

bool sendAll(SocketHandle s, std::string_view data) {
    if (s == kInvalidSocket) return false;
    while (!data.empty()) {
        const ssize_t n = ::send(fd(s), data.data(), std::min<size_t>(data.size(), 1 << 20), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) return false;
            // Non-blocking socket: wait until writable.
            pollfd p{fd(s), POLLOUT, 0};
            if (pollFor(&p, 1, -1) < 0 || (p.revents & (POLLERR | POLLNVAL))) return false;
            continue;
        }
        data.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

long recvSome(SocketHandle s, void* buf, std::size_t n) {
    for (;;) {
        const ssize_t got = ::recv(fd(s), buf, n, 0);
        if (got >= 0 || errno != EINTR) return static_cast<long>(got);
    }
}

int recvUrgentByte(SocketHandle s) {
    char b = 0;
    return ::recv(fd(s), &b, 1, MSG_OOB) == 1 ? static_cast<unsigned char>(b) : -1;
}

int waitReadable(SocketHandle s, int timeoutMs) {
    pollfd p{fd(s), POLLIN, 0};
    const int n = pollFor(&p, 1, timeoutMs);
    if (n < 0) return -1;
    return n > 0 ? 1 : 0;
}

void setNonBlocking(SocketHandle s, bool nonBlocking) { setNonBlockingFd(fd(s), nonBlocking); }

void shutdownWrite(SocketHandle s) { ::shutdown(fd(s), SHUT_WR); }

void shutdownBoth(SocketHandle s) { ::shutdown(fd(s), SHUT_RDWR); }

void closeSocket(SocketHandle& s) {
    if (s != kInvalidSocket) ::close(fd(s));
    s = kInvalidSocket;
}

void interruptSocket(SocketHandle& s) {
    if (s != kInvalidSocket) ::shutdown(fd(s), SHUT_RDWR);
}

std::string peerName(SocketHandle s) {
    sockaddr_storage ss{};
    socklen_t len = sizeof ss;
    if (s == kInvalidSocket || getpeername(fd(s), reinterpret_cast<sockaddr*>(&ss), &len) != 0) return {};
    return formatSockaddr(reinterpret_cast<sockaddr*>(&ss), len);
}

std::string localName(SocketHandle s) {
    sockaddr_storage ss{};
    socklen_t len = sizeof ss;
    if (s == kInvalidSocket || getsockname(fd(s), reinterpret_cast<sockaddr*>(&ss), &len) != 0) return {};
    return formatSockaddr(reinterpret_cast<sockaddr*>(&ss), len);
}

std::optional<std::pair<std::string, int>> peerAddress(SocketHandle s) {
    sockaddr_storage ss{};
    socklen_t len = sizeof ss;
    if (s == kInvalidSocket || getpeername(fd(s), reinterpret_cast<sockaddr*>(&ss), &len) != 0) return std::nullopt;
    std::string host = numericHost(reinterpret_cast<sockaddr*>(&ss), len);
    if (host.empty()) return std::nullopt;
    return std::make_pair(host, sockaddrPort(reinterpret_cast<sockaddr*>(&ss)));
}

int localPort(SocketHandle s) {
    sockaddr_storage ss{};
    socklen_t len = sizeof ss;
    if (s == kInvalidSocket || getsockname(fd(s), reinterpret_cast<sockaddr*>(&ss), &len) != 0) return 0;
    return sockaddrPort(reinterpret_cast<sockaddr*>(&ss));
}

const short kPollIn = POLLIN;
const short kPollOut = POLLOUT;

/* ------------------------------------------------------------------------ */
/* PollWaker: a pipe (poll and libssh's ssh_event take any descriptor)       */
/* ------------------------------------------------------------------------ */

bool PollWaker::open(std::string& error) {
    int r = -1, w = -1;
    if (!makeWakePipe(r, w, error)) return false;
    std::lock_guard lock(mutex_);
    recv_ = r;
    send_ = w;
    return true;
}

void PollWaker::signal() {
    std::lock_guard lock(mutex_);
    if (send_ == kInvalidSocket) return;
    const char byte = 1;
    // A full pipe means a wake-up is already pending, which is all we need.
    (void)!::write(fd(send_), &byte, 1);
}

void PollWaker::drain() {
    if (recv_ != kInvalidSocket) drainPipe(fd(recv_));
}

void PollWaker::close() {
    std::lock_guard lock(mutex_);
    closeSocket(send_);
    closeSocket(recv_);
}

}  // namespace st::sys
