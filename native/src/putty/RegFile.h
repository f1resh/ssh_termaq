#pragma once
// Windows .reg files (regedit export format): parsing and writing.
// Parsing accepts "Windows Registry Editor Version 5.00" (normally UTF-16LE with BOM) and "REGEDIT4" (ANSI),
// as well as UTF-8 with or without BOM. Value types: "string", dword:XXXXXXXX, hex:, hex(2): (expand string),
// hex(7): (multi string), hex(b): (qword) with backslash line continuations; @ = default value; [-key] deletions
// and "name"=- deletions are ignored.
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace st::putty {

struct RegValue {
    enum class Type { String, Dword, Binary, ExpandString, MultiString, Qword } type = Type::String;
    std::string str;  // UTF-8 (String / ExpandString; MultiString joined with '\n')
    uint64_t num = 0; // Dword / Qword
    std::string bytes;
};

/** Case-insensitive value-name map (registry value names are case-insensitive). */
struct CaseInsensitiveLess {
    bool operator()(const std::string& a, const std::string& b) const;
};
using RegValues = std::map<std::string, RegValue, CaseInsensitiveLess>;

struct RegKey {
    std::string path;  // e.g. "HKEY_CURRENT_USER\Software\SimonTatham\PuTTY\Sessions\My%20Server"
    RegValues values;
};

/** Parses a .reg file (raw file bytes). Throws std::runtime_error on an unrecognised header. */
std::vector<RegKey> parseRegFile(std::string_view fileBytes);
/** Serializes keys as "Windows Registry Editor Version 5.00", returned as UTF-16LE bytes with BOM and CRLF. */
std::string writeRegFile(const std::vector<RegKey>& keys);

/** Convenience accessors on a value map (with defaults). */
std::string regString(const RegValues& v, const std::string& name, const std::string& def = {});
int64_t regInt(const RegValues& v, const std::string& name, int64_t def = 0);
bool regHas(const RegValues& v, const std::string& name);

}  // namespace st::putty
