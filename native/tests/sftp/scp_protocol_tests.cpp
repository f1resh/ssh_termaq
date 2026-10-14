// SCP backend pure helpers: control records, shell quoting, `ls -l` parsing across ls flavours.
#include "sftp/ScpProtocol.h"

#include <catch2/catch_test_macros.hpp>

using namespace st::scp;

TEST_CASE("scp: control records", "[scp]") {
    std::string err;
    auto c = parseRecord("C0644 1234 my file.txt", err);
    REQUIRE(c);
    CHECK(c->type == 'C');
    CHECK(c->mode == 0644);
    CHECK(c->size == 1234);
    CHECK(c->name == "my file.txt");

    auto d = parseRecord("D0755 0 folder", err);
    REQUIRE(d);
    CHECK(d->type == 'D');
    CHECK(d->mode == 0755);

    auto t = parseRecord("T1700000000 0 1700000100 0", err);
    REQUIRE(t);
    CHECK(t->mtime == 1700000000);
    CHECK(t->atime == 1700000100);

    CHECK(parseRecord("E", err)->type == 'E');
    CHECK_FALSE(parseRecord("C0644 12", err));
    CHECK_FALSE(parseRecord("C0644 x name", err));
    CHECK_FALSE(parseRecord("C0644 1 ../evil", err));
    CHECK_FALSE(parseRecord("C0644 1 a/b", err));
    CHECK_FALSE(parseRecord("C0644 1 ..", err));
    CHECK_FALSE(parseRecord("T1 0 2", err));
    CHECK_FALSE(parseRecord("X", err));
    CHECK_FALSE(parseRecord("", err));

    CHECK(fileRecord(0100644, 12, "a b.txt") == "C0644 12 a b.txt\n");
    CHECK(timeRecord(5, 6) == "T5 0 6 0\n");
}

TEST_CASE("scp: shell quoting", "[scp]") {
    CHECK(shellQuote("plain") == "'plain'");
    CHECK(shellQuote("it's") == "'it'\\''s'");
    CHECK(shellQuote("$HOME `x` \"q\"") == "'$HOME `x` \"q\"'");
    CHECK(shellQuote("") == "''");
}

TEST_CASE("scp: permissions, offsets, dates", "[scp]") {
    CHECK(modeFromPermissions("-rw-r--r--") == 0100644);
    CHECK(modeFromPermissions("drwxr-xr-x") == 040755);
    CHECK(modeFromPermissions("lrwxrwxrwx") == 0120777);
    CHECK(modeFromPermissions("-rwsr-xr-x") == 0104755);
    CHECK(modeFromPermissions("-rwSr--r--") == 0104644);
    CHECK(modeFromPermissions("drwxrwsr-x") == 042775);
    CHECK(modeFromPermissions("drwxrwxrwt") == 041777);
    CHECK(modeFromPermissions("drwxrwxrwT") == 041776);
    CHECK(modeFromPermissions("-rw-r--r--+") == 0100644);
    CHECK(modeFromPermissions("crw-rw-rw-") == 020666);

    CHECK(parseTzOffset("+0100") == 3600);
    CHECK(parseTzOffset("-0530\n") == -(5 * 3600 + 30 * 60));
    CHECK(parseTzOffset("+0000") == 0);
    CHECK(parseTzOffset("UTC") == 0);

    CHECK(civilToUnix(1970, 1, 1, 0, 0, 0) == 0);
    CHECK(civilToUnix(2024, 1, 31, 12, 34, 56) == 1706704496);
    CHECK(civilToUnix(2000, 2, 29, 0, 0, 0) == 951782400);
}

TEST_CASE("scp: ls -l parsing", "[scp]") {
    const int64_t now = civilToUnix(2024, 6, 15, 12, 0, 0);

    SECTION("GNU --full-time with zone") {
        auto e = parseLsLine("-rw-r--r-- 1 alice staff 1234 2024-01-31 12:34:56.123456789 +0100 report final.pdf", 0, now);
        REQUIRE(e);
        CHECK(e->type == '-');
        CHECK(e->mode == 0100644);
        CHECK(e->size == 1234);
        CHECK(e->owner == "alice");
        CHECK(e->group == "staff");
        CHECK(e->name == "report final.pdf");
        CHECK(e->mtime == civilToUnix(2024, 1, 31, 11, 34, 56));  // +0100 -> UTC
        CHECK(e->preciseTime);
    }

    SECTION("names keep their spacing; links split at the arrow") {
        auto e = parseLsLine("lrwxrwxrwx 1 root root 7 2024-01-01 00:00:00.000000000 +0000 two  spaces -> /etc/target dir", 0, now);
        REQUIRE(e);
        CHECK(e->type == 'l');
        CHECK(e->name == "two  spaces");
        CHECK(e->linkTarget == "/etc/target dir");
    }

    SECTION("BSD -T (server zone from date +%z)") {
        auto e = parseLsLine("drwxr-xr-x  5 bob  wheel  160 Jan 31 12:34:56 2024 Projects", -18000, now);
        REQUIRE(e);
        CHECK(e->type == 'd');
        CHECK(e->name == "Projects");
        CHECK(e->mtime == civilToUnix(2024, 1, 31, 17, 34, 56));
        CHECK(e->preciseTime);
    }

    SECTION("BusyBox -e") {
        auto e = parseLsLine("-rwxr-xr-x    1 root     root          512 Wed Jan 31 12:34:56 2024 init.sh", 0, now);
        REQUIRE(e);
        CHECK(e->size == 512);
        CHECK(e->name == "init.sh");
        CHECK(e->mtime == civilToUnix(2024, 1, 31, 12, 34, 56));
        CHECK(e->preciseTime);
    }

    SECTION("plain: recent files have a time, old ones a year") {
        auto recent = parseLsLine("-rw-r--r-- 1 u g 10 Mar  3 09:15 notes.txt", 0, now);
        REQUIRE(recent);
        CHECK(recent->mtime == civilToUnix(2024, 3, 3, 9, 15, 0));
        CHECK_FALSE(recent->preciseTime);
        auto lastYear = parseLsLine("-rw-r--r-- 1 u g 10 Dec 24 18:00 gift.txt", 0, now);  // would be in the future
        REQUIRE(lastYear);
        CHECK(lastYear->mtime == civilToUnix(2023, 12, 24, 18, 0, 0));
        auto old = parseLsLine("-rw-r--r-- 1 u g 10 Jul  4  2019 old.txt", 0, now);
        REQUIRE(old);
        CHECK(old->mtime == civilToUnix(2019, 7, 4, 0, 0, 0));
        CHECK(old->name == "old.txt");
    }

    SECTION("device files, ACL markers, missing group") {
        auto dev = parseLsLine("crw-rw-rw- 1 root root 1, 3 2024-01-01 00:00:00.000000000 +0000 null", 0, now);
        REQUIRE(dev);
        CHECK(dev->type == 'c');
        CHECK(dev->size == 0);
        CHECK(dev->name == "null");
        auto acl = parseLsLine("-rw-r--r--+ 1 u g 5 2024-01-01 00:00:00.000000000 +0000 acl.txt", 0, now);
        REQUIRE(acl);
        CHECK(acl->name == "acl.txt");
        auto noGroup = parseLsLine("-rw-r--r-- 1 u 5 2024-01-01 00:00:00.000000000 +0000 x", 0, now);
        REQUIRE(noGroup);
        CHECK(noGroup->size == 5);
        CHECK(noGroup->name == "x");
    }

    SECTION("noise is ignored") {
        CHECK_FALSE(parseLsLine("total 48", 0, now));
        CHECK_FALSE(parseLsLine("", 0, now));
        CHECK_FALSE(parseLsLine("ls: cannot access 'x': No such file or directory", 0, now));
        CHECK(parseLsLine("-rw-r--r-- 1 u g 1 2024-01-01 00:00:00.000000000 +0000 crlf.txt\r", 0, now)->name == "crlf.txt");
    }
}
