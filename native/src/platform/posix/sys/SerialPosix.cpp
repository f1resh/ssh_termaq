// Serial ports on POSIX (platform/sys/Serial.h): the tty opened exclusively (as Windows opens COM ports) in raw mode, and an I/O loop on select(2) - macOS' poll(2) does not work on devices - with a pipe for wake-ups. Port
// enumeration and non-standard speeds are per OS (macOS: mac/sys/SerialMac.cpp).
//
// _DARWIN_UNLIMITED_SELECT: select(2) takes descriptors above FD_SETSIZE (1024) in sets we size ourselves; NetInit
// raises the descriptor limit beyond that. It must come before any system header.
#define _DARWIN_UNLIMITED_SELECT 1

#include "platform/sys/Serial.h"

#include "platform/detail/PosixSerial.h"
#include "platform/sys/Errors.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace st::sys::serial {

namespace {

/** A descriptor set for select(2) as large as its highest descriptor needs. */
class FdSet {
public:
    void add(int fd) {
        const size_t word = static_cast<size_t>(fd) / NFDBITS;
        if (bits_.size() <= word) bits_.resize(std::max(word + 1, sizeof(fd_set) / sizeof(fd_mask)), 0);
        bits_[word] |= bit(fd);
        maxFd_ = std::max(maxFd_, fd);
    }
    bool has(int fd) const {
        const size_t word = static_cast<size_t>(fd) / NFDBITS;
        return word < bits_.size() && (bits_[word] & bit(fd));
    }
    fd_set* get() { return bits_.empty() ? nullptr : reinterpret_cast<fd_set*>(bits_.data()); }
    int count() const { return maxFd_ + 1; }

private:
    static fd_mask bit(int fd) {
        return static_cast<fd_mask>(std::make_unsigned_t<fd_mask>{1} << (static_cast<unsigned>(fd) % NFDBITS));
    }

    std::vector<fd_mask> bits_;
    int maxFd_ = -1;
};

/** Waits for `readA` / `readB` to be readable or `writeFd` writable (-1 = not watched); timeoutMs < 0 = no limit.
 *  The ready ones stay set; false on error (errno). */
bool waitFds(int readA, int readB, int writeFd, int timeoutMs, bool& aReady, bool& bReady, bool& writable) {
    FdSet reads, writes;
    for (int fd : {readA, readB})
        if (fd >= 0) reads.add(fd);
    if (writeFd >= 0) writes.add(writeFd);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    int n;
    do {
        n = ::select(std::max(reads.count(), writes.count()), reads.get(), writes.get(), nullptr,
                     timeoutMs < 0 ? nullptr : &tv);
    } while (n < 0 && errno == EINTR);
    if (n < 0) return false;
    aReady = readA >= 0 && reads.has(readA);
    bReady = readB >= 0 && reads.has(readB);
    writable = writeFd >= 0 && writes.has(writeFd);
    return true;
}

/** The B* constant of a standard speed (0 for others, which need detail::setCustomSpeed). */
speed_t standardSpeed(int baud) {
    static constexpr std::pair<int, speed_t> kSpeeds[] = {
        {50, B50},       {75, B75},       {110, B110},     {134, B134},       {150, B150},       {200, B200},
        {300, B300},     {600, B600},     {1200, B1200},   {1800, B1800},     {2400, B2400},     {4800, B4800},
        {9600, B9600},   {19200, B19200}, {38400, B38400}, {57600, B57600}, {115200, B115200}, {230400, B230400},
    };
    for (const auto& [rate, constant] : kSpeeds)
        if (rate == baud) return constant;
    return 0;
}

int dataBitsFlag(int bits) {
    switch (bits) {
        case 5: return CS5;
        case 6: return CS6;
        case 7: return CS7;
        default: return CS8;
    }
}

void makeNonBlockingPipe(int& readEnd, int& writeEnd) {
    int p[2];
    if (::pipe(p) != 0) return;
    for (int fd : p) {
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
    readEnd = p[0];
    writeEnd = p[1];
}

}  // namespace

struct Port::Impl {
    int fd = -1;
    int wakeRead = -1, wakeWrite = -1;  // a byte in the pipe: write / break / close requests
    std::string line;
    int baudRate = 0;
    int dataBits = 0;
    std::string note;  // said in the event log when run() starts

    ~Impl() {
        closePort();
        for (int f : {wakeRead, wakeWrite})
            if (f >= 0) ::close(f);
    }

    /** Closes the device as soon as the session is done with it, so it can be opened again right away. */
    void closePort() {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }

    void drainWake() {
        char buf[64];
        while (::read(wakeRead, buf, sizeof buf) > 0) {
        }
    }

    /** Writes everything, waiting while the device's buffer is full (or flow control holds it) - but, like the Windows
     *  write timeout, at most 5 s, after which the rest is dropped. False on a device error. */
    bool writeAll(std::string_view data, std::string& error) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!data.empty()) {
            const ssize_t n = ::write(fd, data.data(), data.size());
            if (n > 0) {
                data.remove_prefix(static_cast<size_t>(n));
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && errno != EAGAIN) {
                error = "Error writing to serial device: " + lastErrorMessage();
                return false;
            }
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) return true;
            bool unusedA = false, unusedB = false, writable = false;
            if (!waitFds(-1, -1, fd, static_cast<int>(left), unusedA, unusedB, writable)) {
                error = "Error writing to serial device: " + lastErrorMessage();
                return false;
            }
        }
        return true;
    }
};

Port::Port() : impl_(std::make_unique<Impl>()) { makeNonBlockingPipe(impl_->wakeRead, impl_->wakeWrite); }

Port::~Port() = default;

void Port::wake() {
    const char b = 1;
    if (impl_->wakeWrite >= 0) (void)!::write(impl_->wakeWrite, &b, 1);  // a full pipe is woken already
}

int Port::baudRate() const { return impl_->baudRate; }

int Port::dataBits() const { return impl_->dataBits; }

bool Port::open(const Settings& o, std::string& error) {
    const std::string& line = o.line;
    impl_->line = line;
    impl_->note.clear();
    const std::string path = line.find('/') == std::string::npos ? "/dev/" + line : line;  // "cu.usbserial-1410"
    // Non-blocking: opening a modem line must not wait for its carrier, and the I/O loop never blocks in read().
    impl_->fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    const int fd = impl_->fd;
    auto busy = [&](int err) {
        std::string why = errorMessage(static_cast<unsigned long>(err));
        if (err == EBUSY || err == EWOULDBLOCK) why = "the port is in use by another program";
        error = "Unable to open serial port " + line + ": " + why;
        impl_->closePort();
        return false;
    };
    if (fd < 0) return busy(errno);
    // Ours alone while it is open, as on Windows: the lock keeps out our other sessions and programs that lock too
    // (pyserial's exclusive mode); TIOCEXCL makes the device driver refuse any further open (not pseudo-terminals).
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) return busy(errno);
    ::ioctl(fd, TIOCEXCL);

    termios t{};
    if (::tcgetattr(fd, &t) != 0) {
        error = "Unable to read serial port settings: " + lastErrorMessage();
        impl_->closePort();
        return false;
    }
    auto fail = [&](const std::string& why) {
        error = "Unable to configure serial port " + line + ": " + why;
        impl_->closePort();
        return false;
    };
    ::cfmakeraw(&t);
    const unsigned long dsrDtr = detail::dsrDtrFlowFlags();
    t.c_cflag &= ~static_cast<tcflag_t>(CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS | dsrDtr);
    t.c_cflag |= static_cast<tcflag_t>(CLOCAL | CREAD | dataBitsFlag(std::clamp(o.dataBits, 5, 8)));
    t.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | IXANY | INPCK);
    switch (o.parity) {
        case Parity::Odd: t.c_cflag |= PARENB | PARODD; break;
        case Parity::Even: t.c_cflag |= PARENB; break;
        case Parity::Mark:
        case Parity::Space: return fail("mark and space parity are not available on this system");
        default: break;
    }
    // No 1.5 stop bits here: 2 is the closest a receiver expecting 1.5 accepts.
    if (o.stopBits > 1) t.c_cflag |= CSTOPB;
    if (o.stopBits > 1 && o.stopBits < 2) impl_->note = "1.5 stop bits are not available on this system; using 2";
    switch (o.flowControl) {
        case FlowControl::XonXoff:
            t.c_iflag |= IXON | IXOFF;
            t.c_cc[VSTART] = 0x11;
            t.c_cc[VSTOP] = 0x13;
            break;
        case FlowControl::RtsCts: t.c_cflag |= CRTSCTS; break;
        case FlowControl::DsrDtr:
            if (!dsrDtr) return fail("DSR/DTR flow control is not available on this system");
            t.c_cflag |= static_cast<tcflag_t>(dsrDtr);
            break;
        default: break;
    }
    const int baud = std::max(1, o.baudRate);
    const speed_t standard = standardSpeed(baud);
    ::cfsetspeed(&t, standard ? standard : B9600);
    if (::tcsetattr(fd, TCSANOW, &t) != 0) return fail(lastErrorMessage());
    if (!standard && !detail::setCustomSpeed(fd, baud))
        return fail("cannot set " + std::to_string(baud) + " baud: " + lastErrorMessage());
    // DTR and RTS on, as on Windows (a flow control that drives them takes over).
    int lines = TIOCM_DTR | TIOCM_RTS;
    ::ioctl(fd, TIOCMBIS, &lines);
    ::tcflush(fd, TCIOFLUSH);
    impl_->baudRate = baud;
    impl_->dataBits = std::clamp(o.dataBits, 5, 8);
    return true;
}

std::string Port::run(const PortIo& io) {
    const int fd = impl_->fd;
    if (!impl_->note.empty()) io.log(impl_->note);
    char buf[4096];
    std::string error;

    while (!io.stopRequested()) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n > 0) {
            io.received(std::string_view(buf, static_cast<size_t>(n)));
        } else if (n == 0) {
            error = "Error reading from serial device: the device was disconnected";
            break;
        } else if (errno != EAGAIN && errno != EINTR) {
            error = "Error reading from serial device: " + lastErrorMessage();
            break;
        }

        // Pending writes / break.
        const std::string out = io.takeOutput();
        if (!out.empty() && !impl_->writeAll(out, error)) break;
        if (io.takeBreak()) {
            ::ioctl(fd, TIOCSBRK);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            ::ioctl(fd, TIOCCBRK);
            io.log("Sent break");
        }
        if (n > 0) continue;  // there may be more to read before waiting

        bool readable = false, woken = false, unused = false;
        if (!waitFds(fd, impl_->wakeRead, -1, -1, readable, woken, unused)) {
            error = "Error reading from serial device: " + lastErrorMessage();
            break;
        }
        if (woken) impl_->drainWake();
    }
    impl_->closePort();
    return error;
}

}  // namespace st::sys::serial
