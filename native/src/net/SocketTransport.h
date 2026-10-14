#pragma once
// Shared I/O engine for the TCP based non-SSH transports (Telnet, Rlogin, Raw). It implements the threading and
// lifecycle contract of transport/Transport.h once:
//  - start() spawns one I/O thread which connects via openConnection() (proxies, exclusions, cancellation),
//    then waits on the socket's network events and a Waker (WSAWaitForMultipleEvents).
//  - write()/resize()/sendSpecial()/close() only queue work under a mutex and signal the Waker.
//  - Every listener callback is made from the I/O thread; onClose() is delivered exactly once, last.
// Protocol classes derive from it and override the hooks below (all hooks run on the I/O thread).
// IMPORTANT: a derived class must call stopAndJoin() in its destructor, so that the I/O thread (which calls the
// virtual hooks) has finished before the derived part of the object is destroyed.
#include "net/Proxy.h"
#include "net/Socket.h"
#include "transport/Transport.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace st {

class SocketTransport : public Transport {
public:
    explicit SocketTransport(TransportContext ctx);
    ~SocketTransport() override;
    SocketTransport(const SocketTransport&) = delete;
    SocketTransport& operator=(const SocketTransport&) = delete;

    void start() override;
    void write(std::string bytes) override;
    void resize(int cols, int rows, int widthPx, int heightPx) override;
    void sendSpecial(const std::string& id) override;
    void close() override;

protected:
    /* ---- hooks (I/O thread) ----------------------------------------------------------------------- */

    /** Before connecting (may prompt). Return false with `error` set to fail the connection. */
    virtual bool prepare(std::string& /*error*/) { return true; }
    /** Extra connection options (e.g. rlogin's privileged source ports). */
    virtual OpenOptions openOptions() { return {}; }
    /** Deliver TCP urgent data inline (telnet) or through onUrgent() (rlogin). */
    virtual bool urgentInline() const { return true; }
    /** Socket connected: send the protocol handshake / initial negotiation. */
    virtual void onConnected() {}
    /** Bytes received from the network. */
    virtual void onReceive(std::string_view data) = 0;
    /** One byte of TCP urgent data (only when urgentInline() is false). */
    virtual void onUrgent(unsigned char /*byte*/) {}
    /** Bytes from write(); default: sent unchanged. */
    virtual void onUserData(std::string_view data) { sendRaw(data); }
    /** Terminal size changed (cols()/rows()/widthPx()/heightPx() hold the new values). */
    virtual void onResize() {}
    /** Special command from sendSpecial(). */
    virtual void onSpecial(const std::string& /*id*/) {}
    /** Keepalive interval in ms after the last transmission (0 = none) and the action to take. */
    virtual int keepaliveIntervalMs() const { return 0; }
    virtual void onKeepalive() {}
    /** Name used in messages ("Telnet", "Rlogin", "Raw"). */
    virtual std::string protocolName() const = 0;

    /* ---- helpers (I/O thread) --------------------------------------------------------------------- */

    /** Queue bytes for the network (sent in order; partial sends continue on FD_WRITE). */
    void sendRaw(std::string_view bytes);
    /** Queue one byte to be sent as TCP urgent data after everything queued before it (telnet Synch). */
    void sendUrgent(unsigned char byte);
    /** Half-close the sending direction once all queued output has been sent. */
    void sendEof();
    void deliver(std::string_view bytes);
    void log(LogLevel level, const std::string& msg);
    void ldiscChanged(LdiscModes modes);
    /** End the connection: the loop stops, the socket is closed and listener->onClose(info) is delivered. */
    void finish(CloseInfo info);
    bool finished() const { return finished_; }

    const TransportContext& ctx() const { return ctx_; }
    const SessionConfig& session() const { return ctx_.session; }
    TransportListener* listener() const { return ctx_.listener; }
    int cols() const { return cols_; }
    int rows() const { return rows_; }
    int widthPx() const { return widthPx_; }
    int heightPx() const { return heightPx_; }

    /** Requests close and joins the I/O thread (idempotent). Derived destructors must call it. */
    void stopAndJoin();

private:
    struct OutSegment {
        std::string data;
        bool urgent = false;
        bool eof = false;
    };

    void run();
    void loop();
    void drainRequests();
    void flushOutput();
    bool readAvailable();
    void readUrgent();

    TransportContext ctx_;
    std::thread thread_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> started_{false};
    Waker waker_;

    // Requests from other threads (guarded by mu_).
    std::mutex mu_;
    std::string pendingWrite_;
    std::deque<std::string> pendingSpecials_;
    bool resizePending_ = false;
    int reqCols_ = 80, reqRows_ = 24, reqWidthPx_ = 0, reqHeightPx_ = 0;
    bool closeRequested_ = false;

    // I/O thread state.
    SOCKET sock_ = INVALID_SOCKET;
    WSAEVENT sockEvent_ = WSA_INVALID_EVENT;
    std::deque<OutSegment> out_;
    bool eofSent_ = false;
    bool finished_ = false;
    CloseInfo closeInfo_;
    int cols_ = 80, rows_ = 24, widthPx_ = 0, heightPx_ = 0;
    std::chrono::steady_clock::time_point lastSend_{};
};

}  // namespace st
