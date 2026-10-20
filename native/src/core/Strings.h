#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace st {

std::string toLower(std::string_view s);
std::string trim(std::string_view s);
bool startsWith(std::string_view s, std::string_view prefix);
bool endsWith(std::string_view s, std::string_view suffix);
bool iequals(std::string_view a, std::string_view b);
std::vector<std::string> split(std::string_view s, char sep, bool skipEmpty = false);
std::string join(const std::vector<std::string>& parts, std::string_view sep);
std::string replaceAll(std::string s, std::string_view from, std::string_view to);

/** True when `s` is valid UTF-8 (no overlong forms, surrogates or code points above U+10FFFF). */
bool isValidUtf8(std::string_view s);
/** UTF-16LE bytes -> UTF-8. Unpaired surrogates become U+FFFD; an odd trailing byte is ignored; NULs are kept. */
std::string utf16leToUtf8(std::string_view bytes);
/** UTF-8 -> UTF-16LE bytes (no BOM). Invalid UTF-8 becomes U+FFFD. */
std::string utf8ToUtf16le(std::string_view utf8);

/** Hex encode/decode (lowercase). decode returns false on invalid input. */
std::string hexEncode(std::string_view bytes);
bool hexDecode(std::string_view hex, std::string& out);

}  // namespace st
