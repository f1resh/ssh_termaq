// I/O engine shared by the Telnet / Rlogin / Raw transports (see SocketTransport.h).
#include "net/SocketTransport.h"

#include "core/Strings.h"
#include "platform/sys/Debug.h"
#include "platform/sys/Errors.h"

#include <algorithm>
#include <climits>
#include <exception>

namespace st {

namespace {

using Clock = std::chrono::steady_clock;

/** Upper bound of bytes read per loop iteration, so queued writes / close requests are never starved. */
constexpr size_t kMaxReadPerIteration = 1 << 20;

CloseInfo userClosed() {
    CloseInfo ci;
    ci.clean = true;
    ci.reason = "Connection closed by user";
    return ci;
}

CloseInfo errorClose(std::string reason) {
    CloseInfo ci;
    ci.error = true;
    ci.reason = std::move(reason);
    return ci;
}

}  // namespace

SocketTransport::SocketTransport(TransportContext ctx) : ctx_(std::move(ctx)) {
    reqCols_ = cols_ = ctx_.cols > 0 ? ctx_.cols : 80;
    reqRows_ = rows_ = ctx_.rows > 0 ? ctx_.rows : 24;
    reqWidthPx_ = widthPx_ = ctx_.widthPx;
    reqHeightPx_ = heightPx_ = ctx_.heightPx;
}

SocketTransport::~SocketTransport() { stopAndJoin(); }

void SocketTransport::stopAndJoin() {
    close();
    if (thread_.joinable()) thread_.join();
}

void SocketTransport::start() {
    if (started_.exchange(true)) return;
    thread_ = std::thread([this] { run(); });
}

void SocketTransport::write(std::string bytes) {
    if (bytes.empty()) return;
    {
        std::lock_guard lock(mu_);
        pendingWrite_ += bytes;
    }
    waiter_.wake();
}

void SocketTransport::resize(int cols, int rows, int widthPx, int heightPx) {
    {
        std::lock_guard lock(mu_);
        reqCols_ = cols > 0 ? cols : reqCols_;
        reqRows_ = rows > 0 ? rows : reqRows_;
        reqWidthPx_ = std::max(0, widthPx);
        reqHeightPx_ = std::max(0, heightPx);
        resizePending_ = true;
    }
    waiter_.wake();
}

void SocketTransport::sendSpecial(const std::string& id) {
    {
        std::lock_guard lock(mu_);
        pendingSpecials_.push_back(id);
    }
    waiter_.wake();
}

void SocketTransport::close() {
    {
        std::lock_guard lock(mu_);
        closeRequested_ = true;
    }
    cancel_ = true;
    waiter_.wake();
}

/* ------------------------------------------------------------------------ */
/* Helpers for derived classes (I/O thread)                                   */
/* ------------------------------------------------------------------------ */

void SocketTransport::sendRaw(std::string_view bytes) {
    if (bytes.empty() || finished_ || eofSent_ || sock_ == sys::kInvalidSocket) return;
    if (!out_.empty() && !out_.back().urgent && !out_.back().eof)
        out_.back().data.append(bytes);
    else
        out_.push_back(OutSegment{std::string(bytes), false, false});
    flushOutput();
}

void SocketTransport::sendUrgent(unsigned char byte) {
    if (finished_ || eofSent_ || sock_ == sys::kInvalidSocket) return;
    out_.push_back(OutSegment{std::string(1, static_cast<char>(byte)), true, false});
    flushOutput();
}

void SocketTransport::sendEof() {
    if (finished_ || eofSent_ || sock_ == sys::kInvalidSocket) return;
    out_.push_back(OutSegment{{}, false, true});
    flushOutput();
}

void SocketTransport::deliver(std::string_view bytes) {
    if (!bytes.empty() && ctx_.listener && !finished_) ctx_.listener->onData(bytes);
}

void SocketTransport::log(LogLevel level, const std::string& msg) {
    if (ctx_.listener) ctx_.listener->onLog(level, msg);
}

void SocketTransport::ldiscChanged(LdiscModes modes) {
    if (ctx_.listener && !finished_) ctx_.listener->onLdiscChanged(modes);
}

void SocketTransport::finish(CloseInfo info) {
    if (finished_) return;
    finished_ = true;
    closeInfo_ = std::move(info);
}

/* ------------------------------------------------------------------------ */
/* I/O thread                                                                */
/* ------------------------------------------------------------------------ */

void SocketTransport::run() {
    sys::setThreadName("st-" + toLower(protocolName()));
    try {
        const std::string host = sys::stripBrackets(trim(ctx_.session.host));
        const int port = ctx_.session.port > 0 ? ctx_.session.port : defaultPort(ctx_.session.protocol);
        if (ctx_.listener) ctx_.listener->onStatus(ConnStatus::Connecting, "Connecting to " + sys::formatHostPort(host, port));

        std::string error;
        if (cancel_) {
            finish(userClosed());
        } else if (!prepare(error)) {
            finish(cancel_ ? userClosed() : errorClose(error));
        } else {
            std::string connectedTo;
            OpenOptions oo = openOptions();
            oo.connectedTo = &connectedTo;
            sock_ = openConnection(ctx_, host, port, error, &cancel_, oo);
            if (sock_ == sys::kInvalidSocket) {
                finish(cancel_ ? userClosed() : errorClose(error));
            } else {
                log(LogLevel::Info, "Connected to " + connectedTo);
                if (!waiter_.attach(sock_, urgentInline(), error)) {
                    finish(errorClose("Network error: " + error));
                } else {
                    if (ctx_.listener) ctx_.listener->onStatus(ConnStatus::Connected, "Connected");
                    lastSend_ = Clock::now();
                    onConnected();
                    loop();
                }
            }
        }
    } catch (const std::exception& e) {
        finish(errorClose(std::string("Internal error: ") + e.what()));
    } catch (...) {
        finish(errorClose("Internal error"));
    }
    if (!finished_) finish(userClosed());
    closeConnection(sock_);
    waiter_.detach();
    if (ctx_.listener) ctx_.listener->onClose(closeInfo_);
}

void SocketTransport::loop() {
    while (!finished_) {
        drainRequests();
        if (finished_) break;
        flushOutput();
        if (finished_) break;

        int timeout = -1;
        const int keepalive = keepaliveIntervalMs();
        if (keepalive > 0) {
            const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - lastSend_).count();
            timeout = static_cast<int>(std::clamp<long long>(keepalive - idle, 0, INT_MAX));
        }
        sys::SocketWaiter::Events ev;
        std::string waitError;
        if (!waiter_.wait(timeout, ev, waitError)) {
            finish(errorClose("Network error: " + waitError));
            break;
        }
        if (ev.timedOut) {
            if (keepalive > 0 &&
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - lastSend_).count() >= keepalive) {
                onKeepalive();
                lastSend_ = Clock::now();  // also when the hook sent nothing: avoid spinning
            }
            continue;
        }

        if (ev.urgent) readUrgent();
        if (!finished_ && ev.readable) readAvailable();
        if (!finished_ && ev.writable) flushOutput();
        if (!finished_ && ev.closed) {
            // Drain whatever is still buffered, then report the close.
            while (!finished_ && readAvailable()) {
            }
            if (!finished_) {
                if (!ev.closeError.empty()) {
                    finish(errorClose("Network error: " + ev.closeError));
                } else {
                    CloseInfo ci;
                    ci.clean = true;
                    ci.reason = "Remote side closed the connection";
                    finish(ci);
                }
            }
        }
    }
    // Best effort: push out anything still queued (e.g. the last keystrokes before a user close).
    if (!out_.empty() && sock_ != sys::kInvalidSocket) {
        for (const auto& seg : out_) {
            if (seg.eof || seg.data.empty()) continue;
            sys::sendSome(sock_, seg.data.data(), seg.data.size(), seg.urgent);
        }
        out_.clear();
    }
}

void SocketTransport::drainRequests() {
    std::string data;
    std::deque<std::string> specials;
    bool resizeNow = false, closeNow = false;
    {
        std::lock_guard lock(mu_);
        data.swap(pendingWrite_);
        specials.swap(pendingSpecials_);
        if (resizePending_) {
            resizeNow = true;
            resizePending_ = false;
            cols_ = reqCols_;
            rows_ = reqRows_;
            widthPx_ = reqWidthPx_;
            heightPx_ = reqHeightPx_;
        }
        closeNow = closeRequested_;
    }
    if (resizeNow && !finished_) onResize();
    if (!data.empty() && !finished_) onUserData(data);
    for (const auto& id : specials) {
        if (finished_) break;
        onSpecial(id);
    }
    if (closeNow) finish(userClosed());
}

void SocketTransport::flushOutput() {
    while (!out_.empty() && !finished_ && sock_ != sys::kInvalidSocket) {
        OutSegment& seg = out_.front();
        if (seg.eof) {
            sys::shutdownWrite(sock_);
            eofSent_ = true;
            out_.clear();
            waiter_.wantWrite(false);
            return;
        }
        if (seg.data.empty()) {
            out_.pop_front();
            continue;
        }
        const size_t chunk = std::min<size_t>(seg.data.size(), 1 << 20);
        const long n = sys::sendSome(sock_, seg.data.data(), chunk, seg.urgent);
        if (n < 0) {
            if (sys::lastNetError() == sys::NetErr::WouldBlock) {
                waiter_.wantWrite(true);  // resumed when the socket has room again
                return;
            }
            finish(errorClose("Network error: " + sys::lastSocketErrorMessage()));
            return;
        }
        lastSend_ = Clock::now();
        seg.data.erase(0, static_cast<size_t>(n));
        if (seg.data.empty()) out_.pop_front();
    }
    if (out_.empty()) waiter_.wantWrite(false);
}

bool SocketTransport::readAvailable() {
    char buf[65536];
    size_t total = 0;
    while (!finished_ && total < kMaxReadPerIteration) {
        const long n = sys::recvSome(sock_, buf, sizeof(buf));
        if (n > 0) {
            total += static_cast<size_t>(n);
            onReceive(std::string_view(buf, static_cast<size_t>(n)));
            continue;
        }
        if (n == 0) {
            CloseInfo ci;
            ci.clean = true;
            ci.reason = "Remote side closed the connection";
            finish(ci);
            return false;
        }
        if (sys::lastNetError() == sys::NetErr::WouldBlock) return false;
        finish(errorClose("Network error: " + sys::lastSocketErrorMessage()));
        return false;
    }
    return !finished_;
}

void SocketTransport::readUrgent() {
    for (int i = 0; i < 64 && !finished_; ++i) {
        const int b = sys::recvUrgentByte(sock_);
        if (b < 0) break;
        onUrgent(static_cast<unsigned char>(b));
    }
}

}  // namespace st
