// SCP backend end-to-end: the test server refuses SFTP and runs exec requests through Git for Windows' sh.exe, so
// browsing uses real GNU ls / mkdir / mv / rm and transfers talk to OpenSSH's own `scp -t` / `scp -f`.
#include "SftpFixture.h"

#include <catch2/catch_test_macros.hpp>

using namespace st;
using namespace st::test;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

TestSshServerOptions scpServer() {
    TestSshServerOptions o;
    o.enableSftp = false;
    o.execShell = gitShell();
    return o;
}

const Json* findEntry(const Json& list, const std::string& name) {
    for (const auto& e : list)
        if (e["name"] == name) return &e;
    return nullptr;
}

std::string pattern(size_t n) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 7 + 3) & 0xff);
    return s;
}

}  // namespace

TEST_CASE("scp: browse and modify through shell commands", "[scp]") {
    if (gitShell().empty()) SKIP("Git for Windows (sh.exe, ls.exe, scp.exe) is not installed");
    SftpFixture f(scpServer());
    REQUIRE(f.opened["protocol"] == "scp");
    CHECK(f.opened["capabilities"]["resume"] == false);
    CHECK(f.opened["capabilities"]["preciseTimes"] == true);  // GNU ls --full-time
    const std::string home = f.opened["home"];
    REQUIRE_FALSE(home.empty());
    REQUIRE(home[0] == '/');

    const fs::path r = f.remoteRoot.path();
    fs::create_directories(r / "docs");
    spit(r / "docs" / "readme.txt", "hello scp");
    spit(r / "with space.txt", "s");
    spit(r / "it's quoted.txt", "q");
    spit(r / widen("Ünïcødé.txt"), "u");
    setMtime(r / "with space.txt", 1700000000);

    Json list = f.h.invoke("sftp:list", Json::array({f.id, home}));
    REQUIRE(list.size() == 4);
    const Json* docs = findEntry(list, "docs");
    REQUIRE(docs);
    CHECK((*docs)["type"] == "dir");
    CHECK((*docs)["path"] == home + "/docs");
    CHECK((*docs)["permissions"].get<std::string>()[0] == 'd');
    const Json* spaced = findEntry(list, "with space.txt");
    REQUIRE(spaced);
    CHECK((*spaced)["type"] == "file");
    CHECK((*spaced)["size"] == 1);
    CHECK((*spaced)["mtime"] == 1700000000LL * 1000);
    CHECK(findEntry(list, "it's quoted.txt"));
    CHECK(findEntry(list, "Ünïcødé.txt"));

    // stat
    Json st = f.h.invoke("sftp:stat", Json::array({f.id, home + "/docs/readme.txt"}));
    CHECK(st["name"] == "readme.txt");
    CHECK(st["size"] == 9);
    CHECK(f.h.invoke("sftp:stat", Json::array({f.id, home + "/nope"})).is_null());
    CHECK(f.h.invoke("sftp:realpath", Json::array({f.id, home + "/docs/.."})) == home);

    // modify
    f.h.invoke("sftp:mkdir", Json::array({f.id, home + "/made dir"}));
    CHECK(fs::is_directory(r / "made dir"));
    f.h.invoke("sftp:rename", Json::array({f.id, home + "/it's quoted.txt", home + "/made dir/renamed.txt"}));
    CHECK(slurp(r / "made dir" / "renamed.txt") == "q");
    CHECK_FALSE(f.h.invokeError("sftp:rename", Json::array({f.id, home + "/with space.txt", home + "/docs"})).empty());
    f.h.invoke("sftp:chmod", Json::array({f.id, home + "/docs/readme.txt", 0600}));
    CHECK_FALSE(f.h.invokeError("sftp:remove", Json::array({f.id, home + "/made dir", false})).empty());  // not empty
    f.h.invoke("sftp:remove", Json::array({f.id, home + "/made dir", true}));
    CHECK_FALSE(fs::exists(r / "made dir"));
    f.h.invoke("sftp:remove", Json::array({f.id, home + "/with space.txt", false}));
    CHECK_FALSE(fs::exists(r / "with space.txt"));
    CHECK_FALSE(f.h.invokeError("sftp:list", Json::array({f.id, home + "/missing"})).empty());
}

TEST_CASE("scp: transfers use the scp protocol", "[scp]") {
    if (gitShell().empty()) SKIP("Git for Windows (sh.exe, ls.exe, scp.exe) is not installed");
    SftpFixture f(scpServer());
    REQUIRE(f.opened["protocol"] == "scp");
    const std::string home = f.opened["home"];
    const fs::path r = f.remoteRoot.path(), l = f.local.path();
    const std::string big = pattern(3 * 1024 * 1024 + 11);

    SECTION("single files both ways, times preserved") {
        spit(l / "big.bin", big);
        setMtime(l / "big.bin", 1650000000);
        CHECK(f.waitDone(f.upload(l / "big.bin", home))["state"] == "done");
        CHECK(slurp(r / "big.bin") == big);
        CHECK(mtimeOf(r / "big.bin") == 1650000000);

        fs::create_directories(l / "back");
        Json p = f.waitDone(f.download(home + "/big.bin", l / "back"));
        CHECK(p["state"] == "done");
        CHECK(p["transferred"] == static_cast<double>(big.size()));
        CHECK(slurp(l / "back" / "big.bin") == big);
        CHECK(mtimeOf(l / "back" / "big.bin") == 1650000000);
    }

    SECTION("folders recurse with listing + one channel per file") {
        fs::create_directories(l / "tree" / "sub dir");
        spit(l / "tree" / "a.txt", "a");
        spit(l / "tree" / "sub dir" / "b c.txt", "bc");
        spit(l / "tree" / "empty.txt", "");
        Json up = f.waitDone(f.upload(l / "tree", home));
        CHECK(up["state"] == "done");
        CHECK(up["filesDone"] == 3);
        CHECK(slurp(r / "tree" / "sub dir" / "b c.txt") == "bc");
        CHECK(fs::exists(r / "tree" / "empty.txt"));

        Json down = f.waitDone(f.download(home + "/tree", l / "copy"));
        CHECK(down["state"] == "done");
        CHECK(slurp(l / "copy" / "tree" / "sub dir" / "b c.txt") == "bc");
        CHECK(slurp(l / "copy" / "tree" / "a.txt") == "a");
    }

    SECTION("conflicts work, resume is never offered") {
        spit(r / "x.txt", "remote version");
        spit(l / "x.txt", "local");
        const std::string tid = f.download(home + "/x.txt", l);
        Json c = f.waitConflict();
        CHECK(c["canResume"] == false);
        f.resolve(c, "overwrite");
        CHECK(f.waitDone(tid)["state"] == "done");
        CHECK(slurp(l / "x.txt") == "remote version");
        CHECK_FALSE(fs::exists(l / "x.txt.st-partial"));
    }

    SECTION("errors from the remote scp are reported") {
        Json p = f.waitDone(f.download(home + "/does-not-exist.bin", l));
        CHECK(p["state"] == "error");
        CHECK_FALSE(p["error"].get<std::string>().empty());
        CHECK_FALSE(fs::exists(l / "does-not-exist.bin"));
    }

    SECTION("cancel stops a running upload") {
        spit(l / "huge.bin", std::string(40 * 1024 * 1024, 'z'));
        const std::string tid = f.upload(l / "huge.bin", home);
        REQUIRE(f.h.waitEvent([&](const Json& e) {
            return e["ch"] == "sftp:onProgress" && e["args"][0]["transferId"] == tid && e["args"][0]["transferred"].get<double>() > 0;
        }));
        f.h.send("sftp:cancelTransfer", Json::array({tid}));
        CHECK(f.waitDone(tid)["state"] == "cancelled");
    }
}

TEST_CASE("scp: protocol selection", "[scp]") {
    if (gitShell().empty()) SKIP("Git for Windows (sh.exe, ls.exe, scp.exe) is not installed");

    SECTION("auto falls back, explicit sftp fails") {
        TestSshServerOptions o = scpServer();
        SftpFixture f(o, Json{{"fileProtocol", "auto"}});
        CHECK(f.opened["protocol"] == "scp");
        const std::string err = f.h.invokeError("sftp:open", Json::array({f.connId, Json{{"fileProtocol", "sftp"}}}));
        CHECK_FALSE(err.empty());
    }

    SECTION("scp can be chosen even when SFTP exists") {
        TestSshServerOptions o;
        o.execShell = gitShell();
        SftpFixture f(o, Json{{"fileProtocol", "scp"}});
        CHECK(f.opened["protocol"] == "scp");
    }

    SECTION("neither protocol: a clear error") {
        TestSshServerOptions o;
        o.enableSftp = false;  // and exec only knows echo / exit
        SftpFixture f(o, nullptr);
        const std::string err = f.h.invokeError("sftp:open", Json::array({f.connId, Json::object()}));
        CHECK(err.find("neither SFTP nor SCP") != std::string::npos);
    }
}
