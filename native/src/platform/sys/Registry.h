#pragma once
// Read access to the current user's part of the Windows registry (HKEY_CURRENT_USER), e.g. PuTTY's saved sessions.
// Other OSes have no registry: every key reads as missing.
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace st::sys::registry {

struct Value {
    enum class Type { String, Dword, Binary } type = Type::Binary;
    std::string str;           // String: REG_SZ / REG_EXPAND_SZ, UTF-8
    std::uint32_t dword = 0;   // Dword: REG_DWORD
    std::string bytes;         // Binary: any other type, raw data
};

/** Names of the subkeys of HKEY_CURRENT_USER\<path> (UTF-8, '\' separated path); empty when the key is missing. */
std::vector<std::string> subkeys(const std::string& path);
/** Values of HKEY_CURRENT_USER\<path> in registry order; nullopt when the key cannot be opened. */
std::optional<std::vector<std::pair<std::string, Value>>> values(const std::string& path);

}  // namespace st::sys::registry
