#pragma once
// Small socket helpers shared by the in-process test servers and the socket tests (internal to native/tests). Most
// run on the platform layer (platform/sys/Net.h); the raw ones a test peer needs (abortive close, urgent bytes,
// waiting on many sockets) are per OS in native/tests/platform/<os>/support.
#include "platform/sys/Net.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::test::net {

using Handle = sys::SocketHandle;

/** Sets up sockets for the process once (sys::NetInit; idempotent, thread-safe, never torn down). */
void ensureNetInit();

/** Owning socket. */
class Socket {
public:
    Socket() = default;
    explicit Socket(Handle s) : s_(s) {}
    ~Socket() { reset(); }
    Socket(Socket&& o) noexcept : s_(std::exchange(o.s_, sys::kInvalidSocket)) {}
    Socket& operator=(Socket&& o) noexcept {
        if (this != &o) {
            reset();
            s_ = std::exchange(o.s_, sys::kInvalidSocket);
        }
        return *this;
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Handle get() const { return s_; }
    bool valid() const { return s_ != sys::kInvalidSocket; }
    explicit operator bool() const { return valid(); }
    Handle release() { return std::exchange(s_, sys::kInvalidSocket); }
    void reset(Handle s = sys::kInvalidSocket) {
        sys::closeSocket(s_);
        s_ = s;
    }

private:
    Handle s_ = sys::kInvalidSocket;
};

/** Listening TCP socket on 127.0.0.1:port (0 => ephemeral). Throws std::runtime_error on failure. */
Socket listenLoopback(int port, int& boundPort);
/** Local port of a bound socket (0 on error). */
int localPort(Handle s);
/** Peer "address", port of a connected socket. */
std::pair<std::string, int> peerAddress(Handle s);
/** Blocking TCP connect with timeout (IPv4 addresses first). Invalid socket on failure. */
Socket connectTcp(const std::string& host, int port, int timeoutMs);
bool setNonBlocking(Handle s, bool on);
/** Waits until the socket is readable (or has an error / hangup). Returns false on timeout. */
bool waitReadable(Handle s, int timeoutMs);
/** Sends all bytes (blocking socket). Returns false on error. */
bool sendAll(Handle s, std::string_view data);
/** Accepts one pending connection from a non-blocking listener (invalid socket if none). */
Socket acceptNonBlocking(Handle listener);
/** True when the last socket error means "try again later". */
bool wouldBlock();

/** One socket of pollSockets(): what to wait for, and what happened. */
struct PollItem {
    Handle sock = sys::kInvalidSocket;
    bool read = false;
    bool write = false;
    bool readable = false;  // also on hang-up or error: a read then reports it
    bool writable = false;
    bool failed = false;    // an error is pending on the socket
};
/** Waits up to timeoutMs until one of the sockets is ready (WSAPoll / poll); false when waiting failed. */
bool pollSockets(std::vector<PollItem>& items, int timeoutMs);

/* ---- edge-of-stream helpers for the socket semantics tests (native/tests/net/semantics_tests.cpp) ---- */

/** Waits up to timeoutMs for a pending connection and accepts it (invalid socket on timeout). */
Socket acceptWithin(Handle listener, int timeoutMs);
/** Waits up to timeoutMs and receives up to n bytes: > 0 bytes, 0 = the peer closed, -1 = timeout or error. */
int recvWithin(Handle s, char* buf, int n, int timeoutMs);
/** Sends one byte as TCP urgent (out-of-band) data. */
bool sendUrgent(Handle s, unsigned char byte);
/** Waits for and reads one urgent byte (socket without SO_OOBINLINE); -1 on timeout. */
int recvUrgent(Handle s, int timeoutMs);
/** Half-closes the sending direction (the peer sees FIN). */
void shutdownSend(Handle s);
/** Closes with RST instead of FIN (SO_LINGER with a zero timeout). */
void abortiveClose(Socket& s);
/** Sets SO_RCVBUF (on a listener: inherited by accepted sockets). */
void setReceiveBuffer(Handle s, int bytes);
/** CPU time used by this process so far (user + system), in seconds. */
double processCpuSeconds();

}  // namespace st::test::net
