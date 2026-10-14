#pragma once
// Small Winsock helpers shared by the in-process test servers (internal to native/tests/support).
#include "core/Win.h"

#include <string>
#include <string_view>
#include <utility>

namespace st::test::net {

/** Initialises Winsock once per process (idempotent, thread-safe). */
void ensureWinsock();

/** Owning SOCKET handle. */
class Socket {
public:
    Socket() = default;
    explicit Socket(SOCKET s) : s_(s) {}
    ~Socket() { reset(); }
    Socket(Socket&& o) noexcept : s_(std::exchange(o.s_, INVALID_SOCKET)) {}
    Socket& operator=(Socket&& o) noexcept {
        if (this != &o) {
            reset();
            s_ = std::exchange(o.s_, INVALID_SOCKET);
        }
        return *this;
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    SOCKET get() const { return s_; }
    bool valid() const { return s_ != INVALID_SOCKET; }
    explicit operator bool() const { return valid(); }
    SOCKET release() { return std::exchange(s_, INVALID_SOCKET); }
    void reset(SOCKET s = INVALID_SOCKET) {
        if (s_ != INVALID_SOCKET) closesocket(s_);
        s_ = s;
    }

private:
    SOCKET s_ = INVALID_SOCKET;
};

/** Listening TCP socket on 127.0.0.1:port (0 => ephemeral). Throws std::runtime_error on failure. */
Socket listenLoopback(int port, int& boundPort);
/** Local port of a bound socket (0 on error). */
int localPort(SOCKET s);
/** Peer "address", port of a connected socket. */
std::pair<std::string, int> peerAddress(SOCKET s);
/** Blocking TCP connect with timeout (resolves host via getaddrinfo, tries each address). Invalid socket on failure. */
Socket connectTcp(const std::string& host, int port, int timeoutMs);
bool setNonBlocking(SOCKET s, bool on);
/** Waits until the socket is readable (or has an error / hangup). Returns false on timeout. */
bool waitReadable(SOCKET s, int timeoutMs);
/** Sends all bytes (blocking socket). Returns false on error. */
bool sendAll(SOCKET s, std::string_view data);
/** Accepts one pending connection from a non-blocking listener (invalid socket if none). */
Socket acceptNonBlocking(SOCKET listener);
/** True when the last Winsock error means "try again later". */
bool wouldBlock();

}  // namespace st::test::net
