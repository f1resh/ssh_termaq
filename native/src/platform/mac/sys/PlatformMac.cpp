#include "platform/Platform.h"

namespace st::sys {

const char* osName() { return "darwin"; }

const char* archName() {
#if defined(__aarch64__)
    return "arm64";
#else
    return "x64";
#endif
}

}  // namespace st::sys
