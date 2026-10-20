// macOS 15 asks the user before an app may reach devices on the local network (Privacy & Security > Local Network);
// while it is not allowed, connects to such addresses fail with EHOSTUNREACH ("No route to host").
#include "platform/posix/sys/NetHints.h"

#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstdint>

namespace st::sys::detail {

namespace {

/** Private, link-local and unique-local addresses: what Local Network privacy covers (not loopback). */
bool isLocalNetwork(const sockaddr* addr) {
    if (addr->sa_family == AF_INET) {
        const auto* a = reinterpret_cast<const sockaddr_in*>(addr);
        const uint32_t ip = ntohl(a->sin_addr.s_addr);
        return (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 16) == 0xA9FE;
    }
    if (addr->sa_family == AF_INET6) {
        const auto* a = reinterpret_cast<const sockaddr_in6*>(addr);
        const uint8_t b0 = a->sin6_addr.s6_addr[0], b1 = a->sin6_addr.s6_addr[1];
        return (b0 == 0xfe && (b1 & 0xc0) == 0x80) || (b0 & 0xfe) == 0xfc;  // fe80::/10, fc00::/7
    }
    return false;
}

}  // namespace

std::string connectFailureHint(int err, const sockaddr* addr) {
    if (!addr || (err != EHOSTUNREACH && err != ENETUNREACH && err != EPERM) || !isLocalNetwork(addr)) return {};
    return "macOS may be blocking the local network: allow SSHTermaq in System Settings > Privacy & Security > Local Network";
}

}  // namespace st::sys::detail
