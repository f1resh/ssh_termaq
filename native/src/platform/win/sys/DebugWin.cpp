#include "platform/sys/Debug.h"

#include "platform/detail/WinApi.h"
#include "platform/win/sys/WinText.h"

namespace st::sys {

void debugOutput(std::string_view line) { OutputDebugStringW(widen(line).c_str()); }

void setThreadName(std::string_view name) { SetThreadDescription(GetCurrentThread(), widen(name).c_str()); }

}  // namespace st::sys
