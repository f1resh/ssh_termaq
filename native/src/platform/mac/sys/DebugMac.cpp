#include "platform/sys/Debug.h"

#include <os/log.h>
#include <pthread.h>

#include <string>

namespace st::sys {

void debugOutput(std::string_view line) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
    const std::string text(line);
    os_log(OS_LOG_DEFAULT, "%{public}s", text.c_str());
}

void setThreadName(std::string_view name) {
    const std::string text(name.substr(0, 63));  // the limit of pthread names
    pthread_setname_np(text.c_str());
}

}  // namespace st::sys
