// Edit: download to a temp copy, "launch the editor" (a test hook), upload every saved change, ask when the server
// copy changed meanwhile, stop watching on request.
#include "SftpFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <mutex>
#include <thread>

using namespace st;
using namespace st::test;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

/** Count of onEdit events with the given state for an edit id. */
size_t editEvents(SftpFixture& f, const std::string& editId, const std::string& state) {
    size_t n = 0;
    for (const auto& e : f.h.eventsOn("sftp:onEdit"))
        if (e["args"][0]["editId"] == editId && e["args"][0]["state"] == state) ++n;
    return n;
}

bool waitEditState(SftpFixture& f, const std::string& editId, const std::string& state, size_t count,
                   std::chrono::milliseconds timeout = 15s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (editEvents(f, editId, state) >= count) return true;
        std::this_thread::sleep_for(50ms);
    }
    return false;
}

}  // namespace

TEST_CASE("edit: saved changes are uploaded", "[edit]") {
    SftpFixture f;
    const fs::path r = f.remoteRoot.path();
    spit(r / "config.ini", "a=1\n");
    setMtime(r / "config.ini", 1700000000);

    std::mutex lm;
    std::vector<std::pair<std::string, std::string>> launched;
    f.sftp->setEditLauncherForTests([&](const std::string& path, const std::string& editor) {
        std::lock_guard lk(lm);
        launched.emplace_back(path, editor);
    });

    const std::string editId = f.h.invoke("sftp:edit", Json::array({f.id, "/config.ini", Json{{"editor", "C:\\Tools\\ed.exe"}}}));
    REQUIRE(waitEditState(f, editId, "opened", 1));
    fs::path local;
    {
        std::lock_guard lk(lm);
        REQUIRE(launched.size() == 1);
        CHECK(launched[0].second == "C:\\Tools\\ed.exe");
        local = widen(launched[0].first);
    }
    CHECK(slurp(local) == "a=1\n");
    CHECK(local.filename() == L"config.ini");

    // First save
    spit(local, "a=2\n");
    REQUIRE(waitEditState(f, editId, "uploaded", 1));
    CHECK(slurp(r / "config.ini") == "a=2\n");
    CHECK(editEvents(f, editId, "uploading") >= 1);

    // Second save
    std::this_thread::sleep_for(1100ms);  // a different mtime second on the server
    spit(local, "a=3\n");
    REQUIRE(waitEditState(f, editId, "uploaded", 2));
    CHECK(slurp(r / "config.ini") == "a=3\n");

    SECTION("a server copy changed meanwhile asks first") {
        spit(r / "config.ini", "changed by someone else\n");
        setMtime(r / "config.ini", 1800000000);
        spit(local, "a=4\n");
        Json c = f.waitConflict();
        CHECK(c["reason"] == "changed");
        CHECK(c["direction"] == "upload");
        SECTION("keep the server copy") {
            f.resolve(c, "skip");
            REQUIRE(waitEditState(f, editId, "error", 1));
            CHECK(slurp(r / "config.ini") == "changed by someone else\n");
        }
        SECTION("overwrite it") {
            f.resolve(c, "overwrite");
            REQUIRE(waitEditState(f, editId, "uploaded", 3));
            CHECK(slurp(r / "config.ini") == "a=4\n");
        }
    }

    SECTION("stop watching") {
        f.h.send("sftp:stopEdit", Json::array({editId}));
        REQUIRE(waitEditState(f, editId, "closed", 1));
        spit(local, "ignored\n");
        std::this_thread::sleep_for(2000ms);
        CHECK(slurp(r / "config.ini") == "a=3\n");
    }
}

TEST_CASE("edit: missing files fail cleanly", "[edit]") {
    SftpFixture f;
    f.sftp->setEditLauncherForTests([](const std::string&, const std::string&) { FAIL("must not launch"); });
    CHECK_FALSE(f.h.invokeError("sftp:edit", Json::array({f.id, "/nope.txt", Json::object()})).empty());
    CHECK(f.h.eventsOn("sftp:onEdit").empty());
}
