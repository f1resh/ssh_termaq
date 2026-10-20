// Transfer engine: conflict policies and questions, "apply to all", resume in both directions, move, cancellation.
#include "SftpFixture.h"

#include "sftp/SftpUtil.h"
#include "sftp/TransferEngine.h"

#include <catch2/catch_test_macros.hpp>

#include <thread>

using namespace st;
using namespace st::test;
using namespace st::sftpimpl;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

std::string pattern(size_t n, unsigned seed) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 131 + seed) & 0xff);
    return s;
}

}  // namespace

TEST_CASE("transfer: names that differ only in letter case", "[sftp][transfer]") {
    // Where the disk ignores letter case (macOS, Windows) a download keeps both, the later one numbered.
    sftputil::LocalNames folded(false);
    CHECK(folded.claim("README.txt") == "README.txt");
    CHECK(folded.claim("Readme.txt") == "Readme (2).txt");
    CHECK(folded.claim("readme.TXT") == "readme (3).TXT");
    CHECK(folded.claim("Readme (2).txt") == "Readme (2) (2).txt");  // taken by the renamed one
    CHECK(folded.claim("docs") == "docs");
    CHECK(folded.claim("DOCS") == "DOCS (2)");
    sftputil::LocalNames exact(true);
    CHECK(exact.claim("README.txt") == "README.txt");
    CHECK(exact.claim("Readme.txt") == "Readme.txt");
}

TEST_CASE("transfer: conflict decisions", "[sftp][transfer]") {
    using D = ConflictDecision;
    CHECK(decideConflict("overwrite", 100, 200, false, true) == D::Overwrite);
    CHECK(decideConflict("skip", 300, 200, true, true) == D::Skip);
    CHECK(decideConflict("rename", 300, 200, true, true) == D::Rename);
    CHECK(decideConflict("resume", 300, 200, true, true) == D::Resume);
    CHECK(decideConflict("resume", 300, 200, false, true) == D::Overwrite);
    CHECK(decideConflict("newer", 300, 200, false, true) == D::Overwrite);
    CHECK(decideConflict("newer", 200, 300, false, true) == D::Skip);
    CHECK(decideConflict("newer", 201, 200, false, true) == D::Skip);       // within the 2 s tolerance
    CHECK(decideConflict("newer", 250, 200, false, false) == D::Skip);      // minute precision
    CHECK(decideConflict("newer", 0, 200, false, true) == D::Overwrite);    // unknown time

    CHECK(numberedName("report.pdf", 1) == "report (1).pdf");
    CHECK(numberedName("archive.tar.gz", 2) == "archive.tar (2).gz");
    CHECK(numberedName(".bashrc", 1) == ".bashrc (1)");
    CHECK(numberedName("Makefile", 3) == "Makefile (3)");
}

TEST_CASE("transfer: download conflicts", "[sftp][transfer]") {
    SftpFixture f;
    const fs::path r = f.remoteRoot.path(), l = f.local.path();
    spit(r / "a.txt", "remote-a");
    fs::create_directories(r / "dir");
    spit(r / "dir" / "1.txt", "remote-1");
    spit(r / "dir" / "2.txt", "remote-2");
    spit(r / "dir" / "3.txt", "remote-3");

    SECTION("ask -> skip leaves the local file alone") {
        spit(l / "a.txt", "local-a");
        const std::string tid = f.download("/a.txt", l);
        Json c = f.waitConflict();
        CHECK(c["transferId"] == tid);
        CHECK(c["direction"] == "download");
        CHECK(c["name"] == "a.txt");
        CHECK(c["reason"] == "exists");
        CHECK(c["source"]["size"] == 8);
        CHECK(c["target"]["size"] == 7);
        CHECK(c["canResume"] == true);
        CHECK(c["multiple"] == false);
        f.resolve(c, "skip");
        Json p = f.waitDone(tid);
        CHECK(p["state"] == "done");
        CHECK(p["skipped"] == 1);
        CHECK(slurp(l / "a.txt") == "local-a");
    }

    SECTION("apply to all answers once for a whole folder") {
        fs::create_directories(l / "dir");
        for (const char* n : {"1.txt", "2.txt", "3.txt"}) spit(l / "dir" / n, "old");
        const std::string tid = f.download("/dir", l);
        Json c = f.waitConflict();
        CHECK(c["multiple"] == true);
        f.resolve(c, "overwrite", true);
        Json p = f.waitDone(tid);
        CHECK(p["state"] == "done");
        CHECK(p["filesDone"] == 3);
        CHECK(p["filesTotal"] == 3);
        CHECK(f.h.eventsOn("sftp:onConflict").size() == 1);
        CHECK(slurp(l / "dir" / "2.txt") == "remote-2");
    }

    SECTION("rename picks a free name") {
        spit(l / "a.txt", "local-a");
        spit(l / "a (1).txt", "taken");
        const std::string tid = f.download("/a.txt", l, Json{{"conflict", "rename"}});
        CHECK(f.waitDone(tid)["state"] == "done");
        CHECK(slurp(l / "a.txt") == "local-a");
        CHECK(slurp(l / "a (1).txt") == "taken");
        CHECK(slurp(l / "a (2).txt") == "remote-a");
    }

    SECTION("rename with an explicit name") {
        spit(l / "a.txt", "local-a");
        const std::string tid = f.download("/a.txt", l);
        f.resolve(f.waitConflict(), "rename", false, "chosen.txt");
        CHECK(f.waitDone(tid)["state"] == "done");
        CHECK(slurp(l / "chosen.txt") == "remote-a");
        CHECK(slurp(l / "a.txt") == "local-a");
    }

    SECTION("newer only replaces older files") {
        fs::create_directories(l / "dir");
        spit(l / "dir" / "1.txt", "older-local");
        spit(l / "dir" / "2.txt", "newer-local");
        setMtime(r / "dir" / "1.txt", 1700000000);
        setMtime(r / "dir" / "2.txt", 1700000000);
        setMtime(l / "dir" / "1.txt", 1600000000);
        setMtime(l / "dir" / "2.txt", 1800000000);
        const std::string tid = f.download("/dir", l, Json{{"conflict", "newer"}});
        Json p = f.waitDone(tid);
        CHECK(p["state"] == "done");
        CHECK(p["skipped"] == 1);
        CHECK(slurp(l / "dir" / "1.txt") == "remote-1");
        CHECK(slurp(l / "dir" / "2.txt") == "newer-local");
        CHECK(slurp(l / "dir" / "3.txt") == "remote-3");
        CHECK(mtimeOf(l / "dir" / "1.txt") == 1700000000);  // timestamps preserved
        CHECK(f.h.eventsOn("sftp:onConflict").empty());
    }

    SECTION("cancel answer cancels every transfer of the request") {
        spit(l / "a.txt", "local-a");
        fs::create_directories(l / "dir");
        spit(l / "dir" / "1.txt", "old");
        Json ids = f.h.invoke("sftp:download", Json::array({f.id, Json::array({"/a.txt", "/dir"}), u8(l), Json::object()}));
        REQUIRE(ids.size() == 2);
        f.resolve(f.waitConflict(), "cancel");
        CHECK(f.waitDone(ids[0])["state"] == "cancelled");
        CHECK(f.waitDone(ids[1])["state"] == "cancelled");
        CHECK(slurp(l / "a.txt") == "local-a");
    }

    SECTION("cancelling a transfer that waits for an answer") {
        spit(l / "a.txt", "local-a");
        const std::string tid = f.download("/a.txt", l);
        f.waitConflict();
        f.h.send("sftp:cancelTransfer", Json::array({tid}));
        CHECK(f.waitDone(tid, 10s)["state"] == "cancelled");
    }
}

TEST_CASE("transfer: resume", "[sftp][transfer]") {
    SftpFixture f;
    const fs::path r = f.remoteRoot.path(), l = f.local.path();
    const std::string big = pattern(2 * 1024 * 1024 + 77, 7);
    spit(r / "big.bin", big);

    SECTION("an interrupted download continues its partial file") {
        // The partial file deliberately differs from the remote prefix: a resumed download keeps it.
        spit(l / "big.bin.st-partial", std::string(1000, 'X'));
        const std::string tid = f.download("/big.bin", l);
        CHECK(f.waitDone(tid)["state"] == "done");
        const std::string got = slurp(l / "big.bin");
        REQUIRE(got.size() == big.size());
        CHECK(got.substr(0, 1000) == std::string(1000, 'X'));
        CHECK(got.substr(1000) == big.substr(1000));
        CHECK_FALSE(fs::exists(l / "big.bin.st-partial"));
    }

    SECTION("resume answer continues a shorter existing file") {
        spit(l / "big.bin", big.substr(0, 4096));
        const std::string tid = f.download("/big.bin", l);
        Json c = f.waitConflict();
        CHECK(c["canResume"] == true);
        f.resolve(c, "resume");
        CHECK(f.waitDone(tid)["state"] == "done");
        CHECK(slurp(l / "big.bin") == big);
    }

    SECTION("upload resume appends to the server copy") {
        spit(l / "up.bin", big);
        spit(r / "up.bin", big.substr(0, 100000));
        const std::string tid = f.upload(l / "up.bin", "/", Json{{"conflict", "resume"}});
        CHECK(f.waitDone(tid)["state"] == "done");
        CHECK(slurp(r / "up.bin") == big);
    }

    SECTION("upload conflicts ask with server facts") {
        spit(l / "up.bin", "new contents");
        spit(r / "up.bin", "server");
        const std::string tid = f.upload(l / "up.bin", "/");
        Json c = f.waitConflict();
        CHECK(c["direction"] == "upload");
        CHECK(c["target"]["path"] == "/up.bin");
        CHECK(c["target"]["size"] == 6);
        f.resolve(c, "overwrite");
        CHECK(f.waitDone(tid)["state"] == "done");
        CHECK(slurp(r / "up.bin") == "new contents");
    }
}

TEST_CASE("transfer: move", "[sftp][transfer]") {
    SftpFixture f;
    const fs::path r = f.remoteRoot.path(), l = f.local.path();

    SECTION("download move deletes the remote files and folders") {
        fs::create_directories(r / "tree" / "sub");
        spit(r / "tree" / "x.txt", "x");
        spit(r / "tree" / "sub" / "y.txt", "y");
        const std::string tid = f.download("/tree", l, Json{{"move", true}});
        Json p = f.waitDone(tid);
        CHECK(p["state"] == "done");
        CHECK(p["move"] == true);
        CHECK(slurp(l / "tree" / "sub" / "y.txt") == "y");
        CHECK_FALSE(fs::exists(r / "tree"));
    }

    SECTION("upload move deletes the local files and folders") {
        fs::create_directories(l / "tree" / "sub");
        spit(l / "tree" / "x.txt", "x");
        spit(l / "tree" / "sub" / "y.txt", "y");
        const std::string tid = f.upload(l / "tree", "/", Json{{"move", true}});
        CHECK(f.waitDone(tid)["state"] == "done");
        CHECK(slurp(r / "tree" / "sub" / "y.txt") == "y");
        CHECK_FALSE(fs::exists(l / "tree"));
    }

    SECTION("skipped files stay at the source") {
        spit(r / "keep.txt", "remote");
        spit(l / "keep.txt", "local");
        const std::string tid = f.download("/keep.txt", l, Json{{"move", true}, {"conflict", "skip"}});
        CHECK(f.waitDone(tid)["skipped"] == 1);
        CHECK(fs::exists(r / "keep.txt"));
    }
}

TEST_CASE("transfer: uploads merge into existing folders", "[sftp][transfer]") {
    SftpFixture f;
    const fs::path r = f.remoteRoot.path(), l = f.local.path();
    fs::create_directories(r / "site");
    spit(r / "site" / "old.html", "old");
    fs::create_directories(l / "site");
    spit(l / "site" / "new.html", "new");
    const std::string tid = f.upload(l / "site", "/");
    CHECK(f.waitDone(tid)["state"] == "done");
    CHECK(slurp(r / "site" / "old.html") == "old");
    CHECK(slurp(r / "site" / "new.html") == "new");
    CHECK(f.h.eventsOn("sftp:onConflict").empty());

    // A file where a folder should go is an error, not a merge.
    spit(r / "clash", "file");
    fs::create_directories(l / "clash");
    spit(l / "clash" / "z.txt", "z");
    Json p = f.waitDone(f.upload(l / "clash", "/"));
    CHECK(p["state"] == "error");
    CHECK_FALSE(p["error"].get<std::string>().empty());
}

TEST_CASE("transfer: folder transfers report the listing before copying", "[sftp][transfer]") {
    SftpFixture f;
    const fs::path r = f.remoteRoot.path(), l = f.local.path();
    for (int d = 0; d < 20; ++d) {
        const std::string dir = "d" + std::to_string(d);
        fs::create_directories(r / "tree" / dir / "sub");
        spit(r / "tree" / dir / "a.txt", "aaaa");
        spit(r / "tree" / dir / "sub" / "b.txt", "bb");
    }

    // The progress events of one transfer, in order: listing (counting up, nothing copied yet), then the copy.
    auto check = [&](const std::string& tid, int files, double bytes) {
        std::vector<Json> events;
        for (const auto& e : f.h.eventsOn("sftp:onProgress"))
            if (e["args"][0]["transferId"] == tid) events.push_back(e["args"][0]);
        REQUIRE_FALSE(events.empty());
        size_t lastListing = 0, listed = 0;
        int found = 0;
        for (size_t i = 0; i < events.size(); ++i) {
            if (!events[i].value("listing", false)) continue;
            ++listed;
            lastListing = i;
            CHECK(events[i]["transferred"] == 0);
            CHECK(events[i]["filesTotal"].get<int>() >= found);
            found = events[i]["filesTotal"].get<int>();
        }
        CHECK(listed >= 1);
        for (size_t i = 0; i < events.size(); ++i)
            if (events[i]["transferred"].get<double>() > 0) CHECK(i > lastListing);
        const Json& last = events.back();
        CHECK(last["state"] == "done");
        CHECK_FALSE(last.value("listing", false));
        CHECK(last["filesTotal"] == files);
        CHECK(last["total"] == bytes);
    };

    check(f.waitDone(f.download("/tree", l))["transferId"], 40, 20 * 6);
    CHECK(slurp(l / "tree" / "d7" / "sub" / "b.txt") == "bb");
    fs::create_directories(r / "up");
    check(f.waitDone(f.upload(l / "tree", "/up"))["transferId"], 40, 20 * 6);
    CHECK(slurp(r / "up" / "tree" / "d3" / "a.txt") == "aaaa");
}

TEST_CASE("transfer: folder uploads can leave out macOS Finder's files", "[sftp][transfer]") {
    SftpFixture f;
    const fs::path r = f.remoteRoot.path(), l = f.local.path();
    fs::create_directories(l / "site" / "img");
    spit(l / "site" / "index.html", "i");
    spit(l / "site" / ".DS_Store", "finder");
    spit(l / "site" / "img" / ".DS_Store", "finder");
    spit(l / "site" / "img" / "._logo.png", "fork");
    spit(l / "site" / "img" / "logo.png", "png");
    spit(l / "site" / ".htaccess", "kept: other dot files are not Finder's");

    fs::create_directories(r / "skip");
    Json p = f.waitDone(f.upload(l / "site", "/skip", Json{{"skipMacFiles", true}}));
    CHECK(p["state"] == "done");
    CHECK(p["filesTotal"] == 3);
    CHECK(fs::exists(r / "skip" / "site" / "index.html"));
    CHECK(fs::exists(r / "skip" / "site" / "img" / "logo.png"));
    CHECK(fs::exists(r / "skip" / "site" / ".htaccess"));
    CHECK_FALSE(fs::exists(r / "skip" / "site" / ".DS_Store"));
    CHECK_FALSE(fs::exists(r / "skip" / "site" / "img" / ".DS_Store"));
    CHECK_FALSE(fs::exists(r / "skip" / "site" / "img" / "._logo.png"));

    // Off (the default): everything goes, as before.
    fs::create_directories(r / "all");
    CHECK(f.waitDone(f.upload(l / "site", "/all"))["filesTotal"] == 6);
    CHECK(fs::exists(r / "all" / "site" / "img" / "._logo.png"));
}
