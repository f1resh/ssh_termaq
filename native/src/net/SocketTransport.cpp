// I/O engine shared by the Telnet / Rlogin / Raw transports (see SocketTransport.h).
#include "net/SocketTransport.h"

#include "core/Strings.h"

#include <algorithm>
#include <climits>
#include <exception>

namespace sc {

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
    waker_.signal();
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
    waker_.signal();
}

void SocketTransport::sendSpecial(const std::string& id) {
    {
        std::lock_guard lock(mu_);
        pendingSpecials_.push_back(id);
    }
    waker_.signal();
}

void SocketTransport::close() {
    {
        std::lock_guard lock(mu_);
        closeRequested_ = true;
    }
    cancel_ = true;
    waker_.signal();
}

/* ------------------------------------------------------------------------ */
/* Helpers for derived classes (I/O thread)                                   */
/* ------------------------------------------------------------------------ */

void SocketTransport::sendRaw(std::string_view bytes) {
    if (bytes.empty() || finished_ || eofSent_ || sock_ == INVALID_SOCKET) return;
    if (!out_.empty() && !out_.back().urgent && !out_.back().eof)
        out_.back().data.append(bytes);
    else
        out_.push_back(OutSegment{std::string(bytes), false, false});
    flushOutput();
}

void SocketTransport::sendUrgent(unsigned char byte) {
    if (finished_ || eofSent_ || sock_ == INVALID_SOCKET) return;
    out_.push_back(OutSegment{std::string(1, static_cast<char>(byte)), true, false});
    flushOutput();
}

void SocketTransport::sendEof() {
    if (finished_ || eofSent_ || sock_ == INVALID_SOCKET) return;
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
    SetThreadDescription(GetCurrentThread(), widen("sc-" + toLower(protocolName())).c_str());
    try {
        const std::string host = stripBrackets(trim(ctx_.session.host));
        const int port = ctx_.session.port > 0 ? ctx_.session.port : defaultPort(ctx_.session.protocol);
        if (ctx_.listener) ctx_.listener->onStatus(ConnStatus::Connecting, "Connecting to " + formatHostPort(host, port));

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
            if (sock_ == INVALID_SOCKET) {
                finish(cancel_ ? userClosed() : errorClose(error));
            } else {
                log(LogLevel::Info, "Connected to " + connectedTo);
                sockEvent_ = WSACreateEvent();
                BOOL inlineOob = urgentInline() ? TRUE : FALSE;
                setsockopt(sock_, SOL_SOCKET, SO_OOBINLINE, reinterpret_cast<const char*>(&inlineOob), sizeof(inlineOob));
                if (sockEvent_ == WSA_INVALID_EVENT ||
                    WSAEventSelect(sock_, sockEvent_, FD_READ | FD_WRITE | FD_OOB | FD_CLOSE) == SOCKET_ERROR) {
                    finish(errorClose("Network error: " + lastWsaErrorMessage()));
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
    if (sockEvent_ != WSA_INVALID_EVENT) {
        WSACloseEvent(sockEvent_);
        sockEvent_ = WSA_INVALID_EVENT;
    }
    if (ctx_.listener) ctx_.listener->onClose(closeInfo_);
}

void SocketTransport::loop() {
    while (!finished_) {
        drainRequests();
        if (finished_) break;
        flushOutput();
        if (finished_) break;

        DWORD timeout = WSA_INFINITE;
        const int keepalive = keepaliveIntervalMs();
        if (keepalive > 0) {
            const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - lastSend_).count();
            timeout = static_cast<DWORD>(std::max<long long>(0, keepalive - idle));
        }
        WSAEVENT events[2] = {sockEvent_, waker_.event()};
        const DWORD rc = WSAWaitForMultipleEvents(2, events, FALSE, timeout, FALSE);
        if (rc == WSA_WAIT_FAILED) {
            finish(errorClose("Network error: " + lastWsaErrorMessage()));
            break;
        }
        if (rc == WSA_WAIT_TIMEOUT) {
            if (keepalive > 0 &&
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - lastSend_).count() >= keepalive) {
                onKeepalive();
                lastSend_ = Clock::now();  // also when the hook sent nothing: avoid spinning
            }
            continue;
        }
        if (rc == WSA_WAIT_EVENT_0 + 1) waker_.reset();

        WSANETWORKEVENTS ne{};
        if (WSAEnumNetworkEvents(sock_, sockEvent_, &ne) == SOCKET_ERROR) {
            finish(errorClose("Network error: " + lastWsaErrorMessage()));
            break;
        }
        if (ne.lNetworkEvents & FD_OOB) readUrgent();
        if (!finished_ && (ne.lNetworkEvents & FD_READ)) readAvailable();
        if (!finished_ && (ne.lNetworkEvents & FD_WRITE)) flushOutput();
        if (!finished_ && (ne.lNetworkEvents & FD_CLOSE)) {
            // Drain whatever is still buffered, then report the close.
            while (!finished_ && readAvailable()) {
            }
            if (!finished_) {
                const int err = ne.iErrorCode[FD_CLOSE_BIT];
                if (err != 0) {
                    finish(errorClose("Network error: " + winErrorMessage(static_cast<unsigned long>(err))));
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
    if (!out_.empty() && sock_ != INVALID_SOCKET) {
        for (const auto& seg : out_) {
            if (seg.eof || seg.data.empty()) continue;
            send(sock_, seg.data.data(), static_cast<int>(std::min<size_t>(seg.data.size(), INT_MAX)), seg.urgent ? MSG_OOB : 0);
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
    while (!out_.empty() && !finished_ && sock_ != INVALID_SOCKET) {
        OutSegment& seg = out_.front();
        if (seg.eof) {
            shutdown(sock_, SD_SEND);
            eofSent_ = true;
            out_.clear();
            return;
        }
        if (seg.data.empty()) {
            out_.pop_front();
            continue;
        }
        const int chunk = static_cast<int>(std::min<size_t>(seg.data.size(), 1 << 20));
        const int n = send(sock_, seg.data.data(), chunk, seg.urgent ? MSG_OOB : 0);
        if (n == SOCKET_ERROR) {
            const int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) return;  // resumed on FD_WRITE
            finish(errorClose("Network error: " + winErrorMessage(static_cast<unsigned long>(err))));
            return;
        }
        lastSend_ = Clock::now();
        seg.data.erase(0, static_cast<size_t>(n));
        if (seg.data.empty()) out_.pop_front();
    }
}

bool SocketTransport::readAvailable() {
    char buf[65536];
    size_t total = 0;
    while (!finished_ && total < kMaxReadPerIteration) {
        const int n = recv(sock_, buf, sizeof(buf), 0);
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
        const int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return false;
        finish(errorClose("Network error: " + winErrorMessage(static_cast<unsigned long>(err))));
        return false;
    }
    return !finished_;
}

void SocketTransport::readUrgent() {
    for (int i = 0; i < 64 && !finished_; ++i) {
        char b = 0;
        const int n = recv(sock_, &b, 1, MSG_OOB);
        if (n != 1) break;
        onUrgent(static_cast<unsigned char>(b));
    }
}

}  // namespace sc
