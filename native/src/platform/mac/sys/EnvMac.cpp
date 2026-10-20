#include "platform/sys/Env.h"

#include <mach-o/dyld.h>
#include <sys/stat.h>

#include <climits>
#include <cstdlib>
#include <vector>

namespace fs = std::filesystem;

namespace st::sys {

fs::path executablePath() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    char resolved[PATH_MAX];
    if (realpath(buf.data(), resolved)) return resolved;
    return buf.data();
}

std::int64_t executableBuildTime() {
    const fs::path exe = executablePath();
    struct stat st {};
    if (exe.empty() || stat(exe.c_str(), &st) != 0) return 0;
    return static_cast<std::int64_t>(st.st_mtime);
}

fs::path userDataRoot() {
    fs::path home = homeDirectory();
    if (home.empty()) return {};
    return home / "Library" / "Application Support";
}

}  // namespace st::sys
