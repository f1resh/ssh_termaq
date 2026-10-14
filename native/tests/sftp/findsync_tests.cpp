// Find (glob + recursive search) and Synchronize (filters, comparison rules, compare -> apply -> nothing left).
#include "SftpFixture.h"

#include "sftp/FindSync.h"

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace sc;
using namespace sc::test;
using namespace sc::sftpimpl;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

/** Runs sftp:find and collects every result name until the final event. */
std::pair<std::vector<std::string>, Json> findAll(SftpFixture& f, Json req) {
    const std::string findId = f.h.invoke("sftp:find", Json::array({f.id, req}));
    REQUIRE(f.h.waitEvent([&](const Json& e) {
        return e["ch"] == "sftp:onFind" && e["args"][0]["findId"] == findId && e["args"][0]["done"] == true;
    }));
    std::vector<std::string> paths;
    Json last;
    for (const auto& e : f.h.eventsOn("sftp:onFind")) {
        if (e["args"][0]["findId"] != findId) continue;
        for (const auto& r : e["args"][0]["results"]) paths.push_back(r["path"]);
        last = e["args"][0];
    }
    std::sort(paths.begin(), paths.end());
    return {paths, last};
}

std::set<std::string> describe(const Json& items) {
    std::set<std::string> out;
    for (const auto& i : items) out.insert(i["action"].get<std::string>() + " " + i["relPath"].get<std::string>());
    return out;
}

void waitAll(SftpFixture& f, const Json& ids) {
    for (const auto& id : ids) CHECK(f.waitDone(id)["state"] == "done");
}

}  // namespace

TEST_CASE("find: glob matching", "[find]") {
    CHECK(globMatch("*.log", "app.log", false));
    CHECK(globMatch("*.log", "APP.LOG", false));
    CHECK_FALSE(globMatch("*.log", "APP.LOG", true));
    CHECK(globMatch("?.txt", "a.txt", false));
    CHECK_FALSE(globMatch("?.txt", "ab.txt", false));
    CHECK(globMatch("a*b*c", "aXXbYYc", false));
    CHECK_FALSE(globMatch("a*b*c", "aXXbYY", false));
    CHECK(globMatch("[ab]*", "beta", false));
    CHECK_FALSE(globMatch("[ab]*", "gamma", false));
    CHECK(globMatch("[!a]*", "beta", false));
    CHECK_FALSE(globMatch("[^b]*", "beta", false));
    CHECK(globMatch("file[0-9].bin", "file7.bin", false));
    CHECK_FALSE(globMatch("file[0-9].bin", "fileX.bin", false));
    CHECK(globMatch("[abc", "[abc", false));  // no closing bracket: literal
    CHECK(globMatch("*", "", false));
    CHECK(globMatch("*.tar.gz", "backup.tar.gz", false));
    CHECK(globMatch("report", "Report", false));

    CHECK(splitMasks(" *.log ;; *.txt;") == std::vector<std::string>{"*.log", "*.txt"});
}

TEST_CASE("find: recursive search over SFTP", "[find]") {
    SftpFixture f;
    const fs::path r = f.remoteRoot.path();
    fs::create_directories(r / "logs" / "old");
    fs::create_directories(r / ".cache");
    spit(r / "logs" / "app.log", "1");
    spit(r / "logs" / "old" / "APP.LOG", "2");
    spit(r / "logs" / "notes.txt", "3");
    spit(r / ".cache" / "hidden.log", "4");
    spit(r / "readme.md", "5");

    auto [paths, last] = findAll(f, Json{{"root", "/"}, {"pattern", "*.log"}});
    CHECK(paths == std::vector<std::string>{"/logs/app.log", "/logs/old/APP.LOG"});
    CHECK(last["scannedDirs"].get<int>() >= 3);
    CHECK(last["error"] == "");

    auto [hidden, h] = findAll(f, Json{{"root", "/"}, {"pattern", "*.log"}, {"includeHidden", true}, {"caseSensitive", true}});
    CHECK(hidden == std::vector<std::string>{"/.cache/hidden.log", "/logs/app.log"});

    auto [multi, m] = findAll(f, Json{{"root", "/logs"}, {"pattern", "*.txt; old"}});
    CHECK(multi == std::vector<std::string>{"/logs/notes.txt", "/logs/old"});

    auto [capped, c] = findAll(f, Json{{"root", "/"}, {"pattern", "*"}, {"maxResults", 2}});
    CHECK(capped.size() == 2);
    CHECK(c["truncated"] == true);

    auto [none, err] = findAll(f, Json{{"root", "/missing"}, {"pattern", "*"}});
    CHECK(none.empty());
    CHECK_FALSE(err["error"].get<std::string>().empty());
}

TEST_CASE("sync: filters and comparison rules", "[sync]") {
    SyncRequest req;
    req.exclude = "*.tmp; node_modules/; .git/";
    req.includeHidden = false;
    SyncFilter filter(req);
    CHECK(filter.accepts("a.txt", false));
    CHECK_FALSE(filter.accepts("x.TMP", false));
    CHECK_FALSE(filter.accepts("node_modules", true));
    CHECK(filter.accepts("node_modules", false));  // folder-only mask
    CHECK_FALSE(filter.accepts(".env", false));     // hidden

    Tree local{{"same.txt", {false, 5, 1000}},
               {"newer.txt", {false, 5, 2000}},
               {"older.txt", {false, 5, 1000}},
               {"size.txt", {false, 6, 1000}},
               {"tolerant.txt", {false, 5, 1001}},
               {"onlyLocal.txt", {false, 1, 1000}},
               {"newDir", {true, 0, 0}},
               {"newDir/inner.txt", {false, 1, 1000}},
               {"clash", {true, 0, 0}},
               {"clash/x", {false, 1, 1}}};
    Tree remote{{"same.txt", {false, 5, 1000}},
                {"newer.txt", {false, 5, 1000}},
                {"older.txt", {false, 5, 2000}},
                {"size.txt", {false, 5, 1000}},
                {"tolerant.txt", {false, 5, 1000}},
                {"onlyRemote.txt", {false, 1, 1000}},
                {"oldDir", {true, 0, 0}},
                {"oldDir/f", {false, 1, 1}},
                {"clash", {false, 1, 1}}};

    auto run = [&](const std::string& direction, bool deleteExtra, bool time = true, bool size = true) {
        SyncRequest r;
        r.direction = direction;
        r.deleteExtra = deleteExtra;
        r.compareTime = time;
        r.compareSize = size;
        std::set<std::string> out;
        for (const auto& i : compareTrees(local, remote, r, 2)) out.insert(i.action + " " + i.relPath + " " + i.reason);
        return out;
    };

    CHECK(run("upload", true) == std::set<std::string>{"upload newer.txt newer", "upload older.txt older",
                                                       "upload size.txt size", "upload onlyLocal.txt new",
                                                       "upload newDir new", "deleteRemote onlyRemote.txt extra",
                                                       "deleteRemote oldDir extra"});
    CHECK(run("download", false) == std::set<std::string>{"download newer.txt older", "download older.txt newer",
                                                          "download size.txt size", "download onlyRemote.txt new",
                                                          "download oldDir new"});
    CHECK(run("both", true) == std::set<std::string>{"upload newer.txt newer", "download older.txt newer",
                                                     "upload onlyLocal.txt new", "upload newDir new",
                                                     "download onlyRemote.txt new", "download oldDir new"});
    // size only: the time difference does not matter
    CHECK(run("upload", false, false, true) == std::set<std::string>{"upload size.txt size", "upload onlyLocal.txt new",
                                                                     "upload newDir new"});
}

TEST_CASE("sync: compare, apply, compare again finds nothing", "[sync]") {
    SftpFixture f;
    const fs::path r = f.remoteRoot.path(), l = f.local.path();
    fs::create_directories(l / "site" / "css");
    spit(l / "site" / "index.html", "<html>new</html>");
    spit(l / "site" / "css" / "app.css", "body{}");
    spit(l / "site" / "draft.tmp", "skip me");
    fs::create_directories(r / "site" / "old");
    spit(r / "site" / "index.html", "<html>old</html>");
    spit(r / "site" / "old" / "gone.html", "gone");
    setMtime(l / "site" / "index.html", 1700000500);
    setMtime(r / "site" / "index.html", 1700000000);

    Json req{{"localDir", u8(l / "site")}, {"remoteDir", "/site"}, {"direction", "upload"},  {"compareTime", true},
             {"compareSize", true},       {"deleteExtra", true},  {"exclude", "*.tmp"},     {"includeHidden", true}};
    Json items = f.h.invoke("sftp:syncCompare", Json::array({f.id, req}));
    CHECK(describe(items) == std::set<std::string>{"upload index.html", "upload css", "deleteRemote old"});

    waitAll(f, f.h.invoke("sftp:syncApply", Json::array({f.id, req, items, Json{{"deleteToRecycleBin", false}}})));
    CHECK(slurp(r / "site" / "index.html") == "<html>new</html>");
    CHECK(slurp(r / "site" / "css" / "app.css") == "body{}");
    CHECK_FALSE(fs::exists(r / "site" / "old"));
    CHECK_FALSE(fs::exists(r / "site" / "draft.tmp"));
    CHECK(f.h.invoke("sftp:syncCompare", Json::array({f.id, req})).empty());

    // The other way round: a change on the server comes down, a local extra file is deleted.
    spit(r / "site" / "css" / "app.css", "body{color:red}");
    setMtime(r / "site" / "css" / "app.css", mtimeOf(l / "site" / "css" / "app.css") + 100);
    spit(l / "site" / "extra.txt", "local only");
    req["direction"] = "download";
    items = f.h.invoke("sftp:syncCompare", Json::array({f.id, req}));
    CHECK(describe(items) == std::set<std::string>{"download css/app.css", "deleteLocal extra.txt"});
    waitAll(f, f.h.invoke("sftp:syncApply", Json::array({f.id, req, items, Json{{"deleteToRecycleBin", false}}})));
    CHECK(slurp(l / "site" / "css" / "app.css") == "body{color:red}");
    CHECK_FALSE(fs::exists(l / "site" / "extra.txt"));
    CHECK(f.h.invoke("sftp:syncCompare", Json::array({f.id, req})).empty());

    // Unsafe paths from the page are refused.
    Json bad = Json::array({Json{{"relPath", "../escape.txt"}, {"isDir", false}, {"action", "download"}, {"reason", "new"}}});
    CHECK_FALSE(f.h.invokeError("sftp:syncApply", Json::array({f.id, req, bad, Json::object()})).empty());
}
