#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace st {

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);

std::string toLower(std::string_view s);
std::string trim(std::string_view s);
bool startsWith(std::string_view s, std::string_view prefix);
bool endsWith(std::string_view s, std::string_view suffix);
bool iequals(std::string_view a, std::string_view b);
std::vector<std::string> split(std::string_view s, char sep, bool skipEmpty = false);
std::string join(const std::vector<std::string>& parts, std::string_view sep);
std::string replaceAll(std::string s, std::string_view from, std::string_view to);

/** Win32 error code -> readable UTF-8 message. */
std::string winErrorMessage(unsigned long code);
/** Last Win32 error as readable message. */
std::string lastErrorMessage();
/** WSAGetLastError() as readable message. */
std::string lastWsaErrorMessage();

/** Hex encode/decode (lowercase). decode returns false on invalid input. */
std::string hexEncode(std::string_view bytes);
bool hexDecode(std::string_view hex, std::string& out);

}  // namespace st
