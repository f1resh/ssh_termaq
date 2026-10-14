// SFTP service end-to-end: Bridge requests -> SftpService -> SshTransport::post -> TestSshServer's SFTP subsystem.
#include "TestBridge.h"
#include "TestServers.h"

#include "bridge/Bridge.h"
#include "core/Crypto.h"
#include "core/Strings.h"
#include "session/SessionManager.h"
#include "sftp/SftpService.h"
#include "store/Store.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>
#include <thread>

using namespace st;
using namespace st::test;
using namespace std::chrono_literals;


TEST_CASE("sftp: browse, transfer and modify files over an SSH connection", "[sftp]") {
    TempDir remoteRoot, local, data;
    std::filesystem::create_directories(remoteRoot.path() / "docs");
    { std::ofstream(remoteRoot.path() / "docs" / "readme.txt") << "hello sftp"; }

    TestSshServerOptions so;
    so.sftpRoot = remoteRoot.path();
    so.allowKbdInt = false;
    TestSshServer server(so);

    Store store(data.path());
    store.load();
    BridgeHarness h;
    SessionManager sessions(store, h.bridge);
    registerConnHandlers(h.bridge, sessions);
    SftpService sftp(sessions, h.bridge);
    sftp.registerHandlers();

    // Trust the host key and save the password so the connection needs no prompts.
    SessionConfig cfg = makeDefaultSession();
    cfg.host = "127.0.0.1";
    cfg.port = server.port();
    cfg.username = "test";
    cfg.ssh.auth.tryAgent = false;
    KnownHost kh;
    kh.host = "127.0.0.1";
    kh.port = server.port();
    kh.keyType = server.hostKeyType();
    kh.key = server.hostKeyBase64();
    store.knownHosts().add(kh);
    store.credentials().set(Credentials::passwordKey(cfg.id), "secret");

    ConnOpenRequest req;
    req.connId = "pane#1";
    req.session = cfg;
    h.invoke("conn:open", Json::array({Json(req)}));
    REQUIRE(h.waitEvent([](const Json& e) { return e["ch"] == "conn:onStatus" && e["args"][0]["status"] == "connected"; }));

    const Json opened = h.invoke("sftp:open", Json::array({"pane#1"}));
    const std::string id = opened["sftpId"];
    REQUIRE_FALSE(id.empty());
    REQUIRE(opened["home"] == "/");

    Json list = h.invoke("sftp:list", Json::array({id, "/"}));
    REQUIRE(list.size() == 1);
    REQUIRE(list[0]["name"] == "docs");
    REQUIRE(list[0]["type"] == "dir");
    REQUIRE(list[0]["permissions"].get<std::string>()[0] == 'd');

    h.invoke("sftp:mkdir", Json::array({id, "/upload"}));
    REQUIRE(std::filesystem::is_directory(remoteRoot.path() / "upload"));
    h.invoke("sftp:rename", Json::array({id, "/docs/readme.txt", "/docs/README.md"}));
    REQUIRE(std::filesystem::exists(remoteRoot.path() / "docs" / "README.md"));

    // Upload a ~3 MB file and download it back; contents must match.
    std::string big(3 * 1024 * 1024 + 123, '\0');
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>((i * 31 + 7) & 0xff);
    { std::ofstream(local.path() / "big.bin", std::ios::binary).write(big.data(), static_cast<std::streamsize>(big.size())); }
    Json up = h.invoke("sftp:upload", Json::array({id, "/upload", Json::array({narrow((local.path() / "big.bin").wstring())})}));
    REQUIRE(up.size() == 1);
    const std::string upId = up[0];
    REQUIRE(h.waitEvent([&](const Json& e) { return e["ch"] == "sftp:onProgress" && e["args"][0]["transferId"] == upId && e["args"][0]["state"] == "done"; }, 60s));
    REQUIRE(slurp(remoteRoot.path() / "upload" / "big.bin") == big);

    std::filesystem::create_directories(local.path() / "dl");
    Json down = h.invoke("sftp:download", Json::array({id, Json::array({"/upload/big.bin", "/docs"}), narrow((local.path() / "dl").wstring())}));
    REQUIRE(down.size() == 2);
    for (const auto& tid : down)
        REQUIRE(h.waitEvent([&](const Json& e) { return e["ch"] == "sftp:onProgress" && e["args"][0]["transferId"] == tid && e["args"][0]["state"] == "done"; }, 60s));
    REQUIRE(slurp(local.path() / "dl" / "big.bin") == big);
    REQUIRE(slurp(local.path() / "dl" / "docs" / "README.md") == "hello sftp");

    h.invoke("sftp:remove", Json::array({id, "/upload", true}));
    REQUIRE_FALSE(std::filesystem::exists(remoteRoot.path() / "upload"));
    REQUIRE_THROWS(h.invoke("sftp:list", Json::array({id, "/does-not-exist"})));

    h.bridge.onWebMessage(Json{{"t", "send"}, {"ch", "sftp:close"}, {"args", Json::array({id})}}.dump());
    sessions.close("pane#1");
    sftp.closeAll();
    sessions.closeAll();
}
