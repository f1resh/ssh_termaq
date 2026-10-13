// STUB — to be implemented (owner: keys/import module).
#include "keys/KeyLoader.h"

namespace sc {

ssh_key loadPrivateKey(const std::string&, const PassphraseCallback&, std::string& error) {
    error = "Key loading not implemented";
    return nullptr;
}

std::string detectKeyFormat(const std::string&) { return "unknown"; }

}  // namespace sc
