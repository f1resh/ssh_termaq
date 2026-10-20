#pragma once
// Private to platform/: Windows' single-byte code pages as tables and the lead bytes of its double-byte code pages,
// so other OSes convert exactly as Windows does (also the bytes a code page leaves undefined, which Windows turns
// into private-use characters of their own). The data in common/sys/CodePageTables.cpp is generated on Windows
// (native/tests/platform/win/codepage_win_tests.cpp, "[.generate]") and checked there against MultiByteToWideChar,
// WideCharToMultiByte and IsDBCSLeadByteEx on every Windows test run.
#include <cstddef>
#include <cstdint>

namespace st::sys::detail {

/** No character: the byte converts to nothing at all (shown as U+FFFD). */
constexpr char16_t kNoChar = 0xFFFF;

struct SingleByteTable {
    unsigned cp;
    std::uint32_t invalid[4];    // bit i: byte 0x80+i is refused by a strict conversion (MB_ERR_INVALID_CHARS)
    std::uint32_t roundTrip[4];  // bit i: high[i] converts back to byte 0x80+i (WC_NO_BEST_FIT_CHARS)
    char16_t high[128];          // what bytes 0x80..0xFF convert to (0x00..0x7F are ASCII in every table code page)
};

/** Bit `i` (0..127) of a 128-bit set. */
inline bool testBit(const std::uint32_t (&bits)[4], unsigned i) { return (bits[i / 32] >> (i % 32)) & 1u; }

/** The table of single-byte code page `cp`; nullptr when `cp` has none. */
const SingleByteTable* singleByteTable(unsigned cp);
/** All tables (Windows check). */
const SingleByteTable* singleByteTables(std::size_t& count);

struct LeadBytes {
    unsigned cp;
    std::uint32_t bits[8];  // bit b % 32 of bits[b / 32]: byte b starts a two-byte character
};

/** Lead bytes of double-byte code page `cp`; nullptr when `cp` has none listed. */
const LeadBytes* leadBytes(unsigned cp);
/** All lead-byte sets (Windows check). */
const LeadBytes* allLeadBytes(std::size_t& count);

}  // namespace st::sys::detail
