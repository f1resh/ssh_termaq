#pragma once
// Legacy character sets ("code pages") <-> UTF-8, for session/Charset (which keeps the streaming logic, the charset
// names and the built-in tables) and for text written by Windows programs. Code pages are identified by their Windows
// numbers everywhere (1252, 932, 20866, 54936, ...), on every OS.
#include <string>
#include <string_view>

namespace st::sys::codepage {

/** True when this OS can convert code page `cp`. */
bool supported(unsigned cp);
/** True when `cp` uses more than one byte for some characters (DBCS, GB18030). */
bool isMultiByte(unsigned cp);
/** True when byte `b` starts a two-byte character in the DBCS code page `cp`. */
bool isLeadByte(unsigned cp, unsigned char b);
/** Complete characters of `cp` -> UTF-8 in `out`; false (out unspecified) if any byte sequence is invalid. */
bool decodeStrict(unsigned cp, std::string_view bytes, std::string& out);
/** `cp` -> UTF-8, never failing: invalid bytes become the OS's replacement; one '?' per byte if nothing converts. */
std::string decode(unsigned cp, std::string_view bytes);
/** UTF-8 -> `cp`. Characters `cp` cannot represent (each supplementary character included) become one '?' each,
 *  without "best fit" substitutions; GB18030 represents everything. */
std::string encode(unsigned cp, std::string_view utf8);

/** UTF-8 -> the encoding of non-Unicode text written by Windows programs (the "ANSI" code page; best fit allowed). */
std::string toAnsi(std::string_view utf8);
/** ANSI text (see toAnsi) -> UTF-8. */
std::string fromAnsi(std::string_view bytes);

}  // namespace st::sys::codepage
