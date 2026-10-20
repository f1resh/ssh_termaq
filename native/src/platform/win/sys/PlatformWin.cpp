#include "platform/Platform.h"

namespace st::sys {

const char* osName() { return "win32"; }

const char* archName() {
#if defined(_M_ARM64)
    return "arm64";
#else
    return "x64";
#endif
}

}  // namespace st::sys
