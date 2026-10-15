#include "app/BuildInfo.h"

#include "core/Win.h"

#include <ctime>

#include "st_build_info.h"  // generated: build/<preset>/generated (cmake/BuildInfo.cmake)

extern "C" IMAGE_DOS_HEADER __ImageBase;  // the module this code is linked into

namespace st {

namespace {

/** Link time stamped into the PE header of this module by the linker. */
std::int64_t linkTime() {
    const auto* dos = &__ImageBase;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(dos) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->FileHeader.TimeDateStamp;
}

}  // namespace

std::string isoUtc(std::int64_t unixSeconds) {
    if (unixSeconds <= 0) return {};
    const std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm tm{};
    if (gmtime_s(&tm, &t) != 0) return {};
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

const BuildInfo& buildInfo() {
    static const BuildInfo info{ST_VERSION, ST_BUILD_NUMBER, ST_BUILD_COMMIT, isoUtc(linkTime())};
    return info;
}

std::string describeBuild(const BuildInfo& info) {
    std::string s = info.version + " (build " + std::to_string(info.number) + ", " + info.commit;
    if (info.time.size() == 20) s += ", " + info.time.substr(0, 10) + " " + info.time.substr(11, 8) + " UTC";
    return s + ")";
}

}  // namespace st
