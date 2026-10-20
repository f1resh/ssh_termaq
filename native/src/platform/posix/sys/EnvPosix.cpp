#include "platform/sys/Env.h"

#include <pwd.h>
#include <unistd.h>

#include <cstdlib>
#include <vector>

namespace st::sys {

namespace {

/** The password database entry of the effective user, read into `buf`; nullptr if there is none. */
const passwd* currentUserEntry(passwd& pw, std::vector<char>& buf) {
    long size = sysconf(_SC_GETPW_R_SIZE_MAX);
    buf.resize(size > 0 ? static_cast<size_t>(size) : 16384);
    passwd* result = nullptr;
    if (getpwuid_r(geteuid(), &pw, buf.data(), buf.size(), &result) != 0) return nullptr;
    return result;
}

}  // namespace

std::filesystem::path homeDirectory() {
    if (const char* home = std::getenv("HOME"); home && *home) return home;
    passwd pw{};
    std::vector<char> buf;
    if (const passwd* e = currentUserEntry(pw, buf); e && e->pw_dir) return e->pw_dir;
    return {};
}

std::optional<std::string> getEnv(const std::string& name) {
    const char* v = std::getenv(name.c_str());
    if (!v || !*v) return std::nullopt;
    return std::string(v);
}

std::string userName() {
    passwd pw{};
    std::vector<char> buf;
    if (const passwd* e = currentUserEntry(pw, buf); e && e->pw_name && *e->pw_name) return e->pw_name;
    if (auto u = getEnv("USER")) return *u;
    return {};
}

}  // namespace st::sys
