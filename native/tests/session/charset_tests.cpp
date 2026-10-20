// Charset: code page lookup, streaming decoding (split at every byte boundary), encoding, binary strings.
#include "session/Charset.h"

#include "platform/sys/CodePage.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace st;

namespace {

const std::string kFFFD = "\xEF\xBF\xBD";

/** Decodes `bytes` split into two chunks at `cut` (plus flush). */
std::string decodeSplit(const std::string& charset, const std::string& bytes, size_t cut) {
    Decoder d(charset);
    std::string out = d.decode(std::string_view(bytes).substr(0, cut));
    out += d.decode(std::string_view(bytes).substr(cut));
    out += d.flush();
    return out;
}

/** Decodes byte by byte. */
std::string decodeBytewise(const std::string& charset, const std::string& bytes) {
    Decoder d(charset);
    std::string out;
    for (char c : bytes) out += d.decode(std::string_view(&c, 1));
    out += d.flush();
    return out;
}

/** Every 2-way and 3-way split plus byte-by-byte decoding must give `expected`. */
void checkAllSplits(const std::string& charset, const std::string& bytes, const std::string& expected) {
    INFO("charset " << charset);
    for (size_t cut = 0; cut <= bytes.size(); ++cut) {
        INFO("cut at " << cut);
        CHECK(decodeSplit(charset, bytes, cut) == expected);
    }
    for (size_t a = 0; a <= bytes.size(); ++a)
        for (size_t b = a; b <= bytes.size(); ++b) {
            Decoder d(charset);
            std::string out = d.decode(std::string_view(bytes).substr(0, a));
            out += d.decode(std::string_view(bytes).substr(a, b - a));
            out += d.decode(std::string_view(bytes).substr(b));
            out += d.flush();
            INFO("cuts at " << a << "," << b);
            CHECK(out == expected);
        }
    CHECK(decodeBytewise(charset, bytes) == expected);
}

bool codePageInstalled(unsigned cp) { return sys::codepage::supported(cp); }

}  // namespace

TEST_CASE("charset: names and aliases map to code pages", "[charset]") {
    CHECK(codePageForCharset("UTF-8") == 65001);
    CHECK(codePageForCharset("utf8") == 65001);
    CHECK(codePageForCharset("ISO-8859-1") == 28591);
    CHECK(codePageForCharset("latin1") == 28591);
    CHECK(codePageForCharset("iso-8859-15") == 28605);
    CHECK(codePageForCharset("cp1252") == 1252);
    CHECK(codePageForCharset("WINDOWS-1251") == 1251);
    CHECK(codePageForCharset("koi8r") == 20866);
    CHECK(codePageForCharset("KOI8-U") == 21866);
    CHECK(codePageForCharset("sjis") == 932);
    CHECK(codePageForCharset("Shift_JIS") == 932);
    CHECK(codePageForCharset("eucjp") == 20932);
    CHECK(codePageForCharset("gb2312") == 936);
    CHECK(codePageForCharset("GBK") == 936);
    CHECK(codePageForCharset("gb18030") == 54936);
    CHECK(codePageForCharset("big5") == 950);
    CHECK(codePageForCharset("euc-kr") == 51949);
    CHECK(codePageForCharset("macintosh") == 10000);
    CHECK(codePageForCharset("cp437") == 437);
    CHECK(codePageForCharset("CP866") == 866);
    // PuTTY-style descriptive names.
    CHECK(codePageForCharset("ISO-8859-2:1999 (Latin-2, East Europe)") == 28592);
    CHECK(codePageForCharset("Win1252 (Western)") == 1252);
    CHECK(codePageForCharset("no-such-charset") == 0);
    CHECK(codePageForCharset("") == 0);

    const auto all = supportedCharsets();
    REQUIRE(all.size() == 38);
    CHECK(all.front() == "UTF-8");
    for (const auto& name : all) {
        INFO(name);
        CHECK(codePageForCharset(name) != 0);
        // ASCII passes through every supported charset unchanged.
        Decoder d(name);
        CHECK(d.decode("Hello, world! 0123456789 ~") == "Hello, world! 0123456789 ~");
        CHECK(encodeFromUtf8(name, "plain ASCII") == "plain ASCII");
    }
}

TEST_CASE("charset: UTF-8 multi-byte characters split at every byte boundary", "[charset]") {
    const std::string text = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80z";  // a é € 😀 z
    checkAllSplits("UTF-8", text, text);
    checkAllSplits("utf8", "\xD0\x9F\xD1\x80\xD0\xB8\xE4\xB8\xAD", "\xD0\x9F\xD1\x80\xD0\xB8\xE4\xB8\xAD");
}

TEST_CASE("charset: invalid UTF-8 becomes U+FFFD (maximal subparts)", "[charset]") {
    CHECK(decodeSplit("UTF-8", "a\xFF" "b", 1) == "a" + kFFFD + "b");
    CHECK(decodeSplit("UTF-8", "\xC0\xAF", 1) == kFFFD + kFFFD);                     // overlong lead bytes
    CHECK(decodeSplit("UTF-8", "\xE2\x82" "A", 1) == kFFFD + "A");                   // truncated 3-byte sequence
    CHECK(decodeSplit("UTF-8", "\xED\xA0\x80", 2) == kFFFD + kFFFD + kFFFD);         // surrogate
    CHECK(decodeSplit("UTF-8", "\xF4\x90\x80\x80", 1) == kFFFD + kFFFD + kFFFD + kFFFD);  // > U+10FFFF
    CHECK(decodeSplit("UTF-8", "\x80" "x", 0) == kFFFD + "x");                       // lone continuation
    CHECK(decodeSplit("UTF-8", "\xE2\x82\xE2\x82\xAC", 3) == kFFFD + "\xE2\x82\xAC");  // restart at new lead

    Decoder d("UTF-8");
    CHECK(d.decode("ok\xF0\x9F") == "ok");  // incomplete sequence is held
    CHECK(d.flush() == kFFFD);              // ...and flushed as one U+FFFD
    CHECK(d.decode("\x98\x80") == kFFFD + kFFFD);  // stale continuation bytes after flush
}

TEST_CASE("charset: DBCS code pages hold a trailing lead byte", "[charset]") {
    // Shift_JIS: 日本語 + half-width katakana ｱ (single byte 0xB1) + ASCII.
    checkAllSplits("Shift_JIS", "\x93\xFA\x96\x7B\x8C\xEA\xB1z", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xEF\xBD\xB1z");
    // GBK: 中文
    checkAllSplits("GBK", "\xD6\xD0\xCE\xC4!", "\xE4\xB8\xAD\xE6\x96\x87!");
    // Big5: 中文
    checkAllSplits("Big5", "\xA4\xA4\xA4\xE5", "\xE4\xB8\xAD\xE6\x96\x87");
    // EUC-JP: 日本
    if (codePageInstalled(20932)) checkAllSplits("EUC-JP", "\xC6\xFC\xCB\xDC", "\xE6\x97\xA5\xE6\x9C\xAC");
    // EUC-KR: 한
    if (codePageInstalled(51949)) checkAllSplits("EUC-KR", "\xC7\xD1", "\xED\x95\x9C");

    // A lone lead byte at the end of the stream is flushed as U+FFFD; a lead byte followed by a control is invalid.
    Decoder d("Shift_JIS");
    CHECK(d.decode("A\x93") == "A");
    CHECK(d.flush() == kFFFD);
    CHECK(decodeSplit("Shift_JIS", "\x93\r\n", 1) == kFFFD + "\r\n");
}

TEST_CASE("charset: GB18030 two- and four-byte sequences", "[charset]") {
    // 中 (2 bytes), U+0080 (4 bytes: 81 30 81 30), 😀 U+1F600 (4 bytes: 94 39 FC 36)
    const std::string bytes = "\xD6\xD0\x81\x30\x81\x30\x94\x39\xFC\x36" "x";
    const std::string utf8 = "\xE4\xB8\xAD\xC2\x80\xF0\x9F\x98\x80x";
    checkAllSplits("GB18030", bytes, utf8);
    CHECK(encodeFromUtf8("GB18030", utf8) == bytes);
    Decoder d("GB18030");
    CHECK(d.decode("\x81\x30\x81").empty());  // incomplete 4-byte unit held
    CHECK(d.flush() == kFFFD);
}

TEST_CASE("charset: single-byte code pages roundtrip", "[charset]") {
    // KOI8-R: Привет
    const std::string koi = "\xF0\xD2\xC9\xD7\xC5\xD4";
    const std::string privet = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";
    checkAllSplits("KOI8-R", koi, privet);
    CHECK(encodeFromUtf8("KOI8-R", privet) == koi);
    // CP437 box drawing ╔═╗
    const std::string box = "\xE2\x95\x94\xE2\x95\x90\xE2\x95\x97";
    CHECK(Decoder("CP437").decode("\xC9\xCD\xBB") == box);
    CHECK(encodeFromUtf8("CP437", box) == "\xC9\xCD\xBB");
    // windows-1252: € é
    CHECK(Decoder("windows-1252").decode("\x80\xE9") == "\xE2\x82\xAC\xC3\xA9");
    CHECK(encodeFromUtf8("cp1252", "\xE2\x82\xAC\xC3\xA9") == "\x80\xE9");
    // Every byte of ISO-8859-1 roundtrips.
    std::string all;
    for (int b = 1; b < 256; ++b) all += static_cast<char>(b);
    CHECK(encodeFromUtf8("ISO-8859-1", Decoder("ISO-8859-1").decode(all)) == all);
    // Built-in tables for ISO parts Windows does not ship.
    CHECK(Decoder("ISO-8859-10").decode("\xA1") == "\xC4\x84");     // Ą
    CHECK(encodeFromUtf8("ISO-8859-10", "\xC4\x84") == "\xA1");
    CHECK(Decoder("ISO-8859-16").decode("\xA4") == "\xE2\x82\xAC");  // €
    CHECK(Decoder("ISO-8859-11").decode("\xA1") == "\xE0\xB8\x81");  // ก
    CHECK(Decoder("ISO-8859-11").decode("\xDB") == kFFFD);           // unassigned
    CHECK(Decoder("ISO-8859-14").decode("\xA1") == "\xE1\xB8\x82");  // Ḃ
}

TEST_CASE("charset: unmappable characters encode as '?'", "[charset]") {
    CHECK(encodeFromUtf8("windows-1252", "a\xE4\xB8\xAD" "b") == "a?b");         // 中
    CHECK(encodeFromUtf8("windows-1252", "\xC4\x81") == "?");                     // ā: no best-fit to 'a'
    CHECK(encodeFromUtf8("KOI8-R", "\xC3\xA9") == "?");                           // é
    CHECK(encodeFromUtf8("Shift_JIS", "x\xF0\x9F\x98\x80y") == "x?y");            // 😀 -> single '?'
    CHECK(encodeFromUtf8("ISO-8859-10", "\xE2\x82\xAC") == "?");                   // € not in Latin-6
    CHECK(encodeFromUtf8("Shift_JIS", "\xE6\x97\xA5") == "\x93\xFA");             // 日
    CHECK(encodeFromUtf8("UTF-8", "\xF0\x9F\x98\x80") == "\xF0\x9F\x98\x80");     // passthrough
    CHECK(encodeFromUtf8("no-such-charset", "\xC3\xA9") == "\xC3\xA9");           // unknown -> UTF-8
}

TEST_CASE("charset: binary strings", "[charset]") {
    const std::string in = std::string("\x00\x7F", 2) + "\xC2\x80" "\xC3\xBF" "\xC4\x80" "A";
    CHECK(binaryStringToBytes(in) == std::string("\x00\x7F\x80\xFF" "A", 5));
    CHECK(binaryStringToBytes("") .empty());
    CHECK(binaryStringToBytes("\x1B[A") == "\x1B[A");
}
