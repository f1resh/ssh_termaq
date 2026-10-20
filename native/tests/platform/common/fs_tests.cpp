// sys::fs contract, the same on every OS (platform/sys/Fs.h).
#include "Scratch.h"
#include "platform/sys/Fs.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

namespace fs = std::filesystem;
using namespace st;
using st::test::Scratch;

namespace {

std::string slurp(const fs::path& p) { return sys::readFile(p).value_or("<missing>"); }

}  // namespace

TEST_CASE("fs: UTF-8 path text round trip", "[platform][fs]") {
    Scratch tmp;
    const std::string name = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xF0\x9F\x98\x80.txt";  // Привет 😀.txt
    const fs::path p = tmp.path() / sys::toPath(name);
    sys::writeFileAtomic(p, "x");
    REQUIRE(fs::exists(p));
    REQUIRE(sys::fromPath(p.filename()) == name);
    REQUIRE(sys::fromGenericPath(sys::toPath("a") / sys::toPath("b")) == "a/b");
}

TEST_CASE("fs: atomic write creates folders, replaces content, leaves no temp files", "[platform][fs]") {
    Scratch tmp;
    const fs::path p = tmp.path() / "a" / "b" / "file.json";
    sys::writeFileAtomic(p, "first");
    sys::writeFileAtomic(p, "second, longer");
    REQUIRE(slurp(p) == "second, longer");
    int files = 0;
    for (const auto& e : fs::directory_iterator(p.parent_path())) {
        (void)e;
        ++files;
    }
    REQUIRE(files == 1);
    REQUIRE_FALSE(sys::readFile(tmp.path() / "missing").has_value());
}

TEST_CASE("fs: whether a folder's names differ by letter case", "[platform][fs]") {
    Scratch tmp;
    sys::writeFileAtomic(tmp.path() / "case.txt", "x");
    // The disk itself is the reference: does CASE.TXT find case.txt?
    std::error_code ec;
    const bool insensitive = fs::exists(tmp.path() / "CASE.TXT", ec);
    CHECK(sys::caseSensitiveFolder(tmp.path()) == !insensitive);
    CHECK(sys::caseSensitiveFolder(tmp.path() / "not" / "yet") == !insensitive);  // a folder a download will make
}

TEST_CASE("fs: fileInfo reports kind, size and times", "[platform][fs]") {
    Scratch tmp;
    const fs::path file = tmp.path() / "f.bin";
    sys::writeFileAtomic(file, std::string(1234, 'z'));
    auto info = sys::fileInfo(file);
    REQUIRE(info.has_value());
    REQUIRE_FALSE(info->isDir);
    REQUIRE_FALSE(info->isLink);
    REQUIRE(info->size == 1234);
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    REQUIRE(info->mtime > now - 600);
    REQUIRE(info->mtime < now + 600);

    auto dir = sys::fileInfo(tmp.path());
    REQUIRE(dir.has_value());
    REQUIRE(dir->isDir);
    REQUIRE_FALSE(sys::fileInfo(tmp.path() / "nope").has_value());
}

TEST_CASE("fs: File writes, resumes, reads and sets times", "[platform][fs]") {
    Scratch tmp;
    const fs::path p = tmp.path() / "data.bin";
    {
        sys::File f;
        REQUIRE(f.open(p, sys::File::Mode::Write));
        REQUIRE(f.isOpen());
        REQUIRE(f.writeAll("hello world", 11));
        REQUIRE(f.close());
        REQUIRE_FALSE(f.isOpen());
    }
    REQUIRE(slurp(p) == "hello world");
    {
        // Resume: keep the first 5 bytes, cut the rest, append.
        sys::File f;
        REQUIRE(f.open(p, sys::File::Mode::WriteKeep));
        REQUIRE(f.seek(5));
        REQUIRE(f.truncate());
        REQUIRE(f.writeAll(", again", 7));
        REQUIRE(f.setTimes(1700000000));
    }
    REQUIRE(slurp(p) == "hello, again");
    auto info = sys::fileInfo(p);
    REQUIRE(info.has_value());
    REQUIRE(info->mtime == 1700000000);
    {
        sys::File f;
        REQUIRE(f.open(p, sys::File::Mode::Read));
        REQUIRE(f.seek(7));
        char buf[64] = {};
        REQUIRE(f.read(buf, sizeof buf) == 5);
        REQUIRE(std::string(buf, 5) == "again");
        REQUIRE(f.read(buf, sizeof buf) == 0);  // end of file
    }
    {
        // Write truncates an existing file; a moved-from File is closed.
        sys::File f;
        REQUIRE(f.open(p, sys::File::Mode::Write));
        sys::File g = std::move(f);
        REQUIRE_FALSE(f.isOpen());
        REQUIRE(g.isOpen());
    }
    REQUIRE(slurp(p).empty());
}

TEST_CASE("fs: File reports errors", "[platform][fs]") {
    Scratch tmp;
    sys::File f;
    REQUIRE_FALSE(f.open(tmp.path() / "missing" / "x", sys::File::Mode::Read));
    REQUIRE_FALSE(f.error().empty());
    REQUIRE_FALSE(f.isOpen());
}

TEST_CASE("fs: renameReplace and removeFile", "[platform][fs]") {
    Scratch tmp;
    const fs::path a = tmp.path() / "a.txt", b = tmp.path() / "b.txt";
    sys::writeFileAtomic(a, "A");
    sys::writeFileAtomic(b, "B");
    std::string error;
    REQUIRE(sys::renameReplace(a, b, error));
    REQUIRE_FALSE(fs::exists(a));
    REQUIRE(slurp(b) == "A");
    REQUIRE(sys::removeFile(b, error));
    REQUIRE_FALSE(fs::exists(b));
    error.clear();
    REQUIRE_FALSE(sys::removeFile(b, error));
    REQUIRE_FALSE(error.empty());
    error.clear();
    REQUIRE_FALSE(sys::renameReplace(a, b, error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("fs: readable now and exclusive writers", "[platform][fs]") {
    Scratch tmp;
    const fs::path p = tmp.path() / "session.log";
    REQUIRE_FALSE(sys::isReadableNow(p));
    std::FILE* first = sys::openExclusiveWriter(p, false);
    REQUIRE(first != nullptr);
    REQUIRE(std::fputs("line\n", first) >= 0);
    REQUIRE(std::fflush(first) == 0);
    // Others may read the log while it is written...
    REQUIRE(sys::isReadableNow(p));
    REQUIRE(slurp(p) == "line\n");
    // ...but a second writer is refused.
    std::FILE* second = sys::openExclusiveWriter(p, true);
    REQUIRE(second == nullptr);
    std::fclose(first);
    second = sys::openExclusiveWriter(p, true);
    REQUIRE(second != nullptr);
    std::fputs("more\n", second);
    std::fclose(second);
    REQUIRE(slurp(p) == "line\nmore\n");
}

TEST_CASE("fs: sanitized file names are usable and stable", "[platform][fs]") {
    Scratch tmp;
    REQUIRE(sys::sanitizeFileName("plain-name_1.txt") == "plain-name_1.txt");
    REQUIRE(sys::sanitizeFileName("\xC3\xA9t\xC3\xA9.txt") == "\xC3\xA9t\xC3\xA9.txt");  // été.txt
    for (const std::string name : {"a/b", "x\x01y", "", ".", "..", "name ", "con", "a:b"}) {
        const std::string s = sys::sanitizeFileName(name);
        INFO(name);
        REQUIRE_FALSE(s.empty());
        REQUIRE(s.find('/') == std::string::npos);
        REQUIRE(s != ".");  // a server's ".." must never climb out of the target folder
        REQUIRE(s != "..");
        REQUIRE(sys::sanitizeFileName(s) == s);  // idempotent
        sys::writeFileAtomic(tmp.path() / sys::toPath(s), "ok");
        REQUIRE(slurp(tmp.path() / sys::toPath(s)) == "ok");
    }
}
