// Contracts of the other platform/sys services that behave the same on every OS.
#include "platform/Platform.h"
#include "platform/sys/CodePage.h"
#include "platform/sys/Env.h"
#include "platform/sys/Errors.h"
#include "platform/sys/Registry.h"
#include "platform/sys/Secret.h"
#include "platform/sys/Time.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace st;

TEST_CASE("platform: names", "[platform]") {
    const std::string os = sys::osName(), arch = sys::archName();
    REQUIRE((os == "win32" || os == "darwin"));
    REQUIRE((arch == "x64" || arch == "arm64"));
}

TEST_CASE("env: executable, folders, variables", "[platform][env]") {
    REQUIRE(std::filesystem::exists(sys::executablePath()));
    REQUIRE(sys::executableBuildTime() > 1600000000);
    REQUIRE(std::filesystem::is_directory(sys::homeDirectory()));
    REQUIRE_FALSE(sys::userDataRoot().empty());
    REQUIRE_FALSE(sys::userName().empty());
    REQUIRE_FALSE(sys::getEnv("SSHTERMAQ_SURELY_NOT_SET_42").has_value());
    REQUIRE_FALSE(sys::errorMessage(2).empty());  // ENOENT / ERROR_FILE_NOT_FOUND
}

TEST_CASE("time: calendar conversions", "[platform][time]") {
    std::tm tm{};
    REQUIRE(sys::utcTime(0, tm));
    REQUIRE(tm.tm_year == 70);
    REQUIRE(tm.tm_mday == 1);
    REQUIRE(sys::utcTime(1700000000, tm));
    REQUIRE(tm.tm_year == 123);  // 2023-11-14 22:13:20 UTC
    REQUIRE(tm.tm_mon == 10);
    REQUIRE(tm.tm_mday == 14);
    REQUIRE(tm.tm_hour == 22);
    REQUIRE(sys::localTime(1700000000, tm));
    REQUIRE(tm.tm_year == 123);
}

TEST_CASE("secret: round trip; foreign or damaged blobs do not decrypt", "[platform][secret]") {
    sys::useTestSecretKey(std::string(32, 'k'));  // never the user's real key store (macOS: the Keychain)
    static constexpr char kPlain[] = "p@ss w\xC3\xB6rd \0 with NUL";
    const std::string plain(kPlain, sizeof kPlain - 1);
    const std::string blob = sys::protectSecret(plain);
    REQUIRE_FALSE(blob.empty());
    REQUIRE(blob.find("p@ss") == std::string::npos);
    REQUIRE(sys::unprotectSecret(blob) == plain);
    std::string damaged = blob;
    damaged[damaged.size() / 2] ^= 0x5A;
    REQUIRE_FALSE(sys::unprotectSecret(damaged).has_value());
    REQUIRE_FALSE(sys::unprotectSecret("AAAA").has_value());
    sys::useTestSecretKey("");
}

TEST_CASE("codepage: conversions every OS must agree on", "[platform][codepage]") {
    using namespace sys::codepage;
    REQUIRE(supported(1252));
    REQUIRE(supported(932));
    REQUIRE_FALSE(isMultiByte(1252));
    REQUIRE(isMultiByte(932));
    REQUIRE(isLeadByte(932, 0x82));
    REQUIRE_FALSE(isLeadByte(932, 0x41));

    std::string out;
    REQUIRE(decodeStrict(1252, "caf\xE9 \x80", out));
    REQUIRE(out == "caf\xC3\xA9 \xE2\x82\xAC");               // café €
    REQUIRE(decodeStrict(932, "\x82\xA0\x82\xA2", out));
    REQUIRE(out == "\xE3\x81\x82\xE3\x81\x84");                // あい
    REQUIRE_FALSE(decodeStrict(932, "\x82\x20", out));          // lead byte + invalid trail
    REQUIRE(decodeStrict(1251, "", out));
    REQUIRE(out.empty());
    REQUIRE(decode(1251, "\xCF\xF0\xE8\xE2\xE5\xF2") == "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82");  // Привет

    REQUIRE(encode(1252, "caf\xC3\xA9 \xE2\x82\xAC") == "caf\xE9 \x80");
    REQUIRE(encode(1252, "\xE6\x97\xA5") == "?");               // 日 is not in cp1252
    REQUIRE(encode(1252, "a\xF0\x9F\x98\x80" "b") == "a?b");    // one '?' per supplementary character
    REQUIRE(encode(932, "\xE3\x81\x82") == "\x82\xA0");
    REQUIRE(encode(54936, "\xF0\x9F\x98\x80") == "\x94\x39\xFC\x36");  // GB18030 maps everything (U+1F600)
    REQUIRE(decodeStrict(54936, "\x94\x39\xFC\x36", out));
    REQUIRE(out == "\xF0\x9F\x98\x80");

    // Where Windows' code pages differ from the standard tables, every OS converts as Windows does.
    REQUIRE(decode(932, "\x81\x60") == "\xEF\xBD\x9E");  // FULLWIDTH TILDE (JIS X 0208: WAVE DASH)
    REQUIRE(decode(932, "\x81\x5C") == "\xE2\x80\x95");  // HORIZONTAL BAR (JIS X 0208: EM DASH)
    REQUIRE(decode(949, "\x81\x41") == "\xEA\xB0\x82");  // U+AC02: Windows' UHC extension, not in EUC-KR
    REQUIRE(decode(936, "\xB0\xA1") == "\xE5\x95\x8A");  // U+554A
    REQUIRE(decode(950, "\xA4\x40") == "\xE4\xB8\x80");  // U+4E00
    REQUIRE(decode(1252, "\x81") == "\xC2\x81");          // undefined in cp1252: the C1 control U+0081
    REQUIRE(encode(932, "\xC3\xA9") == "?");              // no best-fit look-alikes ('e' for e-acute)
    REQUIRE(encode(932, "a\xF0\x9F\x98\x80" "b") == "a?b");

    REQUIRE(fromAnsi(toAnsi("plain ascii")) == "plain ascii");
}

TEST_CASE("registry: missing keys read as missing", "[platform][registry]") {
    REQUIRE(sys::registry::subkeys("Software\\SSHTermaq-tests\\does-not-exist").empty());
    REQUIRE_FALSE(sys::registry::values("Software\\SSHTermaq-tests\\does-not-exist").has_value());
}
