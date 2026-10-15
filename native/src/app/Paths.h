#pragma once
// Well-known application directories.
//   Portable mode : a "portable.flag" file or a "data" directory next to the executable => <exe dir>/data
//   Installed     : %APPDATA%\SSHTermaq
// An explicit override (CLI "--data-dir <path>" or the SSHTERMAQ_DATA_DIR environment variable) wins over both
// (used by tests / side-by-side profiles).
#include <filesystem>

namespace st {

struct AppPaths {
    std::filesystem::path exeDir;          // directory of SSHTermaq.exe
    std::filesystem::path dataDir;         // sessions.json, settings.json, ...
    std::filesystem::path logsDir;         // <dataDir>/logs (app log + default session log directory)
    std::filesystem::path webViewDataDir;  // <dataDir>/WebView2 (WebView2 user data folder)
    bool portable = false;
};

/** Directory containing the running executable. */
std::filesystem::path exeDirectory();

/** %APPDATA% (FOLDERID_RoamingAppData); empty if unavailable. */
std::filesystem::path roamingAppDataDirectory();

/**
 * Resolves the application paths (pure; does not touch the disk except for existence checks).
 * `overrideDataDir` (may be empty) takes precedence over portable/installed detection.
 */
AppPaths resolveAppPaths(const std::filesystem::path& exeDir, const std::filesystem::path& appData,
                         const std::filesystem::path& overrideDataDir = {});

/** resolveAppPaths() for the running process, honouring SSHTERMAQ_DATA_DIR; creates the data/log directories. */
AppPaths initAppPaths(const std::filesystem::path& overrideDataDir = {});

}  // namespace st
