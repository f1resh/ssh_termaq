#pragma once
// The process environment: executable location, per-user folders, environment variables, user name.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace st::sys {

/** Full path of the running executable; empty if it cannot be determined. */
std::filesystem::path executablePath();
/** When the running executable was built, in Unix seconds; 0 if unknown. Windows: the link time in the PE header;
 *  macOS (Mach-O has no link time): the executable file's modification time. */
std::int64_t executableBuildTime();
/** Per-user base folder for application data: %APPDATA% (Windows), ~/Library/Application Support (macOS); empty if
 *  unknown. */
std::filesystem::path userDataRoot();
/** The user's home folder (%USERPROFILE% | $HOME); empty if unknown. */
std::filesystem::path homeDirectory();
/** Environment variable as UTF-8; nullopt when it is not set. */
std::optional<std::string> getEnv(const std::string& name);
/** Login name of the user running the process; empty if unknown. */
std::string userName();

}  // namespace st::sys
