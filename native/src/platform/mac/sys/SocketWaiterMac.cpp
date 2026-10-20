// SocketWaiter on macOS (platform/sys/Net.h): a kqueue per waiter. poll(2) cannot be used: macOS reports no event at
// all for TCP urgent data there, while kqueue's EVFILT_EXCEPT/NOTE_OOB does (and goes quiet once the byte is read).
// Events are reported the way Windows' WSAEventSelect does: readable while unread data remains (EVFILT_READ, level
// triggered), writable while output is queued (EVFILT_WRITE, enabled by wantWrite), urgent data when it is not inline,
// closed once (EV_EOF). Wake-ups are an EVFILT_USER event, kept until the next wait.
#include "platform/sys/Net.h"

#include "platform/sys/Errors.h"

#include <fcntl.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <stdexcept>

namespace st::sys {

namespace {

constexpr uintptr_t kWakeIdent = 1;

bool change(int kq, uintptr_t ident, int16_t filter, uint16_t flags, uint32_t fflags = 0) {
    struct kevent ev;
    EV_SET(&ev, ident, filter, flags, fflags, 0, nullptr);
    return ::kevent(kq, &ev, 1, nullptr, 0, nullptr) == 0;
}

}  // namespace

struct SocketWaiter::Impl {
    int kq = -1;
    int sock = -1;
    bool urgentInline = false;
    bool writeOn = false;
    bool closedReported = false;
};

SocketWaiter::SocketWaiter() : impl_(std::make_unique<Impl>()) {
    impl_->kq = ::kqueue();
    if (impl_->kq < 0) throw std::runtime_error("kqueue: " + lastErrorMessage());
    ::fcntl(impl_->kq, F_SETFD, FD_CLOEXEC);
    if (!change(impl_->kq, kWakeIdent, EVFILT_USER, EV_ADD | EV_CLEAR)) {
        const std::string error = lastErrorMessage();
        ::close(impl_->kq);
        throw std::runtime_error("kevent: " + error);
    }
}

SocketWaiter::~SocketWaiter() {
    detach();
    ::close(impl_->kq);
}

void SocketWaiter::wake() { change(impl_->kq, kWakeIdent, EVFILT_USER, 0, NOTE_TRIGGER); }

bool SocketWaiter::attach(SocketHandle s, bool urgentInline, std::string& error) {
    const int fd = static_cast<int>(s);
    impl_->sock = fd;
    impl_->urgentInline = urgentInline;
    impl_->writeOn = false;
    impl_->closedReported = false;
    const int inlineOob = urgentInline ? 1 : 0;
    setsockopt(fd, SOL_SOCKET, SO_OOBINLINE, &inlineOob, sizeof inlineOob);
    // As WSAEventSelect does: the socket becomes non-blocking.
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0 || !change(impl_->kq, fd, EVFILT_READ, EV_ADD) ||
        !change(impl_->kq, fd, EVFILT_WRITE, EV_ADD | EV_DISABLE) ||
        (!urgentInline && !change(impl_->kq, fd, EVFILT_EXCEPT, EV_ADD, NOTE_OOB))) {
        error = lastSocketErrorMessage();
        return false;
    }
    return true;
}

void SocketWaiter::detach() {
    if (impl_->sock < 0) return;
    // Registrations of a closed descriptor are gone already; errors here are harmless.
    change(impl_->kq, impl_->sock, EVFILT_READ, EV_DELETE);
    change(impl_->kq, impl_->sock, EVFILT_WRITE, EV_DELETE);
    if (!impl_->urgentInline) change(impl_->kq, impl_->sock, EVFILT_EXCEPT, EV_DELETE);
    impl_->sock = -1;
}

void SocketWaiter::wantWrite(bool on) {
    if (on == impl_->writeOn || impl_->sock < 0) return;
    if (change(impl_->kq, impl_->sock, EVFILT_WRITE, on ? EV_ENABLE : EV_DISABLE)) impl_->writeOn = on;
}

bool SocketWaiter::wait(int timeoutMs, Events& ev, std::string& error) {
    ev = Events{};
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    struct kevent out[8];
    int n;
    for (;;) {
        timespec ts{};
        timespec* tsp = nullptr;
        if (timeoutMs >= 0) {
            const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
            const long long left = std::max<long long>(0, timeoutMs - spent);
            ts.tv_sec = static_cast<time_t>(left / 1000);
            ts.tv_nsec = static_cast<long>((left % 1000) * 1000000);
            tsp = &ts;
        }
        n = ::kevent(impl_->kq, nullptr, 0, out, 8, tsp);
        if (n >= 0 || errno != EINTR) break;
    }
    if (n < 0) {
        error = lastSocketErrorMessage();
        return false;
    }
    if (n == 0) {
        ev.timedOut = true;
        return true;
    }
    for (int i = 0; i < n; ++i) {
        const struct kevent& e = out[i];
        if (e.filter == EVFILT_USER) {
            ev.woken = true;
        } else if (static_cast<int>(e.ident) != impl_->sock) {
            continue;  // left over from a socket detached in the meantime
        } else if (e.filter == EVFILT_READ) {
            ev.readable = true;
            if ((e.flags & EV_EOF) && !impl_->closedReported) {
                // Closed (once). The data that came before, and a reset's error, are left for recv() to report.
                ev.closed = true;
                impl_->closedReported = true;
                change(impl_->kq, impl_->sock, EVFILT_READ, EV_DISABLE);  // EV_EOF stays set: do not report it forever
            }
        } else if (e.filter == EVFILT_WRITE) {
            ev.writable = true;
        } else if (e.filter == EVFILT_EXCEPT && (e.fflags & NOTE_OOB)) {
            ev.urgent = true;
        }
    }
    return true;
}

}  // namespace st::sys
