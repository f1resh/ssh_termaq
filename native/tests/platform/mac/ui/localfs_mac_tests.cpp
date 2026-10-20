// Local files on macOS (platform/ui/LocalFs) and the local:* bridge handlers: path helpers, listings, file operations
// (no UI, so existing items are replaced; permanent deletes only, so the Trash is never touched) and folder watching.
#include "TestBridge.h"
#include "TestServers.h"

#include "localfs/LocalFsService.h"
#include "platform/sys/Fs.h"
#include "platform/ui/LocalFs.h"

#include <catch2/catch_test_macros.hpp>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

using namespace st;
using namespace st::test;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

std::string u8(const fs::path& p) { return sys::fromPath(p); }

const LocalEntry* find(const std::vector<LocalEntry>& list, const std::string& name) {
    auto it = std::find_if(list.begin(), list.end(), [&](const LocalEntry& e) { return e.name == name; });
    return it == list.end() ? nullptr : &*it;
}

ui::localfs::ShellOpOptions quiet() {
    ui::localfs::ShellOpOptions o;
    o.showUi = false;
    return o;
}

template <class Pred>
bool waitFor(Pred pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return pred();
}

}  // namespace

TEST_CASE("localfs: path normalisation", "[localfs]") {
    const std::string home = std::getenv("HOME");
    CHECK(ui::localfs::normalizePath("/") == "/");
    CHECK(ui::localfs::normalizePath("/Users/") == "/Users");
    CHECK(ui::localfs::normalizePath("/a//b///c/") == "/a/b/c");
    CHECK(ui::localfs::normalizePath("  /x  ") == "/x");
    CHECK(ui::localfs::normalizePath("~") == home);
    CHECK(ui::localfs::normalizePath("~/x/") == home + "/x");
    CHECK(ui::localfs::normalizePath("~other") == "~other");
    CHECK(ui::localfs::joinPath("/", "x") == "/x");
    CHECK(ui::localfs::joinPath("/a", "x") == "/a/x");
}

TEST_CASE("localfs: list and stat", "[localfs]") {
    TempDir tmp;
    const fs::path root = tmp.path();
    fs::create_directories(root / "sub");
    spit(root / "a.txt", "hello");
    spit(root / sys::toPath("Привет мир 😀.txt"), "unicode");
    spit(root / ".dotfile", "d");
    spit(root / "flagged.txt", "f");
    REQUIRE(::chflags(u8(root / "flagged.txt").c_str(), UF_HIDDEN) == 0);  // Finder's hidden flag
    spit(root / "locked.txt", "l");
    ::chmod(u8(root / "locked.txt").c_str(), 0444);
    fs::create_symlink("a.txt", root / "file-link");
    fs::create_symlink("sub", root / "dir-link");
    fs::create_symlink("missing", root / "broken-link");

    auto list = ui::localfs::list(u8(root));
    REQUIRE(list.size() == 9);
    const LocalEntry* a = find(list, "a.txt");
    REQUIRE(a);
    CHECK(a->type == "file");
    CHECK(a->size == 5);
    CHECK(a->path == u8(root / "a.txt"));
    CHECK(a->mtime > 1600000000000LL);
    CHECK_FALSE(a->hidden);
    CHECK_FALSE(a->readonly);
    CHECK_FALSE(a->link);
    REQUIRE(find(list, "sub"));
    CHECK(find(list, "sub")->type == "dir");
    CHECK(find(list, "sub")->size == 0);
    REQUIRE(find(list, "Привет мир 😀.txt"));
    CHECK(find(list, ".dotfile")->hidden);
    CHECK(find(list, "flagged.txt")->hidden);
    CHECK(find(list, "locked.txt")->readonly);

    // Links describe their targets.
    const LocalEntry* fileLink = find(list, "file-link");
    REQUIRE(fileLink);
    CHECK(fileLink->link);
    CHECK(fileLink->type == "file");
    CHECK(fileLink->size == 5);
    CHECK(find(list, "dir-link")->link);
    CHECK(find(list, "dir-link")->type == "dir");
    CHECK(find(list, "broken-link")->link);
    CHECK(find(list, "broken-link")->type == "file");

    // stat
    auto st = ui::localfs::stat(u8(root / "a.txt"));
    REQUIRE(st);
    CHECK(st->name == "a.txt");
    CHECK(st->size == 5);
    CHECK(ui::localfs::stat(u8(root / "dir-link"))->type == "dir");
    CHECK_FALSE(ui::localfs::stat(u8(root / "missing.bin")));
    CHECK_FALSE(ui::localfs::stat(u8(root / "missing-dir" / "x")));
    CHECK_FALSE(ui::localfs::stat(u8(root / "a.txt" / "x")));  // under a file
    auto rootStat = ui::localfs::stat("/");
    REQUIRE(rootStat);
    CHECK(rootStat->type == "dir");
    CHECK(rootStat->name == "/");

    REQUIRE_THROWS(ui::localfs::list(u8(root / "missing")));
    REQUIRE_THROWS(ui::localfs::list(u8(root / "a.txt")));
}

TEST_CASE("localfs: make folders and rename", "[localfs]") {
    TempDir tmp;
    const fs::path root = tmp.path();
    spit(root / "a.txt", "hello");
    spit(root / "other.txt", "other");

    ui::localfs::makeDir(u8(root / "made"));
    CHECK(fs::is_directory(root / "made"));
    REQUIRE_THROWS(ui::localfs::makeDir(u8(root / "made")));
    ui::localfs::rename(u8(root / "a.txt"), u8(root / "made" / "b.txt"));
    CHECK(slurp(root / "made" / "b.txt") == "hello");
    REQUIRE_THROWS(ui::localfs::rename(u8(root / "a.txt"), u8(root / "c.txt")));  // source is gone

    // Never onto an existing item.
    REQUIRE_THROWS(ui::localfs::rename(u8(root / "other.txt"), u8(root / "made" / "b.txt")));
    CHECK(slurp(root / "made" / "b.txt") == "hello");
    CHECK(slurp(root / "other.txt") == "other");

    // Only the letter case (the same item on a case-insensitive volume).
    ui::localfs::rename(u8(root / "other.txt"), u8(root / "Other.txt"));
    auto list = ui::localfs::list(u8(root));
    CHECK(find(list, "Other.txt"));
    CHECK_FALSE(find(list, "other.txt"));
}

TEST_CASE("localfs: copy, move and delete", "[localfs]") {
    TempDir tmp;
    const fs::path root = tmp.path();
    fs::create_directories(root / "made");
    fs::create_directories(root / "sub" / "deep");
    fs::create_directories(root / "target");
    spit(root / "made" / "b.txt", "hello");
    spit(root / "sub" / "deep" / "s.txt", "s");
    spit(root / ".hidden", "h");
    fs::create_symlink("b.txt", root / "made" / "link");

    ui::localfs::copy({u8(root / "made"), u8(root / ".hidden")}, u8(root / "target"), false, quiet());
    CHECK(slurp(root / "target" / "made" / "b.txt") == "hello");
    CHECK(fs::is_symlink(root / "target" / "made" / "link"));  // links stay links
    CHECK(fs::exists(root / "target" / ".hidden"));
    CHECK(fs::exists(root / "made" / "b.txt"));
    ui::localfs::copy({u8(root / "sub")}, u8(root / "target"), true, quiet());
    CHECK(slurp(root / "target" / "sub" / "deep" / "s.txt") == "s");
    CHECK_FALSE(fs::exists(root / "sub"));

    SECTION("existing items are replaced as a whole") {
        spit(root / "made" / "b.txt", "new");
        spit(root / "target" / "made" / "extra.txt", "old only");
        ui::localfs::copy({u8(root / "made")}, u8(root / "target"), false, quiet());
        CHECK(slurp(root / "target" / "made" / "b.txt") == "new");
        CHECK_FALSE(fs::exists(root / "target" / "made" / "extra.txt"));

        spit(root / "made" / "b.txt", "moved");
        ui::localfs::copy({u8(root / "made" / "b.txt")}, u8(root / "target" / "made"), true, quiet());
        CHECK(slurp(root / "target" / "made" / "b.txt") == "moved");
        CHECK_FALSE(fs::exists(root / "made" / "b.txt"));

        // A file replacing a folder; nothing is left behind.
        spit(root / "sub", "a file now");
        ui::localfs::copy({u8(root / "sub")}, u8(root / "target"), false, quiet());
        CHECK(slurp(root / "target" / "sub") == "a file now");
        auto names = ui::localfs::list(u8(root / "target"));
        CHECK(names.size() == 3);  // made, sub, .hidden
    }

    SECTION("copies into the same folder get Finder's names") {
        ui::localfs::copy({u8(root / "made" / "b.txt")}, u8(root / "made"), false, quiet());
        ui::localfs::copy({u8(root / "made" / "b.txt")}, u8(root / "made"), false, quiet());
        CHECK(slurp(root / "made" / "b copy.txt") == "hello");
        CHECK(slurp(root / "made" / "b copy 2.txt") == "hello");
        ui::localfs::copy({u8(root / "made")}, u8(root), false, quiet());
        CHECK(fs::is_directory(root / "made copy"));
        // Moving an item to where it is changes nothing.
        ui::localfs::copy({u8(root / "made" / "b.txt")}, u8(root / "made"), true, quiet());
        CHECK(slurp(root / "made" / "b.txt") == "hello");
    }

    SECTION("a folder never goes into itself") {
        REQUIRE_THROWS(ui::localfs::copy({u8(root / "target")}, u8(root / "target" / "made"), false, quiet()));
        REQUIRE_THROWS(ui::localfs::copy({u8(root / "target")}, u8(root / "target"), true, quiet()));
        // Nor replaces a folder that holds it.
        fs::create_directories(root / "target" / "target");
        REQUIRE_THROWS(ui::localfs::copy({u8(root / "target" / "target")}, u8(root), true, quiet()));
        CHECK(fs::is_directory(root / "target" / "made"));
    }

    SECTION("errors") {
        REQUIRE_THROWS(ui::localfs::copy({u8(root / "missing")}, u8(root / "target"), false, quiet()));
        REQUIRE_THROWS(ui::localfs::copy({u8(root / "made")}, u8(root / "missing"), false, quiet()));
        REQUIRE_THROWS(ui::localfs::copy({u8(root / "made")}, u8(root / ".hidden"), false, quiet()));
        REQUIRE_THROWS(ui::localfs::remove({u8(root / "missing")}, false, quiet()));
    }

    // permanent delete (recursive)
    ui::localfs::remove({u8(root / "target"), u8(root / "made")}, false, quiet());
    CHECK_FALSE(fs::exists(root / "target"));
    CHECK_FALSE(fs::exists(root / "made"));
}

TEST_CASE("localfs: Computer lists places and volumes", "[localfs]") {
    auto places = ui::localfs::listPlaces();
    const std::string home = std::getenv("HOME");
    CHECK(ui::localfs::documentsFolder() == home);
    auto has = [&](const std::string& path, const std::string& type) {
        return std::any_of(places.begin(), places.end(), [&](const LocalEntry& e) { return e.path == path && e.type == type; });
    };
    CHECK(has(home, "place"));
    CHECK(has("/", "drive"));
    for (const auto& e : places) {
        CHECK_FALSE(e.name.empty());
        CHECK_FALSE(e.icon.empty());
        if (e.path == "/") {
            CHECK(e.totalBytes > 0);
            CHECK(e.freeBytes > 0);
            CHECK(e.icon == "drive");
        }
    }
    // Places come before volumes.
    auto firstDrive = std::find_if(places.begin(), places.end(), [](const LocalEntry& e) { return e.type == "drive"; });
    CHECK(std::all_of(firstDrive, places.end(), [](const LocalEntry& e) { return e.type == "drive"; }));
}

TEST_CASE("localfs: folder watcher reports changes, debounced", "[localfs]") {
    TempDir tmp;
    fs::create_directories(tmp.path() / "sub");
    std::atomic<int> calls{0};
    {
        ui::localfs::DirWatcher w(u8(tmp.path()), [&] { ++calls; });
        spit(tmp.path() / "one.txt", "1");
        REQUIRE(waitFor([&] { return calls.load() >= 1; }, 3000ms));

        // A burst of changes collapses into very few notifications.
        std::this_thread::sleep_for(400ms);
        int before = calls.load();
        for (int i = 0; i < 20; ++i) spit(tmp.path() / ("f" + std::to_string(i)), "x");
        std::this_thread::sleep_for(800ms);
        const int burst = calls.load() - before;
        CHECK(burst >= 1);
        CHECK(burst <= 3);

        // Not recursive: changes inside a subfolder are not this folder's.
        before = calls.load();
        spit(tmp.path() / "sub" / "inner.txt", "x");
        std::this_thread::sleep_for(800ms);
        CHECK(calls.load() == before);

        // Renames and deletes are.
        fs::rename(tmp.path() / "one.txt", tmp.path() / "two.txt");
        REQUIRE(waitFor([&] { return calls.load() > before; }, 3000ms));
        before = calls.load();
        fs::remove(tmp.path() / "two.txt");
        REQUIRE(waitFor([&] { return calls.load() > before; }, 3000ms));
    }
    // Destroyed watcher: no more callbacks.
    const int after = calls.load();
    spit(tmp.path() / "late.txt", "x");
    std::this_thread::sleep_for(500ms);
    CHECK(calls.load() == after);
    REQUIRE_THROWS(ui::localfs::DirWatcher(u8(tmp.path() / "missing"), [] {}));
}

TEST_CASE("localfs: the watched folder itself goes away", "[localfs]") {
    TempDir tmp;
    fs::create_directories(tmp.path() / "watched");
    std::atomic<int> calls{0};
    ui::localfs::DirWatcher w(u8(tmp.path() / "watched"), [&] { ++calls; });
    fs::rename(tmp.path() / "watched", tmp.path() / "renamed");
    CHECK(waitFor([&] { return calls.load() >= 1; }, 3000ms));
}

TEST_CASE("localfs: bridge handlers", "[localfs]") {
    TempDir tmp;
    BridgeHarness h;
    LocalFsService svc(h.bridge, []() -> void* { return nullptr; });
    svc.registerHandlers();

    spit(tmp.path() / "x.txt", "data");
    Json list = h.invoke("local:list", Json::array({u8(tmp.path())}));
    REQUIRE(list.size() == 1);
    CHECK(list[0]["name"] == "x.txt");
    CHECK(list[0]["type"] == "file");
    CHECK_FALSE(list[0].contains("freeBytes"));

    Json places = h.invoke("local:list", Json::array({""}));
    REQUIRE(places.is_array());
    REQUIRE_FALSE(places.empty());

    CHECK(h.invoke("local:stat", Json::array({u8(tmp.path() / "nope")})).is_null());
    CHECK(h.invoke("local:stat", Json::array({u8(tmp.path() / "x.txt")}))["size"] == 4);
    CHECK_FALSE(h.invoke("local:home", Json::array()).get<std::string>().empty());

    h.invoke("local:mkdir", Json::array({u8(tmp.path() / "d")}));
    h.invoke("local:rename", Json::array({u8(tmp.path() / "x.txt"), u8(tmp.path() / "d" / "y.txt")}));
    CHECK(slurp(tmp.path() / "d" / "y.txt") == "data");
    CHECK_FALSE(h.invokeError("local:list", Json::array({u8(tmp.path() / "missing")})).empty());

    const std::string watchId = h.invoke("local:watch", Json::array({u8(tmp.path())}));
    spit(tmp.path() / "new.txt", "n");
    REQUIRE(h.waitEvent([&](const Json& e) { return e["ch"] == "local:onChanged" && e["args"][0] == watchId; }, 3000ms));
    h.send("local:unwatch", Json::array({watchId}));
    svc.closeAll();
}
