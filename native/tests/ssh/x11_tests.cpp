// X11 forwarding's plain logic (ssh/X11Forward): display names, Xauthority files and the cookie gate every forwarded
// X connection passes. The end-to-end test through a session is in ssh_tests.cpp.
#include "ssh/X11Forward.h"

#include <catch2/catch_test_macros.hpp>

using namespace st::ssh;

namespace {

std::string be16(size_t v) { return std::string{static_cast<char>((v >> 8) & 0xff), static_cast<char>(v & 0xff)}; }
std::string counted(const std::string& s) { return be16(s.size()) + s; }
std::string entry(uint16_t family, const std::string& address, const std::string& number, const std::string& name,
                  const std::string& data) {
    return be16(family) + counted(address) + counted(number) + counted(name) + counted(data);
}

/** An X11 connection setup as a client sends it. */
std::string setup(bool msb, const std::string& name, const std::string& data, const std::string& after = "") {
    auto u16 = [&](size_t v) {
        const char hi = static_cast<char>((v >> 8) & 0xff), lo = static_cast<char>(v & 0xff);
        return msb ? std::string{hi, lo} : std::string{lo, hi};
    };
    auto padded = [](std::string s) { return s.append((4 - s.size() % 4) % 4, '\0'); };
    return std::string(1, msb ? 'B' : 'l') + std::string(1, '\0') + u16(11) + u16(0) + u16(name.size()) + u16(data.size()) +
           std::string(2, '\0') + padded(name) + padded(data) + after;
}

const std::string kFake = "0123456789abcdef";
const std::string kReal = "REALREALREALREAL";

}  // namespace

TEST_CASE("x11: display names", "[ssh][x11]") {
    X11Display d;
    std::string error;
    REQUIRE(parseX11Display(":0", d, error));
    CHECK(d.unixPath == "/tmp/.X11-unix/X0");
    CHECK(d.host == "localhost");
    CHECK(d.port == 6000);
    CHECK(d.local);

    REQUIRE(parseX11Display("unix:3.2", d, error));
    CHECK(d.unixPath == "/tmp/.X11-unix/X3");
    CHECK(d.display == 3);
    CHECK(d.screen == 2);

    REQUIRE(parseX11Display("localhost:10", d, error));
    CHECK(d.unixPath == "/tmp/.X11-unix/X10");  // this machine: its socket first, then TCP
    CHECK(d.host == "localhost");
    CHECK(d.port == 6010);

    REQUIRE(parseX11Display("tcp/localhost:1", d, error));
    CHECK(d.unixPath.empty());
    CHECK(d.host == "localhost");
    CHECK(d.local);

    REQUIRE(parseX11Display("xhost.example:5", d, error));
    CHECK(d.unixPath.empty());
    CHECK(d.host == "xhost.example");
    CHECK(d.port == 6005);
    CHECK_FALSE(d.local);

    REQUIRE(parseX11Display("[::1]:0", d, error));
    CHECK(d.host == "::1");
    CHECK(d.local);

    // macOS XQuartz: DISPLAY is its launchd socket.
    REQUIRE(parseX11Display("/private/tmp/com.apple.launchd.AbC/org.xquartz:0", d, error));
    CHECK(d.unixPath == "/private/tmp/com.apple.launchd.AbC/org.xquartz:0");
    CHECK(d.host.empty());
    CHECK(d.display == 0);
    CHECK(d.local);
    REQUIRE(parseX11Display("/tmp/x:1.1", d, error));
    CHECK(d.unixPath == "/tmp/x:1");
    CHECK(d.screen == 1);

    for (const char* bad : {"localhost", ":x", ":0.", "/tmp/socket", "host:99999", "tcp/:0", ""}) {
        INFO(bad);
        CHECK_FALSE(parseX11Display(bad, d, error));
    }
}

TEST_CASE("x11: the real cookie from an Xauthority file", "[ssh][x11]") {
    const std::string a(16, 'a'), b(16, 'b'), c(16, 'c'), w(16, 'w'), i(16, 'i');
    const std::string file = entry(256, "other-mac", "0", "MIT-MAGIC-COOKIE-1", b) +
                             entry(256, "MyMac.local", "0", "MIT-MAGIC-COOKIE-1", a) +
                             entry(256, "MyMac.local", "1", "MIT-MAGIC-COOKIE-1", c) +
                             entry(256, "MyMac.local", "2", "XDM-AUTHORIZATION-1", std::string(16, 'x')) +
                             entry(0, std::string("\x0a\x00\x00\x05", 4), "5", "MIT-MAGIC-COOKIE-1", i) +
                             entry(65535, "", "7", "MIT-MAGIC-COOKIE-1", w) + "\x01";  // a cut-off entry is ignored
    const auto entries = parseXauthority(file);
    REQUIRE(entries.size() == 6);
    CHECK(entries[1].address == "MyMac.local");

    X11Display local;
    std::string error;
    REQUIRE(parseX11Display(":0", local, error));
    CHECK(findX11Cookie(entries, local, "mymac", "") == a);  // our host name, without the domain, any case
    CHECK(findX11Cookie(entries, local, "unknown", "") == b);  // else another local entry for the display
    REQUIRE(parseX11Display(":1", local, error));
    CHECK(findX11Cookie(entries, local, "MyMac.local", "") == c);
    REQUIRE(parseX11Display(":2", local, error));
    CHECK_FALSE(findX11Cookie(entries, local, "MyMac.local", ""));  // only MIT-MAGIC-COOKIE-1
    REQUIRE(parseX11Display(":7", local, error));
    CHECK(findX11Cookie(entries, local, "MyMac.local", "") == w);  // an entry for any address

    X11Display remote;
    REQUIRE(parseX11Display("10.0.0.5:5", remote, error));
    CHECK(findX11Cookie(entries, remote, "MyMac.local", std::string("\x0a\x00\x00\x05", 4)) == i);
    CHECK_FALSE(findX11Cookie(entries, remote, "MyMac.local", std::string("\x0a\x00\x00\x06", 4)));
    CHECK(parseXauthority("").empty());
}

TEST_CASE("x11: the cookie gate", "[ssh][x11]") {
    SECTION("the fake cookie lets the client in, with the real one in its place") {
        for (bool msb : {false, true}) {
            X11AuthGate gate(kFake, kReal);
            const std::string in = setup(msb, "MIT-MAGIC-COOKIE-1", kFake, "first request");
            std::string toServer, toClient;
            // Byte by byte: the setup may come in pieces.
            for (size_t k = 0; k + 1 < in.size() - 13; ++k)
                REQUIRE(gate.feed(in.data() + k, 1, toServer, toClient) == X11AuthGate::State::Waiting);
            const size_t done = in.size() - 14;
            REQUIRE(gate.feed(in.data() + done, in.size() - done, toServer, toClient) == X11AuthGate::State::Open);
            CHECK(toServer == setup(msb, "MIT-MAGIC-COOKIE-1", kReal, "first request"));
            CHECK(toClient.empty());
        }
    }
    SECTION("without a real cookie the X server gets none") {
        X11AuthGate gate(kFake, "");
        std::string toServer, toClient;
        const std::string in = setup(false, "MIT-MAGIC-COOKIE-1", kFake, "x");
        REQUIRE(gate.feed(in.data(), in.size(), toServer, toClient) == X11AuthGate::State::Open);
        CHECK(toServer == setup(false, "", "", "x"));
    }
    SECTION("anything else is refused with the X11 failed reply") {
        struct Case {
            std::string name, data, reason;
        };
        for (const auto& c : {Case{"MIT-MAGIC-COOKIE-1", std::string(16, 'A'), "wrong authorization"},
                              Case{"", "", "no authorization"},
                              Case{"XDM-AUTHORIZATION-1", kFake, "wrong authorization"},
                              Case{"MIT-MAGIC-COOKIE-1", kFake.substr(0, 15), "wrong authorization"}}) {
            X11AuthGate gate(kFake, kReal);
            std::string toServer, toClient;
            const std::string in = setup(true, c.name, c.data);
            REQUIRE(gate.feed(in.data(), in.size(), toServer, toClient) == X11AuthGate::State::Refused);
            CHECK(gate.reason() == c.reason);
            CHECK(toServer.empty());
            // Failed (0), reason length, version 11.0 in the client's byte order, length in 4-byte units, the reason.
            REQUIRE(toClient.size() >= 8);
            CHECK(toClient[0] == '\0');
            const size_t n = static_cast<unsigned char>(toClient[1]);
            CHECK(toClient.substr(2, 4) == std::string("\0\x0b\0\0", 4));
            CHECK(static_cast<unsigned char>(toClient[7]) * 4 == toClient.size() - 8);
            CHECK(toClient.substr(8, n).find(c.reason) != std::string::npos);
            // Whatever follows is ignored.
            CHECK(gate.feed("more", 4, toServer, toClient) == X11AuthGate::State::Refused);
            CHECK(toServer.empty());
        }
    }
    SECTION("not an X11 setup") {
        X11AuthGate gate(kFake, kReal);
        std::string toServer, toClient;
        CHECK(gate.feed("GET / HTTP/1.1\r\n", 16, toServer, toClient) == X11AuthGate::State::Refused);
    }
}
