#pragma once
// UTF-8 <-> UTF-16 for Win32 calls. Private to platform/win (and the Windows tests in native/tests/platform/win).
#include <string>
#include <string_view>

namespace st {

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);

}  // namespace st
