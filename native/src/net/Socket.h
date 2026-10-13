#pragma once
// Winsock helpers shared by all network transports.
#include "core/Types.h"
#include "core/Win.h"

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace sc {

/** RAII WSAStartup/WSACleanup. Create one in main() before any networking. */
class WsaInit {
public:
    WsaInit();
    ~WsaInit();
    WsaInit(const WsaInit&) = delete;
    WsaInit& operator=(const WsaInit&) = delete;
};

struct ConnectOptions {
    AddressFamily family = AddressFamily::Auto;
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
 * Resolve host and connect (tries each address in turn, non-blocking connect with timeout).
 * Polls `cancel` (if given) at least every 100 ms and aborts with error "Cancelled".
 * Returns INVALID_SOCKET with `error` set on failure. The returned socket is in blocking mode.
 * `resolvedAddress` (optional) receives the numeric address actually connected to.
 */
SOCKET connectTcp(const std::string& host, int port, const ConnectOptions& opts, std::string& error,
                  const std::atomic<bool>* cancel = nullptr, std::string* resolvedAddress = nullptr);

/** Connected loopback TCP socket pair (a <-> b). Both blocking, TCP_NODELAY. */
bool makeSocketPair(SOCKET& a, SOCKET& b, std::string& error);

/** Listen on bindAddress:port ('' => loopback, '*' or '0.0.0.0' => all IPv4, '::' => all IPv6). port 0 => ephemeral. */
SOCKET listenTcp(const std::string& bindAddress, int port, AddressFamily family, std::string& error, int* boundPort = nullptr);

/** closesocket + set INVALID_SOCKET (no-op when already invalid). */
void closeSocket(SOCKET& s);
/** Blocking send of all bytes; false on error. */
bool sendAll(SOCKET s, std::string_view data);
void setNonBlocking(SOCKET s, bool nonBlocking);
/** "host:port" / "[v6]:port" of the peer, '' on failure. */
std::string peerName(SOCKET s);

/* ---- additions (net module) ------------------------------------------------------------------------ */

/**
 * Resolve `host` to numeric addresses ("192.0.2.1", "2001:db8::1"), in resolver order. Honours `family`, polls
 * `cancel` at least every 100 ms ("Cancelled") and gives up after timeoutMs (<= 0: no limit). Empty + error on failure.
 */
std::vector<std::string> resolveHost(const std::string& host, AddressFamily family, std::string& error,
                                     const std::atomic<bool>* cancel = nullptr, int timeoutMs = 20000);
/** "host:port" / "[v6]:port" of the local end, '' on failure. */
std::string localName(SOCKET s);
/** Local port of a bound socket, 0 on failure. */
int localPort(SOCKET s);
/** True for numeric IPv4 / IPv6 literals ("[::1]" accepted). `family` (optional) receives AF_INET / AF_INET6. */
bool isNumericAddress(const std::string& host, int* family = nullptr);
/** "[2001:db8::1]" -> "2001:db8::1"; anything else unchanged. */
std::string stripBrackets(const std::string& host);
/** "host:port", bracketing IPv6 literals ("[::1]:22"). */
std::string formatHostPort(const std::string& host, int port);

/**
 * Wakes a thread blocked in WSAWaitForMultipleEvents / select-style loops: wraps a manual-reset WSAEVENT
 * that other threads signal(). Loops include event() in their wait set and call reset() after waking.
 */
class Waker {
public:
    Waker();
    ~Waker();
    Waker(const Waker&) = delete;
    Waker& operator=(const Waker&) = delete;
    void signal();
    void reset();
    WSAEVENT event() const { return event_; }

private:
    WSAEVENT event_;
};

}  // namespace sc
