// Windows specifics of platform/sys/Fs.h: NTFS naming rules, executable types, the Mark-of-the-Web.
#include "Scratch.h"
#include "platform/sys/Fs.h"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <iterator>
#include <string>

using namespace st;
using st::test::Scratch;

TEST_CASE("fs (Windows): NTFS file name rules", "[platform][fs]") {
    REQUIRE(sys::sanitizeFileName("a<b>c:d\"e/f\\g|h?i*j") == "a_b_c_d_e_f_g_h_i_j");
    REQUIRE(sys::sanitizeFileName("tab\there") == "tab_here");
    REQUIRE(sys::sanitizeFileName("name. . ") == "name");
    REQUIRE(sys::sanitizeFileName("...") == "_");
    REQUIRE(sys::sanitizeFileName("CON") == "_CON");
    REQUIRE(sys::sanitizeFileName("nul.txt") == "_nul.txt");
    REQUIRE(sys::sanitizeFileName("com7.log") == "_com7.log");
    REQUIRE(sys::sanitizeFileName("console") == "console");
    REQUIRE(sys::sanitizeFileName("com10") == "com10");
}

TEST_CASE("fs (Windows): executable types and the Mark-of-the-Web", "[platform][fs]") {
    REQUIRE(sys::isExecutableType("setup.EXE"));
    REQUIRE(sys::isExecutableType("x.ps1"));
    REQUIRE(sys::isExecutableType("link.lnk"));
    REQUIRE_FALSE(sys::isExecutableType("notes.txt"));
    REQUIRE_FALSE(sys::isExecutableType("noext"));

    Scratch tmp;
    const auto file = tmp.path() / "download.txt";
    sys::writeFileAtomic(file, "hello");
    sys::markDownloaded(file);
    std::ifstream ads(file.wstring() + L":Zone.Identifier", std::ios::binary);
    REQUIRE(ads.good());
    const std::string zone((std::istreambuf_iterator<char>(ads)), std::istreambuf_iterator<char>());
    REQUIRE(zone.find("ZoneId=3") != std::string::npos);
    REQUIRE(sys::readFile(file) == std::optional<std::string>("hello"));  // the file itself is unchanged
}
