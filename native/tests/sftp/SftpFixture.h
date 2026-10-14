#pragma once
// Connected SSH session + SftpService against the in-process TestSshServer, driven through the bridge.
#include "TestBridge.h"
#include "TestServers.h"

#include "core/Strings.h"
#include "session/SessionManager.h"
#include "sftp/SftpService.h"
#include "store/Store.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

namespace st::test {

inline std::string u8(const std::filesystem::path& p) { return narrow(p.wstring()); }

/** Sets a file's modification time to `unixSeconds`. */
inline void setMtime(const std::filesystem::path& p, int64_t unixSeconds) {
    using namespace std::chrono;
    const auto sys = system_clock::time_point(seconds(unixSeconds));
    std::filesystem::last_write_time(p, std::chrono::clock_cast<std::filesystem::file_time_type::clock>(sys));
}

inline int64_t mtimeOf(const std::filesystem::path& p) {
    using namespace std::chrono;
    const auto ft = std::filesystem::last_write_time(p);
    return duration_cast<seconds>(std::chrono::clock_cast<system_clock>(ft).time_since_epoch()).count();
}

struct SftpFixture {
    TempDir remoteRoot, local, data;
    std::unique_ptr<TestSshServer> server;
    std::unique_ptr<Store> store;
    BridgeHarness h;
    std::unique_ptr<SessionManager> sessions;
    std::unique_ptr<SftpService> sftp;
    std::string connId = "pane#1";
    std::string id;  // sftpId
    Json opened;

    /** openOptions = null: connect only (the test calls sftp:open itself). */
    explicit SftpFixture(TestSshServerOptions so = {}, Json openOptions = Json::object()) {
        so.sftpRoot = remoteRoot.path();
        so.allowKbdInt = false;
        server = std::make_unique<TestSshServer>(so);
        store = std::make_unique<Store>(data.path());
        store->load();
        sessions = std::make_unique<SessionManager>(*store, h.bridge);
        registerConnHandlers(h.bridge, *sessions);
        sftp = std::make_unique<SftpService>(*sessions, h.bridge);
        sftp->registerHandlers();

        // Trust the host key and save the password so the connection needs no prompts.
        SessionConfig cfg = makeDefaultSession();
        cfg.host = "127.0.0.1";
        cfg.port = server->port();
        cfg.username = "test";
        cfg.ssh.auth.tryAgent = false;
        cfg.ssh.noShell = true;
        KnownHost kh;
        kh.host = "127.0.0.1";
        kh.port = server->port();
        kh.keyType = server->hostKeyType();
        kh.key = server->hostKeyBase64();
        store->knownHosts().add(kh);
        store->credentials().set(Credentials::passwordKey(cfg.id), "secret");

        ConnOpenRequest req;
        req.connId = connId;
        req.session = cfg;
        h.invoke("conn:open", Json::array({Json(req)}));
        REQUIRE(h.waitEvent([&](const Json& e) {
            return e["ch"] == "conn:onStatus" && e["args"][0]["connId"] == connId && e["args"][0]["status"] == "connected";
        }));
        if (openOptions.is_null()) return;
        opened = h.invoke("sftp:open", Json::array({connId, openOptions}));
        id = opened["sftpId"];
    }

    ~SftpFixture() {
        if (!id.empty()) h.send("sftp:close", Json::array({id}));
        sessions->close(connId);
        sftp->closeAll();
        sessions->closeAll();
    }

    /** Last progress event of a transfer. */
    Json progress(const std::string& transferId) {
        Json last;
        for (const auto& e : h.eventsOn("sftp:onProgress"))
            if (e["args"][0]["transferId"] == transferId) last = e["args"][0];
        return last;
    }

    /** Waits for a final state and returns the last progress event. */
    Json waitDone(const std::string& transferId, std::chrono::milliseconds timeout = std::chrono::seconds(60)) {
        REQUIRE(h.waitEvent(
            [&](const Json& e) {
                if (e["ch"] != "sftp:onProgress" || e["args"][0]["transferId"] != transferId) return false;
                const std::string s = e["args"][0]["state"];
                return s == "done" || s == "error" || s == "cancelled";
            },
            timeout));
        return progress(transferId);
    }

    /** Waits for the n-th (1-based) conflict event and returns it. */
    Json waitConflict(size_t n = 1, std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
        REQUIRE(h.waitEventCount("sftp:onConflict", n, timeout));
        return h.eventsOn("sftp:onConflict")[n - 1]["args"][0];
    }

    void resolve(const Json& conflict, const std::string& action, bool applyToAll = false, const std::string& newName = "") {
        h.send("sftp:resolveConflict",
               Json::array({conflict["conflictId"], Json{{"action", action}, {"applyToAll", applyToAll}, {"newName", newName}}}));
    }

    std::string download(const std::string& remote, const std::filesystem::path& localDir, Json opts = Json::object()) {
        Json ids = h.invoke("sftp:download", Json::array({id, Json::array({remote}), u8(localDir), opts}));
        REQUIRE(ids.size() == 1);
        return ids[0];
    }

    std::string upload(const std::filesystem::path& localPath, const std::string& remoteDir, Json opts = Json::object()) {
        Json ids = h.invoke("sftp:upload", Json::array({id, remoteDir, Json::array({u8(localPath)}), opts}));
        REQUIRE(ids.size() == 1);
        return ids[0];
    }
};

}  // namespace st::test
