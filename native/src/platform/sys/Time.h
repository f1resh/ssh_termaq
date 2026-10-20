#pragma once
// Thread-safe calendar time conversions (localtime_s / gmtime_s on Windows, localtime_r / gmtime_r on POSIX).
#include <ctime>

namespace st::sys {

/** Local time of `t`; false (and `out` unchanged) when it cannot be converted. */
bool localTime(std::time_t t, std::tm& out);
/** UTC time of `t`; false (and `out` unchanged) when it cannot be converted. */
bool utcTime(std::time_t t, std::tm& out);

}  // namespace st::sys
