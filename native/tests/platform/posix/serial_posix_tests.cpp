// platform/sys/Serial.h on POSIX, against a pseudo-terminal standing in for the device: data both ways, wake-ups,
// stop, exclusive use, a vanished device and settings the system lacks.
#include "platform/sys/Serial.h"

#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

using namespace st;
using namespace std::chrono_literals;

namespace {

/** A pseudo-terminal: the test holds the master side, the port under test opens the slave by path. */
struct Pty {
    int master = -1;
    std::string slavePath;
    Pty() {
        master = ::posix_openpt(O_RDWR | O_NOCTTY);
        REQUIRE(master >= 0);
        REQUIRE(::grantpt(master) == 0);
        REQUIRE(::unlockpt(master) == 0);
        slavePath = ::ptsname(master);
    }
    ~Pty() { closeMaster(); }
    void closeMaster() {
        if (master >= 0) ::close(master);
        master = -1;
    }
    /** What the port sent, waiting up to 2 s for `n` bytes. */
    std::string read(size_t n) {
        std::string got;
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (got.size() < n && std::chrono::steady_clock::now() < deadline) {
            pollfd p{master, POLLIN, 0};
            if (::poll(&p, 1, 100) <= 0) continue;
            char buf[4096];
            const ssize_t r = ::read(master, buf, sizeof buf);
            if (r > 0) got.append(buf, static_cast<size_t>(r));
        }
        return got;
    }
};

/** Runs Port::run on a thread, collecting what it receives. */
struct Session {
    sys::serial::Port& port;
    std::mutex m;
    std::condition_variable cv;
    std::string received, output, log;
    bool stop = false, breakNow = false;
    std::string result = "(running)";
    std::thread thread;

    explicit Session(sys::serial::Port& p) : port(p) {
        thread = std::thread([this] {
            sys::serial::PortIo io;
            io.received = [this](std::string_view b) {
                std::lock_guard l(m);
                received.append(b);
                cv.notify_all();
            };
            io.takeOutput = [this] {
                std::lock_guard l(m);
                return std::exchange(output, {});
            };
            io.takeBreak = [this] {
                std::lock_guard l(m);
                return std::exchange(breakNow, false);
            };
            io.stopRequested = [this] {
                std::lock_guard l(m);
                return stop;
            };
            io.log = [this](const std::string& line) {
                std::lock_guard l(m);
                log += line + "\n";
            };
            std::string r = port.run(io);
            std::lock_guard l(m);
            result = std::move(r);
            cv.notify_all();
        });
    }
    ~Session() { finish(); }
    bool waitReceived(const std::string& what) {
        std::unique_lock l(m);
        return cv.wait_for(l, 2s, [&] { return received.find(what) != std::string::npos; });
    }
    void send(const std::string& data) {
        {
            std::lock_guard l(m);
            output += data;
        }
        port.wake();
    }
    /** Waits up to 2 s for run() to end by itself. */
    bool waitEnded() {
        std::unique_lock l(m);
        return cv.wait_for(l, 2s, [&] { return result != "(running)"; });
    }
    std::string finish() {
        {
            std::lock_guard l(m);
            stop = true;
        }
        port.wake();
        if (thread.joinable()) thread.join();
        return result;
    }
};

sys::serial::Settings settingsFor(const Pty& pty) {
    sys::serial::Settings s;
    s.line = pty.slavePath;
    s.baudRate = 115200;
    s.flowControl = sys::serial::FlowControl::None;
    return s;
}

}  // namespace

TEST_CASE("serial (POSIX): data both ways, wake-ups and stop", "[platform][serial]") {
    Pty pty;
    sys::serial::Port port;
    std::string error;
    REQUIRE(port.open(settingsFor(pty), error));
    CHECK(port.baudRate() == 115200);
    CHECK(port.dataBits() == 8);
    {
        Session session(port);
        REQUIRE(::write(pty.master, "hello\r\n", 7) == 7);
        REQUIRE(session.waitReceived("hello\r\n"));  // raw: nothing translated
        session.send("wor");
        session.send("ld\n");
        REQUIRE(pty.read(6) == "world\n");
        session.send(std::string(100000, 'x'));  // more than the tty buffers at once
        REQUIRE(pty.read(100000).size() == 100000);
        REQUIRE(::write(pty.master, "more", 4) == 4);
        REQUIRE(session.waitReceived("more"));
        CHECK(session.finish().empty());
    }
    // The device is closed as soon as run() ends: it opens again right away.
    sys::serial::Port again;
    CHECK(again.open(settingsFor(pty), error));
}

TEST_CASE("serial (POSIX): one user at a time", "[platform][serial]") {
    Pty pty;
    sys::serial::Port first, second;
    std::string error;
    REQUIRE(first.open(settingsFor(pty), error));
    REQUIRE_FALSE(second.open(settingsFor(pty), error));
    CHECK(error == "Unable to open serial port " + pty.slavePath + ": the port is in use by another program");
}

TEST_CASE("serial (POSIX): a vanished device ends the session with an error", "[platform][serial]") {
    Pty pty;
    sys::serial::Port port;
    std::string error;
    REQUIRE(port.open(settingsFor(pty), error));
    Session session(port);
    pty.closeMaster();
    REQUIRE(session.waitEnded());
    CHECK(session.finish().rfind("Error reading from serial device: ", 0) == 0);
}

TEST_CASE("serial (POSIX): missing devices and settings the system lacks", "[platform][serial]") {
    std::string error;
    sys::serial::Port port;
    sys::serial::Settings s;
    s.line = "cu.st-test-no-such-port";
    REQUIRE_FALSE(port.open(s, error));
    CHECK(error == "Unable to open serial port cu.st-test-no-such-port: No such file or directory");

    Pty pty;
    s = settingsFor(pty);
    s.parity = sys::serial::Parity::Mark;
    REQUIRE_FALSE(port.open(s, error));
    CHECK(error == "Unable to configure serial port " + pty.slavePath +
                       ": mark and space parity are not available on this system");
    // ...and the port was released again.
    s.parity = sys::serial::Parity::Even;
    s.stopBits = 1.5;
    REQUIRE(port.open(s, error));
    Session session(port);
    session.send("x");
    REQUIRE(pty.read(1) == "x");
    CHECK(session.finish().empty());
    CHECK(session.log == "1.5 stop bits are not available on this system; using 2\n");
}
