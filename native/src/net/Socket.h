#pragma once
// Winsock helpers shared by all network transports.
#include "core/Types.h"
#include "core/Win.h"

#include <atomic>
#include <string>
#include <string_view>

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
