// The parts of platform/sys/Net.h that are plain text handling, the same on every OS.
#include "platform/sys/Net.h"

namespace st::sys {

std::string stripBrackets(const std::string& host) {
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') return host.substr(1, host.size() - 2);
    return host;
}

std::string formatHostPort(const std::string& hostIn, int port) {
    const std::string host = stripBrackets(hostIn);
    if (host.find(':') != std::string::npos) return "[" + host + "]:" + std::to_string(port);
    return host + ":" + std::to_string(port);
}

}  // namespace st::sys
