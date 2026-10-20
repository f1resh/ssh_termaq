#include "platform/sys/Errors.h"

#include <cerrno>
#include <cstring>

namespace st::sys {

std::string errorMessage(unsigned long code) {
    char buf[256] = {};
    if (strerror_r(static_cast<int>(code), buf, sizeof buf) != 0 || buf[0] == '\0') return "Error " + std::to_string(code);
    return buf;
}

std::string lastErrorMessage() { return errorMessage(static_cast<unsigned long>(errno)); }

std::string lastSocketErrorMessage() { return errorMessage(static_cast<unsigned long>(errno)); }

}  // namespace st::sys
