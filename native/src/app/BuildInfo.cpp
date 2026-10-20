#include "app/BuildInfo.h"

#include "platform/sys/Env.h"
#include "platform/sys/Time.h"

#include <ctime>

#include "st_build_info.h"  // generated: build/<preset>/generated (cmake/BuildInfo.cmake)

namespace st {

std::string isoUtc(std::int64_t unixSeconds) {
    if (unixSeconds <= 0) return {};
    const std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm tm{};
    if (!sys::utcTime(t, tm)) return {};
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

const BuildInfo& buildInfo() {
    static const BuildInfo info{ST_VERSION, ST_BUILD_NUMBER, ST_BUILD_COMMIT, isoUtc(sys::executableBuildTime())};
    return info;
}

std::string describeBuild(const BuildInfo& info) {
    std::string s = info.version + " (build " + std::to_string(info.number) + ", " + info.commit;
    if (info.time.size() == 20) s += ", " + info.time.substr(0, 10) + " " + info.time.substr(11, 8) + " UTC";
    return s + ")";
}

}  // namespace st
