#include "core/Uuid.h"

#include "core/Crypto.h"

#include <cstdio>

namespace sc {

std::string newUuid() {
    std::string b = crypto::randomBytes(16);
    b[6] = static_cast<char>((b[6] & 0x0F) | 0x40);
    b[8] = static_cast<char>((b[8] & 0x3F) | 0x80);
    char out[37];
    auto u = [&](int i) { return static_cast<unsigned>(static_cast<unsigned char>(b[i])); };
    std::snprintf(out, sizeof out, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u(0), u(1), u(2),
                  u(3), u(4), u(5), u(6), u(7), u(8), u(9), u(10), u(11), u(12), u(13), u(14), u(15));
    return out;
}

}  // namespace sc
