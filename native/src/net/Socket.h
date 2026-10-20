#pragma once
// Sockets for the network transports: the platform socket API (platform/sys/Net.h) plus the mapping from session
// settings to it.
#include "core/Types.h"
#include "platform/sys/Net.h"

namespace st {

/** The session's address family preference (Connection > IP version) for the socket layer. */
inline sys::IpFamily ipFamily(AddressFamily f) {
    switch (f) {
        case AddressFamily::IPv4: return sys::IpFamily::IPv4;
        case AddressFamily::IPv6: return sys::IpFamily::IPv6;
        default: return sys::IpFamily::Auto;
    }
}

}  // namespace st
