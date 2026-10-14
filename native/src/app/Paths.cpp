#include "app/Paths.h"

#include "core/Win.h"

#include <shlobj.h>

#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

namespace st {

fs::path exeDirectory() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return fs::current_path();
        if (n < buf.size()) return fs::path(std::wstring(buf.data(), n)).parent_path();
        if (buf.size() >= 32768) return fs::current_path();
        buf.resize(buf.size() * 2);
    }
}

fs::path roamingAppDataDirectory() {
    PWSTR raw = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &raw)) && raw) out = raw;
    CoTaskMemFree(raw);
    if (out.empty()) {
        wchar_t env[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"APPDATA", env, MAX_PATH);
        if (n > 0 && n < MAX_PATH) out = env;
    }
    return out;
}

AppPaths resolveAppPaths(const fs::path& exeDir, const fs::path& appData, const fs::path& overrideDataDir) {
    AppPaths p;
    p.exeDir = exeDir;
    std::error_code ec;
    if (!overrideDataDir.empty()) {
        p.dataDir = overrideDataDir.is_relative() ? fs::absolute(overrideDataDir, ec) : overrideDataDir;
        if (ec) p.dataDir = overrideDataDir;
        p.portable = false;
    } else if (fs::is_regular_file(exeDir / L"portable.flag", ec) || fs::is_directory(exeDir / L"data", ec)) {
        p.dataDir = exeDir / L"data";
        p.portable = true;
    } else {
        if (appData.empty()) throw std::runtime_error("Cannot determine the %APPDATA% directory");
        p.dataDir = appData / L"SSHTerm";
    }
    p.dataDir = p.dataDir.lexically_normal();
    p.logsDir = p.dataDir / L"logs";
    p.webViewDataDir = p.dataDir / L"WebView2";
    return p;
}

AppPaths initAppPaths(const fs::path& overrideDataDir) {
    fs::path override = overrideDataDir;
    if (override.empty()) {
        std::vector<wchar_t> env(32768);
        DWORD n = GetEnvironmentVariableW(L"SSHTERM_DATA_DIR", env.data(), static_cast<DWORD>(env.size()));
        if (n > 0 && n < env.size()) override = std::wstring(env.data(), n);
    }
    AppPaths p = resolveAppPaths(exeDirectory(), roamingAppDataDirectory(), override);
    std::error_code ec;
    fs::create_directories(p.dataDir, ec);
    if (ec) throw std::runtime_error("Cannot create the data directory: " + ec.message());
    fs::create_directories(p.logsDir, ec);
    fs::create_directories(p.webViewDataDir, ec);
    return p;
}

}  // namespace st
