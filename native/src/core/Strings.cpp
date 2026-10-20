#include "core/Strings.h"

#include <algorithm>
#include <cctype>
#include <cstdint>

namespace st {

std::string toLower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

bool startsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

std::vector<std::string> split(std::string_view s, char sep, bool skipEmpty) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t pos = s.find(sep, start);
        std::string_view part = s.substr(start, pos == std::string_view::npos ? std::string_view::npos : pos - start);
        if (!(skipEmpty && part.empty())) out.emplace_back(part);
        if (pos == std::string_view::npos) break;
        start = pos + 1;
    }
    return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string replaceAll(std::string s, std::string_view from, std::string_view to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

namespace {

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

/** Decodes the UTF-8 sequence at s[i]; false for an invalid one (i then skips its maximal subpart, >= 1 byte). */
bool nextUtf8(std::string_view s, size_t& i, uint32_t& cp) {
    const auto b = static_cast<unsigned char>(s[i]);
    if (b < 0x80) {
        cp = b;
        ++i;
        return true;
    }
    int need = 0;
    unsigned char lo = 0x80, hi = 0xBF;
    if (b >= 0xC2 && b <= 0xDF) {
        need = 1;
    } else if (b == 0xE0) {
        need = 2;
        lo = 0xA0;
    } else if ((b >= 0xE1 && b <= 0xEC) || b == 0xEE || b == 0xEF) {
        need = 2;
    } else if (b == 0xED) {
        need = 2;
        hi = 0x9F;
    } else if (b == 0xF0) {
        need = 3;
        lo = 0x90;
    } else if (b >= 0xF1 && b <= 0xF3) {
        need = 3;
    } else if (b == 0xF4) {
        need = 3;
        hi = 0x8F;
    } else {
        ++i;
        return false;
    }
    uint32_t v = b & (need == 1 ? 0x1F : need == 2 ? 0x0F : 0x07);
    size_t j = i + 1;
    for (int k = 0; k < need; ++k, ++j) {
        if (j >= s.size() || static_cast<unsigned char>(s[j]) < lo || static_cast<unsigned char>(s[j]) > hi) {
            i = j;
            return false;
        }
        v = (v << 6) | (static_cast<unsigned char>(s[j]) & 0x3F);
        lo = 0x80;
        hi = 0xBF;
    }
    i = j;
    cp = v;
    return true;
}

}  // namespace

bool isValidUtf8(std::string_view s) {
    size_t i = 0;
    uint32_t cp = 0;
    while (i < s.size())
        if (!nextUtf8(s, i, cp)) return false;
    return true;
}

std::string utf16leToUtf8(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size());
    const size_t units = bytes.size() / 2;
    auto unit = [&bytes](size_t k) {
        return static_cast<uint32_t>(static_cast<unsigned char>(bytes[2 * k]) |
                                     (static_cast<unsigned char>(bytes[2 * k + 1]) << 8));
    };
    for (size_t k = 0; k < units; ++k) {
        const uint32_t u = unit(k);
        if (u >= 0xD800 && u <= 0xDBFF && k + 1 < units && unit(k + 1) >= 0xDC00 && unit(k + 1) <= 0xDFFF) {
            appendUtf8(out, 0x10000 + ((u - 0xD800) << 10) + (unit(k + 1) - 0xDC00));
            ++k;
        } else if (u >= 0xD800 && u <= 0xDFFF) {
            appendUtf8(out, 0xFFFD);
        } else {
            appendUtf8(out, u);
        }
    }
    return out;
}

std::string utf8ToUtf16le(std::string_view utf8) {
    std::string out;
    out.reserve(utf8.size() * 2);
    auto put = [&out](uint32_t u) {
        out += static_cast<char>(u & 0xFF);
        out += static_cast<char>(u >> 8);
    };
    size_t i = 0;
    while (i < utf8.size()) {
        uint32_t cp = 0;
        if (!nextUtf8(utf8, i, cp)) cp = 0xFFFD;
        if (cp >= 0x10000) {
            put(0xD800 + ((cp - 0x10000) >> 10));
            put(0xDC00 + ((cp - 0x10000) & 0x3FF));
        } else {
            put(cp);
        }
    }
    return out;
}

std::string hexEncode(std::string_view bytes) {
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        out.push_back(d[c >> 4]);
        out.push_back(d[c & 15]);
    }
    return out;
}

bool hexDecode(std::string_view hex, std::string& out) {
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (hex.size() % 2) return false;
    out.clear();
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        int a = val(hex[i]), b = val(hex[i + 1]);
        if (a < 0 || b < 0) return false;
        out.push_back(static_cast<char>((a << 4) | b));
    }
    return true;
}

}  // namespace st
