#include "platform/sys/CodePage.h"

#include "platform/detail/WinApi.h"
#include "platform/win/sys/WinText.h"

namespace st::sys::codepage {

namespace {

constexpr unsigned kGb18030 = 54936;

std::wstring toWide(unsigned cp, DWORD flags, std::string_view bytes) {
    const int len = static_cast<int>(bytes.size());
    const int wn = MultiByteToWideChar(cp, flags, bytes.data(), len, nullptr, 0);
    if (wn <= 0) return {};
    std::wstring w(static_cast<size_t>(wn), L'\0');
    MultiByteToWideChar(cp, flags, bytes.data(), len, w.data(), wn);
    return w;
}

}  // namespace

bool supported(unsigned cp) { return IsValidCodePage(cp) != FALSE; }

bool isMultiByte(unsigned cp) {
    CPINFO info{};
    return GetCPInfo(cp, &info) && info.MaxCharSize > 1;
}

bool isLeadByte(unsigned cp, unsigned char b) { return IsDBCSLeadByteEx(cp, static_cast<BYTE>(b)) != FALSE; }

bool decodeStrict(unsigned cp, std::string_view bytes, std::string& out) {
    if (bytes.empty()) {
        out.clear();
        return true;
    }
    const std::wstring w = toWide(cp, MB_ERR_INVALID_CHARS, bytes);
    if (w.empty()) return false;
    out = narrow(w);
    return true;
}

std::string decode(unsigned cp, std::string_view bytes) {
    if (bytes.empty()) return {};
    const std::wstring w = toWide(cp, 0, bytes);
    if (w.empty()) return std::string(bytes.size(), '?');
    return narrow(w);
}

std::string encode(unsigned cp, std::string_view utf8) {
    if (utf8.empty()) return {};
    std::wstring w = widen(utf8);
    if (cp != kGb18030) {
        // No single/double-byte code page can represent supplementary characters; make each one a single '?'
        // (WideCharToMultiByte would emit one '?' per surrogate).
        std::wstring collapsed;
        collapsed.reserve(w.size());
        for (size_t i = 0; i < w.size(); ++i) {
            const wchar_t c = w[i];
            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size() && w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF) {
                collapsed += L'?';
                ++i;
            } else {
                collapsed += c;
            }
        }
        w.swap(collapsed);
    }
    const int wlen = static_cast<int>(w.size());
    // GB18030 maps all of Unicode; it rejects WC_NO_BEST_FIT_CHARS and a default character.
    DWORD flags = cp == kGb18030 ? 0 : WC_NO_BEST_FIT_CHARS;
    const char* defaultChar = cp == kGb18030 ? nullptr : "?";
    int n = WideCharToMultiByte(cp, flags, w.data(), wlen, nullptr, 0, defaultChar, nullptr);
    if (n <= 0 && GetLastError() == ERROR_INVALID_FLAGS) {
        flags = 0;
        n = WideCharToMultiByte(cp, flags, w.data(), wlen, nullptr, 0, defaultChar, nullptr);
    }
    if (n <= 0) return std::string(w.size(), '?');
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(cp, flags, w.data(), wlen, out.data(), n, defaultChar, nullptr);
    return out;
}

std::string toAnsi(std::string_view utf8) {
    const std::wstring w = widen(utf8);
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string fromAnsi(std::string_view bytes) {
    if (bytes.empty()) return {};
    return narrow(toWide(CP_ACP, 0, bytes));
}

}  // namespace st::sys::codepage
