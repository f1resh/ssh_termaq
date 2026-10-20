#include "platform/sys/Time.h"

namespace st::sys {

bool localTime(std::time_t t, std::tm& out) {
    std::tm tm{};
    if (!localtime_r(&t, &tm)) return false;
    out = tm;
    return true;
}

bool utcTime(std::time_t t, std::tm& out) {
    std::tm tm{};
    if (!gmtime_r(&t, &tm)) return false;
    out = tm;
    return true;
}

}  // namespace st::sys
