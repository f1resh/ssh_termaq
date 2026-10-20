// No registry outside Windows: every key reads as missing (platform/sys/Registry.h).
#include "platform/sys/Registry.h"

namespace st::sys::registry {

std::vector<std::string> subkeys(const std::string&) { return {}; }

std::optional<std::vector<std::pair<std::string, Value>>> values(const std::string&) { return std::nullopt; }

}  // namespace st::sys::registry
