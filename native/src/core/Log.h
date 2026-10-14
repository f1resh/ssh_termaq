#pragma once
// Application diagnostics log (not the per-connection event log / session log).
// Writes to OutputDebugString and <dataDir>/logs/sshterm.log once initialized.
#include <filesystem>
#include <string>

namespace st::applog {

void init(const std::filesystem::path& file);
void info(const std::string& msg);
void warn(const std::string& msg);
void error(const std::string& msg);
void debug(const std::string& msg);

}  // namespace st::applog
