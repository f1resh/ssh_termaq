// macOS: a failed connect to the local network explains Local Network privacy (platform/posix/sys/NetHints.h).
#include "platform/posix/sys/NetHints.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <catch2/catch_test_macros.hpp>

#include <cerrno>

using namespace st::sys;

namespace {

std::string hint4(const char* ip, int err) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &a.sin_addr);
    return detail::connectFailureHint(err, reinterpret_cast<const sockaddr*>(&a));
}

std::string hint6(const char* ip, int err) {
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    inet_pton(AF_INET6, ip, &a.sin6_addr);
    return detail::connectFailureHint(err, reinterpret_cast<const sockaddr*>(&a));
}

}  // namespace

TEST_CASE("net (macOS): an unreachable local network address points at Local Network privacy", "[platform][net]") {
    for (const char* ip : {"192.168.1.10", "10.0.0.1", "172.16.5.4", "172.31.255.255", "169.254.1.1"}) {
        INFO(ip);
        CHECK(hint4(ip, EHOSTUNREACH).find("Local Network") != std::string::npos);
    }
    CHECK_FALSE(hint4("192.168.1.10", ENETUNREACH).empty());
    for (const char* ip : {"8.8.8.8", "127.0.0.1", "172.32.0.1", "11.0.0.1"}) {
        INFO(ip);
        CHECK(hint4(ip, EHOSTUNREACH).empty());
    }
    CHECK(hint4("192.168.1.10", ECONNREFUSED).empty());  // reached: something answered
    CHECK(hint4("192.168.1.10", ETIMEDOUT).empty());
    CHECK_FALSE(hint6("fe80::1", EHOSTUNREACH).empty());
    CHECK_FALSE(hint6("fd12:3456::1", EHOSTUNREACH).empty());
    CHECK(hint6("::1", EHOSTUNREACH).empty());
    CHECK(hint6("2001:db8::1", EHOSTUNREACH).empty());
    CHECK(detail::connectFailureHint(EHOSTUNREACH, nullptr).empty());
}
