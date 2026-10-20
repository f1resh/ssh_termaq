#pragma once
// Diagnostics for developers: the debugger's output window on Windows (OutputDebugString), the unified system log on
// macOS (os_log; Console.app, `log stream --process SSHTermaq`), and thread names.
#include <string_view>

namespace st::sys {

/** Writes one line of UTF-8 text (a trailing newline is optional). Thread-safe. */
void debugOutput(std::string_view line);
/** Names the calling thread for debuggers and crash reports ("st-telnet"). */
void setThreadName(std::string_view name);

}  // namespace st::sys
