// Code pages on macOS: single-byte code pages from Windows' own tables (platform/detail/CodePageTables.h), so every
// byte converts exactly as on Windows, undefined bytes included (strict conversions refuse them, lenient ones give
// Windows' private-use character for each); double-byte code pages and GB18030 through CoreFoundation, using the
// encodings that match Windows' (Shift_JIS = DOS Japanese / CP932, GBK = CP936, UHC = CP949, Big5 = CP950).
#include "platform/sys/CodePage.h"

#include "platform/detail/CodePageTables.h"

#include <CoreFoundation/CoreFoundation.h>

#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace st::sys::codepage {

namespace {

constexpr unsigned kGb18030 = 54936;
constexpr unsigned kAnsi = 1252;  // the "ANSI" code page of Windows programs: macOS has none, assume Western
constexpr char32_t kReplacement = 0xFFFD;

CFStringEncoding cfEncoding(unsigned cp) {
    switch (cp) {
        case 932: return kCFStringEncodingDOSJapanese;
        case 936: return kCFStringEncodingDOSChineseSimplif;
        case 949: return kCFStringEncodingDOSKorean;
        case 950: return kCFStringEncodingDOSChineseTrad;
        case 20932: return kCFStringEncodingEUC_JP;
        case 51949: return kCFStringEncodingEUC_KR;
        case kGb18030: return kCFStringEncodingGB_18030_2000;
        default: return kCFStringEncodingInvalidId;
    }
}

void appendUtf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

/** UTF-8 -> code points; invalid sequences become U+FFFD (one per bad byte). */
std::vector<char32_t> codePoints(std::string_view s) {
    std::vector<char32_t> out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
        const unsigned char c = b(i);
        size_t len = 0;
        char32_t cp = 0;
        unsigned char lo = 0x80, hi = 0xBF;
        if (c < 0x80) {
            len = 1;
            cp = c;
        } else if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
            cp = c & 0x1F;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            cp = c & 0x0F;
            if (c == 0xE0) lo = 0xA0;
            if (c == 0xED) hi = 0x9F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            cp = c & 0x07;
            if (c == 0xF0) lo = 0x90;
            if (c == 0xF4) hi = 0x8F;
        }
        bool ok = len > 0 && i + len <= s.size();
        for (size_t k = 1; ok && k < len; ++k) {
            const unsigned char t = b(i + k);
            if (t < (k == 1 ? lo : 0x80) || t > (k == 1 ? hi : 0xBF)) ok = false;
            cp = (cp << 6) | (t & 0x3F);
        }
        if (!ok) {
            out.push_back(kReplacement);
            ++i;
            continue;
        }
        out.push_back(cp);
        i += len;
    }
    return out;
}

std::string toUtf8(CFStringRef s) {
    const CFIndex n = CFStringGetLength(s);
    CFIndex size = 0;
    CFStringGetBytes(s, CFRangeMake(0, n), kCFStringEncodingUTF8, 0, false, nullptr, 0, &size);
    std::string out(static_cast<size_t>(size), '\0');
    CFStringGetBytes(s, CFRangeMake(0, n), kCFStringEncodingUTF8, 0, false, reinterpret_cast<UInt8*>(out.data()), size,
                     nullptr);
    return out;
}

/** Strict CoreFoundation decode; false when CF rejects the bytes. */
bool cfDecode(CFStringEncoding enc, std::string_view bytes, std::string& out) {
    CFStringRef s = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(bytes.data()),
                                            static_cast<CFIndex>(bytes.size()), enc, false);
    if (!s) return false;
    out = toUtf8(s);
    CFRelease(s);
    return true;
}

/** One code point in `enc`; empty when `enc` cannot represent it (no lossy or best-fit substitutes). */
std::string cfEncodeOne(CFStringEncoding enc, char32_t c) {
    UniChar u[2];
    CFIndex units = 1;
    if (c >= 0x10000) {
        c -= 0x10000;
        u[0] = static_cast<UniChar>(0xD800 + (c >> 10));
        u[1] = static_cast<UniChar>(0xDC00 + (c & 0x3FF));
        units = 2;
    } else {
        u[0] = static_cast<UniChar>(c);
    }
    CFStringRef s = CFStringCreateWithCharactersNoCopy(nullptr, u, units, kCFAllocatorNull);
    if (!s) return {};
    UInt8 buf[16];
    CFIndex used = 0;
    const CFIndex converted = CFStringGetBytes(s, CFRangeMake(0, units), enc, 0, false, buf, sizeof buf, &used);
    CFRelease(s);
    if (converted != units) return {};
    return std::string(reinterpret_cast<const char*>(buf), static_cast<size_t>(used));
}

/** Reverse of a single-byte table: character -> byte, for the bytes whose character Windows converts back. */
const std::unordered_map<char32_t, char>& reverseTable(const detail::SingleByteTable& t) {
    static std::mutex m;
    static std::map<unsigned, std::unordered_map<char32_t, char>> cache;
    std::lock_guard lock(m);
    auto [it, inserted] = cache.try_emplace(t.cp);
    if (inserted) {
        for (unsigned i = 0; i < 128; ++i)
            if (detail::testBit(t.roundTrip, i)) it->second.try_emplace(t.high[i], static_cast<char>(0x80 + i));
    }
    return it->second;
}

bool tableDecode(const detail::SingleByteTable& t, std::string_view bytes, std::string& out, bool strict) {
    out.clear();
    out.reserve(bytes.size());
    for (const char ch : bytes) {
        const auto b = static_cast<unsigned char>(ch);
        if (b < 0x80) {
            out += ch;
            continue;
        }
        if (strict && detail::testBit(t.invalid, b - 0x80)) return false;
        const char16_t c = t.high[b - 0x80];
        appendUtf8(out, c == detail::kNoChar ? kReplacement : static_cast<char32_t>(c));
    }
    return true;
}

bool isLead(unsigned cp, unsigned char b) {
    const detail::LeadBytes* l = detail::leadBytes(cp);
    return l && (l->bits[b / 32] >> (b % 32)) & 1u;
}

}  // namespace

bool supported(unsigned cp) {
    if (detail::singleByteTable(cp)) return true;
    const CFStringEncoding enc = cfEncoding(cp);
    return enc != kCFStringEncodingInvalidId && CFStringIsEncodingAvailable(enc);
}

bool isMultiByte(unsigned cp) { return cfEncoding(cp) != kCFStringEncodingInvalidId; }

bool isLeadByte(unsigned cp, unsigned char b) { return isLead(cp, b); }

bool decodeStrict(unsigned cp, std::string_view bytes, std::string& out) {
    if (bytes.empty()) {
        out.clear();
        return true;
    }
    if (const auto* t = detail::singleByteTable(cp)) return tableDecode(*t, bytes, out, true);
    const CFStringEncoding enc = cfEncoding(cp);
    return enc != kCFStringEncodingInvalidId && cfDecode(enc, bytes, out);
}

std::string decode(unsigned cp, std::string_view bytes) {
    if (bytes.empty()) return {};
    std::string out;
    if (const auto* t = detail::singleByteTable(cp)) {
        tableDecode(*t, bytes, out, false);
        return out;
    }
    const CFStringEncoding enc = cfEncoding(cp);
    if (enc == kCFStringEncodingInvalidId) return std::string(bytes.size(), '?');
    if (cfDecode(enc, bytes, out)) return out;
    // Unit by unit: the shortest prefix that converts (1, 2 or 4 bytes for GB18030); U+FFFD for a byte that starts none.
    out.clear();
    for (size_t i = 0; i < bytes.size();) {
        bool done = false;
        for (size_t len : {1, 2, 4}) {
            std::string unit;
            if (i + len <= bytes.size() && cfDecode(enc, bytes.substr(i, len), unit)) {
                out += unit;
                i += len;
                done = true;
                break;
            }
        }
        if (!done) {
            appendUtf8(out, kReplacement);
            ++i;
        }
    }
    return out;
}

std::string encode(unsigned cp, std::string_view utf8) {
    if (utf8.empty()) return {};
    std::string out;
    out.reserve(utf8.size());
    if (const auto* t = detail::singleByteTable(cp)) {
        const auto& reverse = reverseTable(*t);
        for (const char32_t c : codePoints(utf8)) {
            if (c < 0x80) {
                out += static_cast<char>(c);
            } else if (auto it = reverse.find(c); it != reverse.end()) {
                out += it->second;
            } else {
                out += '?';
            }
        }
        return out;
    }
    const CFStringEncoding enc = cfEncoding(cp);
    if (enc == kCFStringEncodingInvalidId) return std::string(codePoints(utf8).size(), '?');
    // Character by character, so a character the code page lacks becomes exactly one '?' (never a best-fit
    // look-alike; a supplementary character included). GB18030 represents everything.
    for (const char32_t c : codePoints(utf8)) {
        if (c < 0x80 && cp != kGb18030) {
            out += static_cast<char>(c);
            continue;
        }
        const std::string bytes = c == kReplacement && cp != kGb18030 ? std::string() : cfEncodeOne(enc, c);
        out += bytes.empty() ? std::string("?") : bytes;
    }
    return out;
}

std::string toAnsi(std::string_view utf8) { return encode(kAnsi, utf8); }

std::string fromAnsi(std::string_view bytes) { return decode(kAnsi, bytes); }

}  // namespace st::sys::codepage
