// Local file system module: path helpers, listings, shell file operations (no UI, permanent deletes only, so the
// Recycle Bin is never touched), folder watching and the local:* bridge handlers.
#include "TestBridge.h"
#include "TestServers.h"

#include "core/Strings.h"
#include "core/Win.h"
#include "localfs/LocalFs.h"
#include "localfs/LocalFsService.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

using namespace sc;
using namespace sc::test;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

std::string u8(const fs::path& p) { return narrow(p.wstring()); }

const LocalEntry* find(const std::vector<LocalEntry>& list, const std::string& name) {
    auto it = std::find_if(list.begin(), list.end(), [&](const LocalEntry& e) { return e.name == name; });
    return it == list.end() ? nullptr : &*it;
}

localfs::ShellOpOptions quiet() {
    localfs::ShellOpOptions o;
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
    CHECK(localfs::normalizePath("c:") == "C:\\");
    CHECK(localfs::normalizePath("c:/") == "C:\\");
    CHECK(localfs::normalizePath("C:\\Users\\") == "C:\\Users");
    CHECK(localfs::normalizePath("c:/a//b///c/") == "C:\\a\\b\\c");
    CHECK(localfs::normalizePath("  D:\\x  ") == "D:\\x");
    CHECK(localfs::normalizePath("\\\\server\\share\\dir\\") == "\\\\server\\share\\dir");
    CHECK(localfs::normalizePath("\\\\?\\C:\\very\\long") == "\\\\?\\C:\\very\\long");
    CHECK(localfs::joinPath("C:\\", "x") == "C:\\x");
    CHECK(localfs::joinPath("C:\\a", "x") == "C:\\a\\x");
}

TEST_CASE("localfs: list, stat and file operations", "[localfs]") {
    TempDir tmp;
    const fs::path root = tmp.path();
    fs::create_directories(root / "sub");
    spit(root / "a.txt", "hello");
    spit(root / widen("Привет мир 😀.txt"), "unicode");
    spit(root / "hidden.txt", "h");
    SetFileAttributesW((root / "hidden.txt").c_str(), FILE_ATTRIBUTE_HIDDEN);

    auto list = localfs::list(u8(root));
    REQUIRE(list.size() == 4);
    const LocalEntry* a = find(list, "a.txt");
    REQUIRE(a);
    CHECK(a->type == "file");
    CHECK(a->size == 5);
    CHECK(a->path == u8(root / "a.txt"));
    CHECK(a->mtime > 1600000000000LL);
    CHECK_FALSE(a->hidden);
    REQUIRE(find(list, "sub"));
    CHECK(find(list, "sub")->type == "dir");
    REQUIRE(find(list, "hidden.txt"));
    CHECK(find(list, "hidden.txt")->hidden);
    REQUIRE(find(list, "Привет мир 😀.txt"));

    // stat
    auto st = localfs::stat(u8(root / "a.txt"));
    REQUIRE(st);
    CHECK(st->name == "a.txt");
    CHECK(st->size == 5);
    CHECK_FALSE(localfs::stat(u8(root / "missing.bin")));
    CHECK_FALSE(localfs::stat(u8(root / "missing-dir" / "x")));
    auto rootStat = localfs::stat("C:\\");
    REQUIRE(rootStat);
    CHECK(rootStat->type == "dir");
    CHECK(rootStat->name == "C:");

    // mkdir / rename
    localfs::makeDir(u8(root / "made"));
    CHECK(fs::is_directory(root / "made"));
    REQUIRE_THROWS(localfs::makeDir(u8(root / "made")));
    localfs::rename(u8(root / "a.txt"), u8(root / "made" / "b.txt"));
    CHECK(slurp(root / "made" / "b.txt") == "hello");
    REQUIRE_THROWS(localfs::rename(u8(root / "a.txt"), u8(root / "c.txt")));  // source is gone

    // copy and move through IFileOperation (silent)
    fs::create_directories(root / "target");
    localfs::copy({u8(root / "made"), u8(root / "hidden.txt")}, u8(root / "target"), false, quiet());
    CHECK(slurp(root / "target" / "made" / "b.txt") == "hello");
    CHECK(fs::exists(root / "target" / "hidden.txt"));
    CHECK(fs::exists(root / "made" / "b.txt"));
    localfs::copy({u8(root / "sub")}, u8(root / "target"), true, quiet());
    CHECK(fs::is_directory(root / "target" / "sub"));
    CHECK_FALSE(fs::exists(root / "sub"));

    // permanent delete (recursive)
    localfs::remove({u8(root / "target"), u8(root / "made")}, false, quiet());
    CHECK_FALSE(fs::exists(root / "target"));
    CHECK_FALSE(fs::exists(root / "made"));
    REQUIRE_THROWS(localfs::list(u8(root / "target")));
}

TEST_CASE("localfs: long paths", "[localfs]") {
    TempDir tmp;
    fs::path deep = tmp.path();
    std::string p = u8(deep);
    for (int i = 0; p.size() < 300; ++i) {
        p = localfs::joinPath(p, std::string(40, static_cast<char>('a' + i)));
        localfs::makeDir(p);
    }
    REQUIRE(p.size() >= 300);
    const std::wstring file = L"\\\\?\\" + widen(localfs::joinPath(p, "deep.txt"));
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    DWORD n = 0;
    WriteFile(h, "abc", 3, &n, nullptr);
    CloseHandle(h);
    auto list = localfs::list(p);
    REQUIRE(list.size() == 1);
    CHECK(list[0].name == "deep.txt");
    CHECK(list[0].size == 3);
    REQUIRE(localfs::stat(list[0].path));
}

TEST_CASE("localfs: This PC lists known folders and drives", "[localfs]") {
    auto places = localfs::listPlaces();
    const auto docs = localfs::documentsFolder();
    REQUIRE_FALSE(docs.empty());
    auto hasPath = [&](const std::string& path, const std::string& type) {
        return std::any_of(places.begin(), places.end(),
                           [&](const LocalEntry& e) { return iequals(e.path, path) && e.type == type; });
    };
    CHECK(hasPath(docs, "place"));
    CHECK(hasPath("C:\\", "drive"));
    for (const auto& e : places) {
        CHECK_FALSE(e.name.empty());
        CHECK_FALSE(e.icon.empty());
    }
    // Places come before drives.
    auto firstDrive = std::find_if(places.begin(), places.end(), [](const LocalEntry& e) { return e.type == "drive"; });
    CHECK(std::all_of(firstDrive, places.end(), [](const LocalEntry& e) { return e.type == "drive"; }));
}

TEST_CASE("localfs: folder watcher reports changes, debounced", "[localfs]") {
    TempDir tmp;
    std::atomic<int> calls{0};
    {
        localfs::DirWatcher w(u8(tmp.path()), [&] { ++calls; });
        spit(tmp.path() / "one.txt", "1");
        REQUIRE(waitFor([&] { return calls.load() >= 1; }, 3000ms));

        // A burst of changes collapses into very few notifications.
        const int before = calls.load();
        for (int i = 0; i < 20; ++i) spit(tmp.path() / ("f" + std::to_string(i)), "x");
        std::this_thread::sleep_for(800ms);
        const int burst = calls.load() - before;
        CHECK(burst >= 1);
        CHECK(burst <= 3);
    }
    // Destroyed watcher: no more callbacks.
    const int after = calls.load();
    spit(tmp.path() / "late.txt", "x");
    std::this_thread::sleep_for(500ms);
    CHECK(calls.load() == after);
    REQUIRE_THROWS(localfs::DirWatcher(u8(tmp.path() / "missing"), [] {}));
}

TEST_CASE("localfs: bridge handlers", "[localfs]") {
    TempDir tmp;
    BridgeHarness h;
    LocalFsService svc(h.bridge, [] { return static_cast<HWND__*>(nullptr); });
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
