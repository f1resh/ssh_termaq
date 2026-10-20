#pragma once
// TCP sockets for the network transports: name resolution, connecting (staggered attempts across addresses,
// cancellable), listening, socket pairs, I/O on connected sockets and the two ways a transport thread waits:
// SocketWaiter (one socket + wake-ups, for the Telnet/Rlogin/Raw engine) and PollWaker (a wake-up handle for loops
// that poll elsewhere, e.g. libssh's ssh_event).
// Sockets are blocking unless setNonBlocking() / SocketWaiter::attach() made them non-blocking, and are never
// inherited by child processes (local proxy commands).
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::sys {

/** A socket: a SOCKET on Windows, a file descriptor on POSIX. */
using SocketHandle = std::intptr_t;
inline constexpr SocketHandle kInvalidSocket = -1;

enum class IpFamily { Auto, IPv4, IPv6 };

/** Process-wide socket setup: create one in main() before any networking (Windows: WSAStartup). */
class NetInit {
public:
    NetInit();
    ~NetInit();
    NetInit(const NetInit&) = delete;
    NetInit& operator=(const NetInit&) = delete;
};

/* ---- addresses -------------------------------------------------------------------------------------------- */

/** True for numeric IPv4 / IPv6 literals ("[::1]" and scoped "fe80::1%3" accepted); `family` receives which. */
bool isNumericAddress(const std::string& host, IpFamily* family = nullptr);
/** "[2001:db8::1]" -> "2001:db8::1"; anything else unchanged. */
std::string stripBrackets(const std::string& host);
/** "host:port", bracketing IPv6 literals ("[::1]:22"). */
std::string formatHostPort(const std::string& host, int port);
/** Network-order bytes of a numeric address: 4 (IPv4) or 16 (IPv6); empty when `host` is not numeric. */
std::string addressBytes(const std::string& host);
/** Numeric text of 4 or 16 network-order address bytes; empty for any other length. */
std::string formatAddressBytes(std::string_view bytes);

/**
 * Resolves `host` to numeric addresses ("192.0.2.1", "2001:db8::1"), in resolver order. Honours `family`, polls
 * `cancel` at least every 100 ms ("Cancelled") and gives up after timeoutMs (<= 0: no limit). Empty + error on failure.
 */
std::vector<std::string> resolveHost(const std::string& host, IpFamily family, std::string& error,
                                     const std::atomic<bool>* cancel = nullptr, int timeoutMs = 20000);

/* ---- connections ------------------------------------------------------------------------------------------ */

struct ConnectOptions {
    IpFamily family = IpFamily::Auto;
    int timeoutMs = 20000;
    bool noDelay = true;
    bool keepAlive = false;
    /** Optional local source port range (rlogin privileged ports): ports are tried from localPortHigh down to
     *  localPortLow; when none can be bound an ephemeral port is used. 0/0 => ephemeral. */
    int localPortHigh = 0;
    int localPortLow = 0;
    /** Optional progress callback for event-log lines ("Looking up host ...", "Connecting to 192.0.2.1 port 22"). */
    std::function<void(const std::string&)> log;
};

/**
 * Resolves host and connects (staggered attempts across the addresses, RFC 8305 style, with a timeout). Polls
 * `cancel` (if given) at least every 100 ms and aborts with error "Cancelled". Returns kInvalidSocket with `error`
 * set on failure. The returned socket is in blocking mode. `resolvedAddress` receives the numeric address used.
 */
SocketHandle connectTcp(const std::string& host, int port, const ConnectOptions& opts, std::string& error,
                        const std::atomic<bool>* cancel = nullptr, std::string* resolvedAddress = nullptr);
/** Connected socket pair (a <-> b), both blocking with TCP_NODELAY where it applies. */
bool makeSocketPair(SocketHandle& a, SocketHandle& b, std::string& error);
/** A blocking stream socket connected to the Unix-domain socket at `path` (ssh-agent, X11 displays): never
 *  inherited, no SIGPIPE. Invalid + `error` on failure; Windows: always (its X servers and agents use TCP / pipes). */
SocketHandle connectUnix(const std::string& path, std::string& error);
/** This machine's host name ("" when unknown). */
std::string localHostName();
/** Listens on bindAddress:port ('' => loopback, '*' or '0.0.0.0' => all IPv4, '::' => all IPv6). port 0 => ephemeral. */
SocketHandle listenTcp(const std::string& bindAddress, int port, IpFamily family, std::string& error,
                       int* boundPort = nullptr);
/** A pending connection of a listener (not inheritable, TCP_NODELAY when `noDelay`); kInvalidSocket if none. */
SocketHandle acceptConnection(SocketHandle listener, bool noDelay);

/* ---- I/O on a connected socket ---------------------------------------------------------------------------- */

enum class NetErr { None, WouldBlock, Other };
/** Kind of the last failed socket call on this thread (sys::lastSocketErrorMessage() has its text). */
NetErr lastNetError();

/** Sends up to n bytes (as TCP urgent data when `urgent`): bytes sent, or -1 (lastNetError()). */
long sendSome(SocketHandle s, const void* data, std::size_t n, bool urgent = false);
/** Blocking send of all bytes (waits while a non-blocking socket is full); false on error. */
bool sendAll(SocketHandle s, std::string_view data);
/** Receives up to n bytes: > 0 bytes, 0 when the peer closed, -1 on error (lastNetError()). */
long recvSome(SocketHandle s, void* buf, std::size_t n);
/** Reads one byte of TCP urgent data (socket without inline urgent data): 0..255, or -1 when there is none. */
int recvUrgentByte(SocketHandle s);
/** Waits until `s` is readable: 1, 0 on timeout, -1 on error. */
int waitReadable(SocketHandle s, int timeoutMs);
void setNonBlocking(SocketHandle s, bool nonBlocking);
/** Half-closes the sending direction (the peer sees EOF once queued data is sent). */
void shutdownWrite(SocketHandle s);
/** Shuts down both directions (cancels a connection that another thread uses). */
void shutdownBoth(SocketHandle s);
/** Closes and sets kInvalidSocket (no-op when already invalid). */
void closeSocket(SocketHandle& s);
/**
 * Makes another thread that is blocked in recv/send on `s` return. Windows closes the socket (shutdown does not wake
 * a blocked recv there) and sets `s` invalid; POSIX shuts it down and leaves closing to closeSocket() after that
 * thread finished.
 */
void interruptSocket(SocketHandle& s);

/** "host:port" / "[v6]:port" of the peer / the local end, '' on failure. */
std::string peerName(SocketHandle s);
std::string localName(SocketHandle s);
/** Numeric address and port of the peer; nullopt on failure. */
std::optional<std::pair<std::string, int>> peerAddress(SocketHandle s);
/** Local port of a bound socket, 0 on failure. */
int localPort(SocketHandle s);

/* ---- waiting ---------------------------------------------------------------------------------------------- */

/**
 * The wait of a socket transport's I/O thread: one connected socket plus wake-ups from other threads. Socket events
 * are reported the way Windows' WSAEventSelect does on every OS: `readable` again while unread data remains,
 * `writable` once after a send could not take everything, `urgent` for TCP urgent data (when it is not inline),
 * `closed` once when the peer closed the connection (with `closeError` text when it was reset).
 */
class SocketWaiter {
public:
    struct Events {
        bool woken = false;     // wake() was called
        bool timedOut = false;  // nothing happened within the timeout
        bool readable = false;
        bool writable = false;
        bool urgent = false;
        bool closed = false;
        std::string closeError;  // empty: closed normally
    };

    SocketWaiter();  // throws std::runtime_error when the OS cannot create the wake-up object
    ~SocketWaiter();
    SocketWaiter(const SocketWaiter&) = delete;
    SocketWaiter& operator=(const SocketWaiter&) = delete;

    /** Thread-safe; a wake-up before attach() or during a wait is kept for the next wait(). */
    void wake();
    /** Starts watching `s` and makes it non-blocking. `urgentInline`: urgent data arrives in the normal stream. */
    bool attach(SocketHandle s, bool urgentInline, std::string& error);
    /** Stops watching (call before closing the socket's handle is reused elsewhere). */
    void detach();
    /** Tells the waiter whether output is queued (a send could not take everything). */
    void wantWrite(bool on);
    /** Waits for events or the timeout (-1 = no timeout). False with `error` set when waiting failed. */
    bool wait(int timeoutMs, Events& events, std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/** The poll() "readable" / "writable" event masks (POLLIN / POLLOUT; values differ by OS), e.g. for libssh's
 *  ssh_event_add_fd. */
extern const short kPollIn;
extern const short kPollOut;

/**
 * Wakes a thread that polls a set of handles (libssh's ssh_event): include pollHandle() in the set for reading;
 * signal() from any thread makes it readable, the polling thread calls drain(). signal() before open() or after
 * close() does nothing.
 */
class PollWaker {
public:
    PollWaker() = default;
    ~PollWaker() { close(); }
    PollWaker(const PollWaker&) = delete;
    PollWaker& operator=(const PollWaker&) = delete;

    bool open(std::string& error);
    void signal();
    void drain();
    void close();
    SocketHandle pollHandle() const { return recv_; }

private:
    std::mutex mutex_;
    SocketHandle recv_ = kInvalidSocket;
    SocketHandle send_ = kInvalidSocket;
};

}  // namespace st::sys
