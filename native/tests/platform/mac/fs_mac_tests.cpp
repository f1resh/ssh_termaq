// macOS specifics of platform/sys/Fs.h: POSIX / APFS naming rules, permissions, executable types, the quarantine
// attribute, exclusive writers.
#include "Scratch.h"
#include "platform/sys/Fs.h"

#include <sys/stat.h>
#include <sys/xattr.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace st;
using st::test::Scratch;

TEST_CASE("fs (macOS): file name rules", "[platform][fs]") {
    REQUIRE(sys::sanitizeFileName("a/b") == "a_b");
    REQUIRE(sys::sanitizeFileName("tab\there\x7f") == "tab_here_");
    REQUIRE(sys::sanitizeFileName("..") == "__");
    REQUIRE(sys::sanitizeFileName("con.txt") == "con.txt");    // no reserved names
    REQUIRE(sys::sanitizeFileName("name. ") == "name. ");      // trailing dots and spaces are fine
    REQUIRE(sys::sanitizeFileName("a:b") == "a:b");            // Finder shows ':' as '/'
    REQUIRE(sys::sanitizeFileName("caf\xE9.txt") == "caf_.txt");  // not UTF-8: APFS refuses it
}

TEST_CASE("fs (macOS): new files written atomically are private", "[platform][fs]") {
    Scratch tmp;
    const auto file = tmp.path() / "secret.json";
    sys::writeFileAtomic(file, "{}");
    struct stat st {};
    REQUIRE(::stat(file.c_str(), &st) == 0);
    REQUIRE((st.st_mode & 0777) == 0600);
    ::chmod(file.c_str(), 0644);
    sys::writeFileAtomic(file, "{\"a\":1}");  // a replaced file keeps its permissions
    REQUIRE(::stat(file.c_str(), &st) == 0);
    REQUIRE((st.st_mode & 0777) == 0644);
}

TEST_CASE("fs (macOS): executable types and the quarantine attribute", "[platform][fs]") {
    REQUIRE(sys::isExecutableType("run.COMMAND"));
    REQUIRE(sys::isExecutableType("Tool.app"));
    REQUIRE(sys::isExecutableType("install.pkg"));
    REQUIRE(sys::isExecutableType("x.sh"));
    REQUIRE_FALSE(sys::isExecutableType("notes.txt"));

    Scratch tmp;
    const auto file = tmp.path() / "download";
    sys::writeFileAtomic(file, "#!/bin/sh\n");
    REQUIRE_FALSE(sys::isExecutableType(file));
    ::chmod(file.c_str(), 0755);
    REQUIRE(sys::isExecutableType(file));  // an execute bit makes it run in Terminal

    sys::markDownloaded(file);
    char value[128] = {};
    const ssize_t n = ::getxattr(file.c_str(), "com.apple.quarantine", value, sizeof value - 1, 0, 0);
    REQUIRE(n > 0);
    REQUIRE(std::string(value, static_cast<size_t>(n)).find(";SSHTermaq;") != std::string::npos);
    REQUIRE(sys::readFile(file) == std::optional<std::string>("#!/bin/sh\n"));  // the file itself is unchanged
}
