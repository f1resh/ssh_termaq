// The single-byte code page tables and double-byte lead bytes other OSes use (platform/detail/CodePageTables.h) must
// make them convert exactly as the Windows build does (platform/win/sys/CodePageWin.cpp): which bytes a strict
// conversion refuses, what each byte becomes (undefined bytes: Windows' private-use character for each), and which
// characters convert back. The hidden "[.generate]" case rewrites the table file from this Windows machine:
//   st_test_platform.exe "[.generate]"
#include "platform/detail/CodePageTables.h"
#include "platform/sys/CodePage.h"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>
#include <string>

using namespace st;
using sys::detail::kNoChar;

namespace {

/** What the Windows build does with one byte of code page `cp`. */
struct ByteResult {
    bool valid = false;      // decodeStrict accepts it
    char16_t ch = kNoChar;   // what decode makes of it (one BMP character; kNoChar if anything else)
    bool roundTrip = false;  // encode turns that character back into the byte
};

/** One BMP character from UTF-8 (kNoChar for anything else). */
char16_t singleBmpChar(const std::string& s) {
    const auto b = [&](size_t i) { return static_cast<unsigned char>(s[i]); };
    if (s.size() == 1 && b(0) < 0x80) return b(0);
    if (s.size() == 2 && (b(0) & 0xE0) == 0xC0) return static_cast<char16_t>(((b(0) & 0x1F) << 6) | (b(1) & 0x3F));
    if (s.size() == 3 && (b(0) & 0xF0) == 0xE0)
        return static_cast<char16_t>(((b(0) & 0x0F) << 12) | ((b(1) & 0x3F) << 6) | (b(2) & 0x3F));
    return kNoChar;
}

ByteResult windows(unsigned cp, unsigned char byte) {
    namespace cpg = sys::codepage;
    const std::string in(1, static_cast<char>(byte));
    ByteResult r;
    std::string out;
    r.valid = cpg::decodeStrict(cp, in, out);
    const std::string lenient = cpg::decode(cp, in);
    r.ch = singleBmpChar(lenient);
    r.roundTrip = r.ch != kNoChar && cpg::encode(cp, lenient) == in;
    return r;
}

char hexDigit(unsigned v) { return "0123456789ABCDEF"[v & 15]; }

std::string hex4(unsigned v) { return {'0', 'x', hexDigit(v >> 12), hexDigit(v >> 8), hexDigit(v >> 4), hexDigit(v)}; }

std::string hex8(std::uint32_t v) { return hex4(v >> 16) + hex4(v & 0xFFFF).substr(2) + "u"; }

std::string bitSet(const std::uint32_t* bits, int n) {
    std::string s = "{";
    for (int i = 0; i < n; ++i) s += hex8(bits[i]) + (i + 1 < n ? ", " : "");
    return s + "}";
}

/** The table file as this Windows machine converts, in the layout of the existing file (only the data changes). */
std::string generateTables() {
    std::ostringstream o;
    o << "// Single-byte code page tables and double-byte lead bytes (platform/detail/CodePageTables.h).\n"
         "// Generated on Windows by\n"
         "//   st_test_platform.exe \"[.generate]\"\n"
         "// from the Windows build's conversions. Do not edit by hand.\n"
         "#include \"platform/detail/CodePageTables.h\"\n\n"
         "namespace st::sys::detail {\n\nnamespace {\n\nconst SingleByteTable kTables[] = {\n";
    size_t count = 0;
    const sys::detail::SingleByteTable* tables = sys::detail::singleByteTables(count);
    for (size_t t = 0; t < count; ++t) {
        const unsigned cp = tables[t].cp;
        std::uint32_t invalid[4] = {}, roundTrip[4] = {};
        char16_t high[128];
        for (unsigned i = 0; i < 128; ++i) {
            const ByteResult r = windows(cp, static_cast<unsigned char>(0x80 + i));
            if (!r.valid) invalid[i / 32] |= 1u << (i % 32);
            if (r.roundTrip) roundTrip[i / 32] |= 1u << (i % 32);
            high[i] = r.ch;
        }
        o << "    {" << cp << ", " << bitSet(invalid, 4) << ", " << bitSet(roundTrip, 4) << ", {\n";
        for (unsigned i = 0; i < 128; i += 8) {
            o << "    ";
            for (unsigned k = i; k < i + 8; ++k) o << " " << hex4(high[k]) << (k + 1 < 128 ? "," : "");
            o << "\n";
        }
        o << "    }},\n";
    }
    o << "};\n\nconst LeadBytes kLeads[] = {\n";
    const sys::detail::LeadBytes* leads = sys::detail::allLeadBytes(count);
    for (size_t l = 0; l < count; ++l) {
        std::uint32_t bits[8] = {};
        for (unsigned b = 0; b < 256; ++b)
            if (sys::codepage::isLeadByte(leads[l].cp, static_cast<unsigned char>(b))) bits[b / 32] |= 1u << (b % 32);
        o << "    {" << leads[l].cp << ", " << bitSet(bits, 8) << "},\n";
    }
    o << "};\n\n}  // namespace\n\n"
         "const SingleByteTable* singleByteTable(unsigned cp) {\n"
         "    for (const auto& t : kTables)\n"
         "        if (t.cp == cp) return &t;\n"
         "    return nullptr;\n}\n\n"
         "const SingleByteTable* singleByteTables(std::size_t& count) {\n"
         "    count = sizeof kTables / sizeof kTables[0];\n"
         "    return kTables;\n}\n\n"
         "const LeadBytes* leadBytes(unsigned cp) {\n"
         "    for (const auto& l : kLeads)\n"
         "        if (l.cp == cp) return &l;\n"
         "    return nullptr;\n}\n\n"
         "const LeadBytes* allLeadBytes(std::size_t& count) {\n"
         "    count = sizeof kLeads / sizeof kLeads[0];\n"
         "    return kLeads;\n}\n\n"
         "}  // namespace st::sys::detail\n";
    return o.str();
}

}  // namespace

TEST_CASE("codepage (Windows): the single-byte tables are Windows' own", "[platform][codepage]") {
    size_t count = 0;
    const sys::detail::SingleByteTable* tables = sys::detail::singleByteTables(count);
    REQUIRE(count > 0);
    std::string mismatches;
    for (size_t t = 0; t < count; ++t) {
        const auto& table = tables[t];
        INFO("cp " << table.cp);
        REQUIRE(sys::codepage::supported(table.cp));
        for (unsigned b = 0; b < 0x100; ++b) {
            const ByteResult r = windows(table.cp, static_cast<unsigned char>(b));
            ByteResult expected;
            if (b < 0x80) {
                expected = {true, static_cast<char16_t>(b), true};  // ASCII
            } else {
                const unsigned i = b - 0x80;
                expected = {!sys::detail::testBit(table.invalid, i), table.high[i], sys::detail::testBit(table.roundTrip, i)};
            }
            if (r.valid != expected.valid || r.ch != expected.ch || r.roundTrip != expected.roundTrip) {
                mismatches += "cp" + std::to_string(table.cp) + " byte " + hex4(b) + ": Windows " +
                              (r.valid ? "valid " : "invalid ") + hex4(r.ch) + (r.roundTrip ? " round trip" : "") +
                              ", table " + (expected.valid ? "valid " : "invalid ") + hex4(expected.ch) +
                              (expected.roundTrip ? " round trip" : "") + "\n";
            }
        }
    }
    INFO("Run st_test_platform.exe \"[.generate]\" to rewrite the tables from Windows.\n" << mismatches);
    CHECK(mismatches.empty());
}

TEST_CASE("codepage (Windows): the lead bytes are Windows' own", "[platform][codepage]") {
    size_t count = 0;
    const sys::detail::LeadBytes* leads = sys::detail::allLeadBytes(count);
    std::string mismatches;
    for (size_t l = 0; l < count; ++l) {
        for (unsigned b = 0; b < 256; ++b) {
            const bool windowsLead = sys::codepage::isLeadByte(leads[l].cp, static_cast<unsigned char>(b));
            const bool table = ((leads[l].bits[b / 32] >> (b % 32)) & 1u) != 0;
            if (windowsLead != table) mismatches += "cp" + std::to_string(leads[l].cp) + " byte " + hex4(b) + "\n";
        }
    }
    INFO("Run st_test_platform.exe \"[.generate]\" to rewrite the tables from Windows.\n" << mismatches);
    CHECK(mismatches.empty());
}

TEST_CASE("codepage (Windows): regenerate the tables", "[.generate]") {
    const std::string path = std::string(ST_SOURCE_DIR) + "/native/src/platform/common/sys/CodePageTables.cpp";
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    REQUIRE(f);
    f << generateTables();
    REQUIRE(f.good());
    WARN("Wrote " << path);
}
