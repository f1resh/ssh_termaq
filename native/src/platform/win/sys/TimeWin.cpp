#include "platform/sys/Time.h"

namespace st::sys {

bool localTime(std::time_t t, std::tm& out) {
    std::tm tm{};
    if (localtime_s(&tm, &t) != 0) return false;
    out = tm;
    return true;
}

bool utcTime(std::time_t t, std::tm& out) {
    std::tm tm{};
    if (gmtime_s(&tm, &t) != 0) return false;
    out = tm;
    return true;
}

}  // namespace st::sys
