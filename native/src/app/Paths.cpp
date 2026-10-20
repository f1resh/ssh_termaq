#include "app/Paths.h"

#include "platform/sys/Env.h"
#include "platform/sys/Fs.h"

#include <stdexcept>

namespace fs = std::filesystem;

namespace st {

fs::path exeDirectory() {
    const fs::path exe = sys::executablePath();
    return exe.empty() ? fs::current_path() : exe.parent_path();
}

AppPaths resolveAppPaths(const fs::path& exeDir, const fs::path& appData, const fs::path& overrideDataDir) {
    AppPaths p;
    p.exeDir = exeDir;
    std::error_code ec;
    if (!overrideDataDir.empty()) {
        p.dataDir = overrideDataDir.is_relative() ? fs::absolute(overrideDataDir, ec) : overrideDataDir;
        if (ec) p.dataDir = overrideDataDir;
        p.portable = false;
    } else if (fs::is_regular_file(exeDir / "portable.flag", ec) || fs::is_directory(exeDir / "data", ec)) {
        p.dataDir = exeDir / "data";
        p.portable = true;
    } else {
        if (appData.empty()) throw std::runtime_error("Cannot determine the user's application data folder");
        p.dataDir = appData / "SSHTermaq";
    }
    p.dataDir = p.dataDir.lexically_normal();
    p.logsDir = p.dataDir / "logs";
    p.webViewDataDir = p.dataDir / "WebView2";
    return p;
}

AppPaths initAppPaths(const fs::path& overrideDataDir) {
    fs::path override = overrideDataDir;
    if (override.empty()) {
        if (auto env = sys::getEnv("SSHTERMAQ_DATA_DIR")) override = sys::toPath(*env);
    }
    AppPaths p = resolveAppPaths(exeDirectory(), sys::userDataRoot(), override);
    std::error_code ec;
    fs::create_directories(p.dataDir, ec);
    if (ec) throw std::runtime_error("Cannot create the data directory: " + ec.message());
    fs::create_directories(p.logsDir, ec);
    fs::create_directories(p.webViewDataDir, ec);
    return p;
}

}  // namespace st
