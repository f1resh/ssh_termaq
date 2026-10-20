#pragma once
// Private to platform/posix and the OS folders built with it: what the OS adds to a failed connect's message.
#include <string>

struct sockaddr;

namespace st::sys::detail {

/** A sentence saying how the OS itself may have stopped a connect to `addr` that failed with `err` ("" for none).
 *  The OS's folder implements it (macOS: Local Network privacy, mac/sys/NetHintsMac.cpp). */
std::string connectFailureHint(int err, const sockaddr* addr);

}  // namespace st::sys::detail
