// Charset translation (owner: session module).
//
// Decoding strategy per charset kind:
//  - UTF-8      : hand-written validator (W3C/Unicode "maximal subpart" replacement), holds incomplete sequences.
//  - single-byte: sys::codepage::decode on the whole chunk (every byte is a complete character).
//  - table      : ISO-8859 parts Windows does not ship (10, 11, 14, 16) use built-in 0xA0..0xFF tables.
//  - DBCS       : (932, 936, 949, 950, 20932, 51949) a trailing lead byte is held for the next chunk; lead bytes come
//                 from sys::codepage::isLeadByte. A lead byte followed by a byte < 0x40 is an invalid lone lead (U+FFFD).
//  - GB18030    : (54936) 1-, 2- and 4-byte units; incomplete units at the chunk end are held.
// Complete runs are converted strictly (sys::codepage::decodeStrict); if that fails the run is converted unit by unit
// so that only the invalid units become U+FFFD. The conversions themselves come from the OS (platform/sys/CodePage.h).
#include "session/Charset.h"

#include "platform/sys/CodePage.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <unordered_map>

namespace st {

namespace {

constexpr std::string_view kReplacement = "\xEF\xBF\xBD";  // U+FFFD
constexpr unsigned kUtf8 = 65001;
constexpr unsigned kGb18030 = 54936;

/** Charset names offered by the UI (ui/src/shared/algorithms.ts CHARSETS), in the same order. */
const std::vector<std::string>& charsetList() {
    static const std::vector<std::string> list = {
        "UTF-8",        "ISO-8859-1",   "ISO-8859-2",   "ISO-8859-3",   "ISO-8859-4",   "ISO-8859-5",   "ISO-8859-6",
        "ISO-8859-7",   "ISO-8859-8",   "ISO-8859-9",   "ISO-8859-10",  "ISO-8859-11",  "ISO-8859-13",  "ISO-8859-14",
        "ISO-8859-15",  "ISO-8859-16",  "windows-1250", "windows-1251", "windows-1252", "windows-1253", "windows-1254",
        "windows-1255", "windows-1256", "windows-1257", "windows-1258", "KOI8-R",       "KOI8-U",       "CP437",
        "CP850",        "CP852",        "CP866",        "Shift_JIS",    "EUC-JP",       "GBK",          "GB18030",
        "Big5",         "EUC-KR",       "macintosh"};
    return list;
}

/* ---- built-in ISO-8859 tables (bytes 0xA0..0xFF; 0x00..0x9F map to U+0000..U+009F; 0 = unassigned) ---------- */

constexpr std::array<uint16_t, 96> kIso8859_10 = {
    0x00A0, 0x0104, 0x0112, 0x0122, 0x012A, 0x0128, 0x0136, 0x00A7, 0x013B, 0x0110, 0x0160, 0x0166, 0x017D, 0x00AD, 0x016A, 0x014A,
    0x00B0, 0x0105, 0x0113, 0x0123, 0x012B, 0x0129, 0x0137, 0x00B7, 0x013C, 0x0111, 0x0161, 0x0167, 0x017E, 0x2015, 0x016B, 0x014B,
    0x0100, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x012E, 0x010C, 0x00C9, 0x0118, 0x00CB, 0x0116, 0x00CD, 0x00CE, 0x00CF,
    0x00D0, 0x0145, 0x014C, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x0168, 0x00D8, 0x0172, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,
    0x0101, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x012F, 0x010D, 0x00E9, 0x0119, 0x00EB, 0x0117, 0x00ED, 0x00EE, 0x00EF,
    0x00F0, 0x0146, 0x014D, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x0169, 0x00F8, 0x0173, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x0138};

constexpr std::array<uint16_t, 96> kIso8859_14 = {
    0x00A0, 0x1E02, 0x1E03, 0x00A3, 0x010A, 0x010B, 0x1E0A, 0x00A7, 0x1E80, 0x00A9, 0x1E82, 0x1E0B, 0x1EF2, 0x00AD, 0x00AE, 0x0178,
    0x1E1E, 0x1E1F, 0x0120, 0x0121, 0x1E40, 0x1E41, 0x00B6, 0x1E56, 0x1E81, 0x1E57, 0x1E83, 0x1E60, 0x1EF3, 0x1E84, 0x1E85, 0x1E61,
    0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7, 0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
    0x0174, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x1E6A, 0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x0176, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7, 0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
    0x0175, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x1E6B, 0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x0177, 0x00FF};

constexpr std::array<uint16_t, 96> kIso8859_16 = {
    0x00A0, 0x0104, 0x0105, 0x0141, 0x20AC, 0x201E, 0x0160, 0x00A7, 0x0161, 0x00A9, 0x0218, 0x00AB, 0x0179, 0x00AD, 0x017A, 0x017B,
    0x00B0, 0x00B1, 0x010C, 0x0142, 0x017D, 0x201D, 0x00B6, 0x00B7, 0x017E, 0x010D, 0x0219, 0x00BB, 0x0152, 0x0153, 0x0178, 0x017C,
    0x00C0, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x0106, 0x00C6, 0x00C7, 0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
    0x0110, 0x0143, 0x00D2, 0x00D3, 0x00D4, 0x0150, 0x00D6, 0x015A, 0x0170, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x0118, 0x021A, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x0107, 0x00E6, 0x00E7, 0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
    0x0111, 0x0144, 0x00F2, 0x00F3, 0x00F4, 0x0151, 0x00F6, 0x015B, 0x0171, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x0119, 0x021B, 0x00FF};

/** ISO-8859-11 (TIS-620 + NBSP): 0xA1..0xDA -> U+0E01..U+0E3A, 0xDF..0xFB -> U+0E3F..U+0E5B, 0xDB..0xDE/0xFC.. unassigned. */
constexpr std::array<uint16_t, 96> makeIso8859_11() {
    std::array<uint16_t, 96> t{};
    t[0] = 0x00A0;
    for (unsigned b = 0xA1; b <= 0xFB; ++b)
        if (b < 0xDB || b > 0xDE) t[b - 0xA0] = static_cast<uint16_t>(0x0E00 + (b - 0xA0));
    return t;
}
constexpr std::array<uint16_t, 96> kIso8859_11 = makeIso8859_11();

/** Windows code page numbers assigned to the ISO parts above (not installed on Windows; we use our tables). */
const std::array<uint16_t, 96>* builtinTable(unsigned cp) {
    switch (cp) {
        case 28600: return &kIso8859_10;
        case 28601: return &kIso8859_11;
        case 28604: return &kIso8859_14;
        case 28606: return &kIso8859_16;
        default: return nullptr;
    }
}

/* ---- UTF-8 helpers ---------------------------------------------------------------------------------------- */

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

/** Valid second-byte range and continuation count for a UTF-8 lead byte; false for bytes that cannot start a sequence. */
bool utf8Lead(unsigned char b, int& need, unsigned char& lo, unsigned char& hi) {
    lo = 0x80;
    hi = 0xBF;
    if (b >= 0xC2 && b <= 0xDF) {
        need = 1;
    } else if (b == 0xE0) {
        need = 2;
        lo = 0xA0;
    } else if ((b >= 0xE1 && b <= 0xEC) || b == 0xEE || b == 0xEF) {
        need = 2;
    } else if (b == 0xED) {
        need = 2;
        hi = 0x9F;  // no surrogates
    } else if (b == 0xF0) {
        need = 3;
        lo = 0x90;  // no overlongs
    } else if (b >= 0xF1 && b <= 0xF3) {
        need = 3;
    } else if (b == 0xF4) {
        need = 3;
        hi = 0x8F;  // <= U+10FFFF
    } else {
        return false;
    }
    return true;
}

/**
 * Decodes one code point at s[i]. Returns false for an invalid sequence (i advanced past its maximal subpart,
 * at least one byte) — also for a sequence truncated by the end of the input.
 */
bool nextCodePoint(std::string_view s, size_t& i, uint32_t& cp) {
    const auto b = static_cast<unsigned char>(s[i]);
    if (b < 0x80) {
        cp = b;
        ++i;
        return true;
    }
    int need = 0;
    unsigned char lo = 0, hi = 0;
    if (!utf8Lead(b, need, lo, hi)) {
        ++i;
        return false;
    }
    uint32_t v = b & (need == 1 ? 0x1F : need == 2 ? 0x0F : 0x07);
    size_t j = i + 1;
    for (int k = 0; k < need; ++k, ++j) {
        if (j >= s.size()) {
            i = j;
            return false;
        }
        const auto c = static_cast<unsigned char>(s[j]);
        if (c < lo || c > hi) {
            i = j;
            return false;
        }
        v = (v << 6) | (c & 0x3F);
        lo = 0x80;
        hi = 0xBF;
    }
    i = j;
    cp = v;
    return true;
}

/* ---- charset resolution ----------------------------------------------------------------------------------- */

/** Lowercase alphanumerics only: "ISO-8859-1" -> "iso88591", "Shift_JIS" -> "shiftjis". */
std::string normalizeName(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) out += static_cast<char>(std::tolower(u));
    }
    return out;
}

const std::unordered_map<std::string, unsigned>& aliasMap() {
    static const std::unordered_map<std::string, unsigned> map = [] {
        std::unordered_map<std::string, unsigned> m;
        auto add = [&m](std::initializer_list<const char*> names, unsigned cp) {
            for (const char* n : names) m.emplace(normalizeName(n), cp);
        };
        add({"UTF-8", "utf8", "unicode-1-1-utf-8"}, 65001);
        // ISO-8859-x and their common aliases.
        add({"ISO-8859-1", "latin1", "l1", "cp819", "ibm819", "iso-ir-100"}, 28591);
        add({"ISO-8859-2", "latin2", "l2", "iso-ir-101"}, 28592);
        add({"ISO-8859-3", "latin3", "l3", "iso-ir-109"}, 28593);
        add({"ISO-8859-4", "latin4", "l4", "iso-ir-110"}, 28594);
        add({"ISO-8859-5", "cyrillic", "iso-ir-144"}, 28595);
        add({"ISO-8859-6", "arabic", "iso-ir-127"}, 28596);
        add({"ISO-8859-7", "greek", "greek8", "iso-ir-126"}, 28597);
        add({"ISO-8859-8", "hebrew", "iso-ir-138"}, 28598);
        add({"ISO-8859-9", "latin5", "l5", "iso-ir-148"}, 28599);
        add({"ISO-8859-10", "latin6", "l6", "iso-ir-157"}, 28600);
        add({"ISO-8859-11", "thai", "tis-620", "tis620"}, 28601);
        add({"ISO-8859-13", "latin7", "l7"}, 28603);
        add({"ISO-8859-14", "latin8", "l8", "iso-celtic"}, 28604);
        add({"ISO-8859-15", "latin9", "l9", "latin0"}, 28605);
        add({"ISO-8859-16", "latin10", "l10"}, 28606);
        // Windows code pages (also "cp1252", "win1252" as used by PuTTY: "Win1252 (Western)").
        for (unsigned cp = 1250; cp <= 1258; ++cp) {
            const std::string n = std::to_string(cp);
            add({("windows-" + n).c_str(), ("cp" + n).c_str(), ("win" + n).c_str(), ("ms" + n).c_str()}, cp);
        }
        add({"windows-874", "cp874", "win874"}, 874);
        add({"KOI8-R", "koi8", "cskoi8r"}, 20866);
        add({"KOI8-U", "koi8-ru"}, 21866);
        add({"CP437", "ibm437", "437", "dos-437", "cspc8codepage437"}, 437);
        add({"CP850", "ibm850", "850", "dos-850"}, 850);
        add({"CP852", "ibm852", "852", "dos-852"}, 852);
        add({"CP866", "ibm866", "866", "dos-866"}, 866);
        add({"Shift_JIS", "sjis", "shift-jis", "ms-kanji", "mskanji", "cp932", "ms932", "windows-31j", "windows-932", "x-sjis"},
            932);
        add({"EUC-JP", "eucjp", "x-euc-jp", "cseucpkdfmtjapanese"}, 20932);
        add({"GBK", "cp936", "ms936", "windows-936", "gb2312", "euc-cn", "euccn", "gb2312-80", "csgb2312", "x-gbk"}, 936);
        add({"GB18030"}, 54936);
        add({"Big5", "big-5", "cp950", "ms950", "windows-950", "big5-hkscs", "csbig5", "x-x-big5"}, 950);
        add({"EUC-KR", "euckr", "cseuckr"}, 51949);
        add({"cp949", "uhc", "ks_c_5601-1987", "ks-c-5601", "windows-949", "ms949"}, 949);
        add({"macintosh", "mac", "macroman", "x-mac-roman", "csmacintosh"}, 10000);
        add({"US-ASCII", "ascii", "ansi_x3.4-1968", "iso646-us"}, 20127);
        return m;
    }();
    return map;
}

enum class Kind { Utf8, SingleByte, Table, Dbcs, Gb18030 };

struct Resolved {
    Kind kind = Kind::Utf8;
    unsigned cp = kUtf8;
    const std::array<uint16_t, 96>* table = nullptr;
};

Resolved resolve(const std::string& charset) {
    Resolved r;
    const unsigned cp = codePageForCharset(charset);
    if (cp == 0 || cp == kUtf8) return r;
    if (const auto* t = builtinTable(cp)) {
        r.kind = Kind::Table;
        r.cp = cp;
        r.table = t;
        return r;
    }
    if (!sys::codepage::supported(cp)) return r;  // not available: fall back to UTF-8
    r.cp = cp;
    if (cp == kGb18030) {
        r.kind = Kind::Gb18030;
        return r;
    }
    r.kind = sys::codepage::isMultiByte(cp) ? Kind::Dbcs : Kind::SingleByte;
    return r;
}

}  // namespace

/* ---- public lookups --------------------------------------------------------------------------------------- */

unsigned int codePageForCharset(const std::string& charset) {
    const auto& map = aliasMap();
    auto it = map.find(normalizeName(charset));
    if (it != map.end()) return it->second;
    // PuTTY style names carry a description: "ISO-8859-1:1998 (Latin-1, West Europe)", "Win1252 (Western)".
    const size_t cut = charset.find_first_of(": (");
    if (cut != std::string::npos && cut > 0) {
        it = map.find(normalizeName(std::string_view(charset).substr(0, cut)));
        if (it != map.end()) return it->second;
    }
    return 0;
}

std::vector<std::string> supportedCharsets() { return charsetList(); }

/* ---- Decoder ---------------------------------------------------------------------------------------------- */

struct Decoder::Impl {
    Resolved r;
    std::array<bool, 256> lead{};
    // UTF-8 state: bytes of the current incomplete sequence, continuation bytes still needed, valid next-byte range.
    std::string seq;
    int need = 0;
    unsigned char lo = 0x80, hi = 0xBF;
    // DBCS / GB18030: bytes of an incomplete trailing unit (at most 3).
    std::string held;

    explicit Impl(const std::string& charset) : r(resolve(charset)) {
        if (r.kind == Kind::Dbcs)
            for (unsigned b = 0x80; b < 256; ++b) lead[b] = sys::codepage::isLeadByte(r.cp, static_cast<unsigned char>(b));
    }

    std::string decodeUtf8(std::string_view in) {
        std::string out;
        out.reserve(in.size() + seq.size());
        size_t i = 0;
        while (i < in.size()) {
            const auto b = static_cast<unsigned char>(in[i]);
            if (need == 0) {
                if (b < 0x80) {
                    size_t j = i + 1;
                    while (j < in.size() && static_cast<unsigned char>(in[j]) < 0x80) ++j;
                    out.append(in.data() + i, j - i);
                    i = j;
                    continue;
                }
                if (!utf8Lead(b, need, lo, hi)) {
                    out += kReplacement;
                    ++i;
                    continue;
                }
                seq.assign(1, static_cast<char>(b));
                ++i;
                continue;
            }
            if (b < lo || b > hi) {
                // Maximal subpart ends here: one U+FFFD, then re-examine b as a potential lead byte.
                out += kReplacement;
                seq.clear();
                need = 0;
                continue;
            }
            seq += static_cast<char>(b);
            lo = 0x80;
            hi = 0xBF;
            ++i;
            if (--need == 0) {
                out += seq;
                seq.clear();
            }
        }
        return out;
    }

    /** Length of the unit at p (DBCS / GB18030); sets `incomplete` when more bytes are needed to decide. */
    size_t unitLength(const unsigned char* p, size_t avail, bool& incomplete) const {
        incomplete = false;
        const unsigned char b = p[0];
        if (r.kind == Kind::Dbcs) {
            if (!lead[b]) return 1;
            if (avail < 2) {
                incomplete = true;
                return 0;
            }
            return p[1] >= 0x40 ? 2 : 1;  // trail bytes are never < 0x40: lone lead byte
        }
        // GB18030
        if (b < 0x81 || b == 0xFF) return 1;
        if (avail < 2) {
            incomplete = true;
            return 0;
        }
        const unsigned char b2 = p[1];
        if (b2 >= 0x30 && b2 <= 0x39) {
            if (avail < 3) {
                incomplete = true;
                return 0;
            }
            if (p[2] < 0x81 || p[2] == 0xFF) return 1;
            if (avail < 4) {
                incomplete = true;
                return 0;
            }
            return (p[3] >= 0x30 && p[3] <= 0x39) ? 4 : 1;
        }
        if ((b2 >= 0x40 && b2 <= 0x7E) || (b2 >= 0x80 && b2 <= 0xFE)) return 2;
        return 1;
    }

    /** Converts complete units; invalid units become U+FFFD. */
    std::string convertUnits(const unsigned char* p, size_t n) const {
        if (n == 0) return {};
        const auto* src = reinterpret_cast<const char*>(p);
        std::string out;
        if (sys::codepage::decodeStrict(r.cp, std::string_view(src, n), out)) return out;
        // Slow path: something in the run is invalid, convert unit by unit.
        out.clear();
        out.reserve(n + 8);
        size_t i = 0;
        while (i < n) {
            if (p[i] < 0x80) {  // ASCII is identical in all supported multi-byte code pages
                out += static_cast<char>(p[i]);
                ++i;
                continue;
            }
            bool incomplete = false;
            size_t ulen = unitLength(p + i, n - i, incomplete);
            if (incomplete || ulen == 0) ulen = n - i;
            std::string unit;
            if (sys::codepage::decodeStrict(r.cp, std::string_view(src + i, ulen), unit)) {
                out += unit;
                i += ulen;
                continue;
            }
            out += kReplacement;
            // An unmapped double-byte pair whose trail byte is ASCII: the trail byte is re-read on its own.
            i += (ulen == 2 && p[i + 1] < 0x80) ? 1 : ulen;
        }
        return out;
    }

    std::string decodeMbcs(std::string_view in) {
        std::string joined;
        std::string_view data = in;
        if (!held.empty()) {
            joined.reserve(held.size() + in.size());
            joined = held;
            joined.append(in);
            data = joined;
        }
        const auto* p = reinterpret_cast<const unsigned char*>(data.data());
        const size_t n = data.size();
        size_t cut = 0;
        while (cut < n) {
            bool incomplete = false;
            const size_t len = unitLength(p + cut, n - cut, incomplete);
            if (incomplete) break;
            cut += len;
        }
        std::string out = convertUnits(p, cut);
        held.assign(data.substr(cut));
        return out;
    }

    std::string decodeSingleByte(std::string_view in) const { return sys::codepage::decode(r.cp, in); }

    std::string decodeTable(std::string_view in) const {
        std::string out;
        out.reserve(in.size() * 2);
        for (char c : in) {
            const auto b = static_cast<unsigned char>(c);
            if (b < 0xA0) {
                appendUtf8(out, b);
                continue;
            }
            const uint16_t cp = (*r.table)[b - 0xA0];
            if (cp)
                appendUtf8(out, cp);
            else
                out += kReplacement;
        }
        return out;
    }
};

Decoder::Decoder(const std::string& charset) : impl_(std::make_unique<Impl>(charset)) {}
Decoder::~Decoder() = default;

std::string Decoder::decode(std::string_view bytes) {
    if (bytes.empty()) return {};
    switch (impl_->r.kind) {
        case Kind::Utf8: return impl_->decodeUtf8(bytes);
        case Kind::SingleByte: return impl_->decodeSingleByte(bytes);
        case Kind::Table: return impl_->decodeTable(bytes);
        case Kind::Dbcs:
        case Kind::Gb18030: return impl_->decodeMbcs(bytes);
    }
    return std::string(bytes);
}

std::string Decoder::flush() {
    std::string out;
    if (impl_->need > 0 || !impl_->seq.empty()) out += kReplacement;
    impl_->seq.clear();
    impl_->need = 0;
    if (!impl_->held.empty()) out += kReplacement;  // one incomplete unit at most
    impl_->held.clear();
    return out;
}

/* ---- encoding --------------------------------------------------------------------------------------------- */

std::string encodeFromUtf8(const std::string& charset, std::string_view utf8) {
    if (utf8.empty()) return {};
    const Resolved r = resolve(charset);
    if (r.kind == Kind::Utf8) return std::string(utf8);

    if (r.kind == Kind::Table) {
        std::string out;
        out.reserve(utf8.size());
        size_t i = 0;
        while (i < utf8.size()) {
            uint32_t cp = 0;
            if (!nextCodePoint(utf8, i, cp)) {
                out += '?';
                continue;
            }
            if (cp < 0xA0) {
                out += static_cast<char>(cp);
                continue;
            }
            char mapped = '?';
            for (size_t k = 0; k < r.table->size(); ++k)
                if ((*r.table)[k] == cp) {
                    mapped = static_cast<char>(0xA0 + k);
                    break;
                }
            out += mapped;
        }
        return out;
    }

    return sys::codepage::encode(r.cp, utf8);
}

std::string binaryStringToBytes(std::string_view utf8) {
    std::string out;
    out.reserve(utf8.size());
    size_t i = 0;
    while (i < utf8.size()) {
        uint32_t cp = 0;
        if (nextCodePoint(utf8, i, cp) && cp < 0x100) out += static_cast<char>(cp);
    }
    return out;
}

}  // namespace st
