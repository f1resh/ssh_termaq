// Integration tests for the SSH transport against the in-process TestSshServer.
#include "TestNet.h"
#include "TestServers.h"

#include "core/Crypto.h"
#include "core/Strings.h"
#include "net/Proxy.h"
#include "net/Socket.h"
#include "session/Prompter.h"
#include "keys/Keygen.h"
#include "ssh/SshJump.h"
#include "ssh/SshTransport.h"
#include "store/Credentials.h"
#include "store/KnownHosts.h"

#include <catch2/catch_test_macros.hpp>

#include <libssh/libssh.h>

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

using namespace sc;
using namespace sc::test;
using namespace std::chrono_literals;

namespace {

struct FakeListener : TransportListener {
    std::mutex m;
    std::condition_variable cv;
    std::string data;
    std::string local;
    std::vector<ConnStatus> statuses;
    std::vector<std::string> logs;
    bool closed = false;
    int closeCount = 0;
    CloseInfo info;

    void onData(std::string_view bytes) override {
        std::lock_guard l(m);
        data.append(bytes);
        cv.notify_all();
    }
    void onStatus(ConnStatus s, const std::string&) override {
        std::lock_guard l(m);
        statuses.push_back(s);
        cv.notify_all();
    }
    void onLog(LogLevel, const std::string& msg) override {
        std::lock_guard l(m);
        logs.push_back(msg);
    }
    void onClose(const CloseInfo& i) override {
        std::lock_guard l(m);
        closed = true;
        ++closeCount;
        info = i;
        cv.notify_all();
    }
    void onLocalOutput(const std::string& t) override {
        std::lock_guard l(m);
        local += t;
        cv.notify_all();
    }
    bool waitData(const std::string& needle, std::chrono::milliseconds t = 5s) {
        std::unique_lock l(m);
        return cv.wait_for(l, t, [&] { return data.find(needle) != std::string::npos; });
    }
    bool waitConnected(std::chrono::milliseconds t = 10s) {
        std::unique_lock l(m);
        return cv.wait_for(l, t, [&] {
            return closed || std::find(statuses.begin(), statuses.end(), ConnStatus::Connected) != statuses.end();
        }) && !closed;
    }
    bool waitClosed(std::chrono::milliseconds t = 10s) {
        std::unique_lock l(m);
        return cv.wait_for(l, t, [&] { return closed; });
    }
    std::string allLogs() {
        std::lock_guard l(m);
        return join(logs, "\n");
    }
};

struct FakePrompter : Prompter {
    std::function<PromptResponse(const Json&)> handler;
    std::mutex m;
    std::vector<Json> asked;
    PromptResponse prompt(const PromptInput& in) override {
        {
            std::lock_guard l(m);
            asked.push_back(in.json);
        }
        if (handler) return handler(in.json);
        PromptResponse r;
        r.cancelled = true;
        return r;
    }
    std::vector<std::string> kinds() {
        std::lock_guard l(m);
        std::vector<std::string> k;
        for (auto& j : asked) k.push_back(j.value("kind", ""));
        return k;
    }
};

PromptResponse accept(const std::string& choice) {
    PromptResponse r;
    r.cancelled = false;
    r.choice = choice;
    return r;
}

PromptResponse answer(std::vector<std::string> values) {
    PromptResponse r;
    r.cancelled = false;
    r.values = std::move(values);
    return r;
}

/** Everything a transport needs, with real KnownHosts / Credentials in a temp dir. */
struct Harness {
    TempDir dir;
    KnownHosts knownHosts{dir.path() / "known_hosts.json"};
    Credentials credentials{dir.path() / "credentials.json"};
    FakeListener listener;
    FakePrompter prompter;
    std::map<std::string, SessionConfig> saved;
    std::unique_ptr<Transport> transport;

    Harness() {
        knownHosts.load();
        credentials.load();
        // By default accept unknown host keys once and type "secret" for password / kbd-int prompts.
        prompter.handler = [](const Json& p) {
            const std::string kind = p.value("kind", "");
            if (kind == "hostkey") return accept("accept-save");
            if (kind == "password") return answer({"secret"});
            if (kind == "kbdint") return answer(std::vector<std::string>(p["prompts"].size(), "secret"));
            PromptResponse r;
            return r;
        };
    }

    static SessionConfig session(const TestSshServer& s) {
        SessionConfig c = makeDefaultSession();
        c.host = "127.0.0.1";
        c.port = s.port();
        c.username = "test";
        c.ssh.auth.tryAgent = false;
        c.connection.keepaliveSeconds = 0;
        return c;
    }

    TransportContext context(const SessionConfig& c) {
        TransportContext ctx;
        ctx.connId = "test#1";
        ctx.session = c;
        ctx.cols = 80;
        ctx.rows = 24;
        ctx.listener = &listener;
        ctx.prompter = &prompter;
        ctx.knownHosts = &knownHosts;
        ctx.credentials = &credentials;
        ctx.resolveSession = [this](const std::string& id) -> std::optional<SessionConfig> {
            auto it = saved.find(id);
            if (it == saved.end()) return std::nullopt;
            return it->second;
        };
        return ctx;
    }

    void start(const SessionConfig& c) {
        transport = makeSshTransport(context(c));
        transport->start();
    }

    ~Harness() { transport.reset(); }
};

std::string authorizedLine(ssh_key key) {
    char* b64 = nullptr;
    ssh_pki_export_pubkey_base64(key, &b64);
    std::string line = std::string(ssh_key_type_to_char(ssh_key_type(key))) + " " + b64 + " test@key";
    ssh_string_free_char(b64);
    return line;
}

}  // namespace

TEST_CASE("ssh: password authentication, shell echo and exit code", "[ssh]") {
    TestSshServerOptions o;
    o.allowKbdInt = false;
    TestSshServer server(o);
    Harness h;
    h.start(Harness::session(server));
    const bool connected = h.listener.waitConnected();
    INFO("logs:\n" << h.listener.allLogs() << "\nclose: " << h.listener.info.reason);
    REQUIRE(connected);
    REQUIRE(h.listener.waitData("$ "));
    h.transport->write("hello world\r");
    REQUIRE(h.listener.waitData("echo: hello world"));
    h.transport->write("exit 7\r");
    REQUIRE(h.listener.waitClosed());
    REQUIRE(h.listener.info.clean);
    REQUIRE(h.listener.info.hasExitCode);
    REQUIRE(h.listener.info.exitCode == 7);
    REQUIRE(h.listener.closeCount == 1);
    REQUIRE(server.waitForEvent("auth:password:ok", 1000));
    REQUIRE(server.waitForEvent("pty:xterm-256color:80x24", 1000));
}

TEST_CASE("ssh: unknown host key prompts once, then is trusted", "[ssh]") {
    TestSshServer server;
    Harness h;
    h.start(Harness::session(server));
    REQUIRE(h.listener.waitConnected());
    auto kinds = h.prompter.kinds();
    REQUIRE(std::count(kinds.begin(), kinds.end(), "hostkey") == 1);
    REQUIRE(h.knownHosts.check("127.0.0.1", server.port(), server.hostKeyType(), [&] {
        std::string blob;
        crypto::base64Decode(server.hostKeyBase64(), blob);
        return blob;
    }()) == HostKeyCheck::Match);
    h.transport.reset();

    FakeListener second;
    h.prompter.asked.clear();
    TransportContext ctx = h.context(Harness::session(server));
    ctx.listener = &second;
    auto t = makeSshTransport(ctx);
    t->start();
    REQUIRE(second.waitConnected());
    kinds = h.prompter.kinds();
    REQUIRE(std::count(kinds.begin(), kinds.end(), "hostkey") == 0);
}

TEST_CASE("ssh: changed host key is reported as a mismatch and can be rejected", "[ssh]") {
    TestSshServer server;
    Harness h;
    KnownHost fake;
    fake.host = "127.0.0.1";
    fake.port = server.port();
    fake.keyType = "ssh-ed25519";
    {
        ssh_key other = nullptr;
        ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &other);
        char* b64 = nullptr;
        ssh_pki_export_pubkey_base64(other, &b64);
        fake.key = b64;
        ssh_string_free_char(b64);
        ssh_key_free(other);
    }
    h.knownHosts.add(fake);
    Json seen;
    h.prompter.handler = [&](const Json& p) {
        if (p.value("kind", "") == "hostkey") {
            seen = p;
            return accept("reject");
        }
        return answer({"secret"});
    };
    h.start(Harness::session(server));
    REQUIRE(h.listener.waitClosed());
    REQUIRE(seen.value("status", "") == "mismatch");
    REQUIRE(seen["previous"].size() == 1);
    REQUIRE_FALSE(h.listener.info.error);  // user decision, not a network error
}

TEST_CASE("ssh: keyboard-interactive with several prompts", "[ssh]") {
    TestSshServerOptions o;
    o.allowPassword = false;
    o.kbdIntQuestions = {{"Password: ", "secret"}, {"Verification code: ", "123456"}};
    o.kbdIntEcho = {false, true};
    o.kbdIntName = "2FA";
    TestSshServer server(o);
    Harness h;
    Json kbd;
    h.prompter.handler = [&](const Json& p) {
        const std::string kind = p.value("kind", "");
        if (kind == "hostkey") return accept("accept-once");
        if (kind == "kbdint") {
            kbd = p;
            return answer({"secret", "123456"});
        }
        return PromptResponse{};
    };
    h.start(Harness::session(server));
    REQUIRE(h.listener.waitConnected());
    REQUIRE(kbd["prompts"].size() == 2);
    REQUIRE(kbd["prompts"][1]["echo"] == true);
    REQUIRE(server.waitForEvent("auth:kbdint:ok", 1000));
}

TEST_CASE("ssh: public key authentication from an OpenSSH key file", "[ssh]") {
    TempDir keys;
    ssh_key key = nullptr;
    REQUIRE(ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &key) == SSH_OK);
    const auto keyPath = keys.path() / "id_ed25519";
    REQUIRE(ssh_pki_export_privkey_file(key, nullptr, nullptr, nullptr, narrow(keyPath.wstring()).c_str()) == SSH_OK);
    TestSshServerOptions o;
    o.allowPassword = false;
    o.allowKbdInt = false;
    o.authorizedKeys = {authorizedLine(key)};
    ssh_key_free(key);
    TestSshServer server(o);
    Harness h;
    SessionConfig c = Harness::session(server);
    c.ssh.auth.privateKeyPath = narrow(keyPath.wstring());
    h.start(c);
    REQUIRE(h.listener.waitConnected());
    REQUIRE(server.waitForEvent("auth:publickey:ok", 1000));
}

TEST_CASE("ssh: encrypted key asks for the passphrase", "[ssh]") {
    TempDir keys;
    ssh_key key = nullptr;
    REQUIRE(ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &key) == SSH_OK);
    const auto keyPath = keys.path() / "id_enc";
    REQUIRE(ssh_pki_export_privkey_file(key, "pass phrase", nullptr, nullptr, narrow(keyPath.wstring()).c_str()) == SSH_OK);
    TestSshServerOptions o;
    o.allowPassword = false;
    o.allowKbdInt = false;
    o.authorizedKeys = {authorizedLine(key)};
    ssh_key_free(key);
    TestSshServer server(o);
    Harness h;
    int asked = 0;
    h.prompter.handler = [&](const Json& p) {
        const std::string kind = p.value("kind", "");
        if (kind == "hostkey") return accept("accept-once");
        if (kind == "passphrase") return answer({++asked == 1 ? "wrong" : "pass phrase"});
        return PromptResponse{};
    };
    SessionConfig c = Harness::session(server);
    c.ssh.auth.privateKeyPath = narrow(keyPath.wstring());
    h.start(c);
    REQUIRE(h.listener.waitConnected());
    REQUIRE(asked == 2);
}

TEST_CASE("ssh: saved password is used without prompting, wrong password retries", "[ssh]") {
    TestSshServerOptions o;
    o.allowKbdInt = false;
    TestSshServer server(o);
    Harness h;
    SessionConfig c = Harness::session(server);
    h.credentials.set(Credentials::passwordKey(c.id), "secret");
    h.start(c);
    REQUIRE(h.listener.waitConnected());
    auto kinds = h.prompter.kinds();
    REQUIRE(std::count(kinds.begin(), kinds.end(), "password") == 0);
    h.transport.reset();

    // Wrong saved password -> prompt; then cancel.
    h.credentials.set(Credentials::passwordKey(c.id), "nope");
    FakeListener l2;
    int prompts = 0;
    h.prompter.handler = [&](const Json& p) {
        if (p.value("kind", "") == "hostkey") return accept("accept-once");
        if (p.value("kind", "") == "password") {
            ++prompts;
            PromptResponse r;  // cancel
            return r;
        }
        return PromptResponse{};
    };
    TransportContext ctx = h.context(c);
    ctx.listener = &l2;
    auto t = makeSshTransport(ctx);
    t->start();
    REQUIRE(l2.waitClosed());
    REQUIRE(prompts == 1);
    REQUIRE(l2.local.find("Access denied") != std::string::npos);
}

TEST_CASE("ssh: resize, environment and terminal type", "[ssh]") {
    TestSshServer server;
    Harness h;
    SessionConfig c = Harness::session(server);
    c.connection.environment = {{"LANG", "en_US.UTF-8"}};
    h.start(c);
    REQUIRE(h.listener.waitConnected());
    REQUIRE(h.listener.waitData("$ "));
    h.transport->resize(132, 43, 0, 0);
    REQUIRE(server.waitForEvent("window-change:132x43", 3000));
    h.transport->write("size\r");
    REQUIRE(h.listener.waitData("132x43"));
    h.transport->write("env LANG\r");
    REQUIRE(h.listener.waitData("en_US.UTF-8"));
    h.transport->write("term\r");
    REQUIRE(h.listener.waitData("xterm-256color"));
}

TEST_CASE("ssh: remote command and signals", "[ssh]") {
    TestSshServer server;
    {
        Harness h;
        SessionConfig c = Harness::session(server);
        c.ssh.remoteCommand = "echo from exec";
        h.start(c);
        REQUIRE(h.listener.waitClosed());
        REQUIRE(h.listener.data.find("from exec") != std::string::npos);
        REQUIRE(h.listener.info.exitCode == 0);
    }
    {
        Harness h;
        h.start(Harness::session(server));
        REQUIRE(h.listener.waitConnected());
        REQUIRE(h.listener.waitData("$ "));
        h.transport->write("signal\r");
        REQUIRE(server.waitForEvent("shell:signal", 3000));
        h.transport->sendSpecial("signal:INT");
        REQUIRE(h.listener.waitData("got signal INT"));
    }
}

TEST_CASE("ssh: large output is delivered completely", "[ssh]") {
    TestSshServer server;
    Harness h;
    h.start(Harness::session(server));
    REQUIRE(h.listener.waitConnected());
    REQUIRE(h.listener.waitData("$ "));
    h.transport->write("big 2000000\r");
    REQUIRE(h.listener.waitData("END", 20s));
    std::lock_guard l(h.listener.m);
    REQUIRE(std::count(h.listener.data.begin(), h.listener.data.end(), 'x') >= 2000000);
}

TEST_CASE("ssh: local port forwarding", "[ssh][forward]") {
    TestEchoServer echo;
    TestSshServer server;
    Harness h;
    SessionConfig c = Harness::session(server);
    int freePort = 0;
    { auto l = net::listenLoopback(0, freePort); }
    PortForward f;
    f.id = "f1";
    f.type = ForwardType::Local;
    f.sourcePort = freePort;
    f.destHost = "127.0.0.1";
    f.destPort = echo.port();
    c.ssh.tunnels = {f};
    h.start(c);
    REQUIRE(h.listener.waitConnected());
    std::this_thread::sleep_for(200ms);
    auto sock = net::connectTcp("127.0.0.1", freePort, 3000);
    REQUIRE(sock);
    REQUIRE(net::sendAll(sock.get(), "through the tunnel"));
    std::string got;
    for (int i = 0; i < 50 && got.size() < 18; ++i) {
        if (!net::waitReadable(sock.get(), 200)) continue;
        char buf[64];
        int n = recv(sock.get(), buf, sizeof buf, 0);
        if (n <= 0) break;
        got.append(buf, static_cast<size_t>(n));
    }
    REQUIRE(got == "through the tunnel");
    REQUIRE(server.waitForEvent("direct-tcpip:127.0.0.1:" + std::to_string(echo.port()), 1000));
}

TEST_CASE("ssh: dynamic SOCKS5 forwarding", "[ssh][forward]") {
    TestEchoServer echo;
    TestSshServer server;
    Harness h;
    SessionConfig c = Harness::session(server);
    int freePort = 0;
    { auto l = net::listenLoopback(0, freePort); }
    PortForward f;
    f.id = "d1";
    f.type = ForwardType::Dynamic;
    f.sourcePort = freePort;
    c.ssh.tunnels = {f};
    c.ssh.noShell = true;
    h.start(c);
    REQUIRE(h.listener.waitConnected());
    std::this_thread::sleep_for(200ms);
    auto sock = net::connectTcp("127.0.0.1", freePort, 3000);
    REQUIRE(sock);
    REQUIRE(net::sendAll(sock.get(), std::string("\x05\x01\x00", 3)));
    char buf[64];
    REQUIRE(net::waitReadable(sock.get(), 3000));
    REQUIRE(recv(sock.get(), buf, 2, 0) == 2);
    REQUIRE(buf[1] == 0);
    std::string req("\x05\x01\x00\x01\x7f\x00\x00\x01", 8);
    req.push_back(static_cast<char>(echo.port() >> 8));
    req.push_back(static_cast<char>(echo.port() & 0xff));
    REQUIRE(net::sendAll(sock.get(), req));
    REQUIRE(net::waitReadable(sock.get(), 3000));
    REQUIRE(recv(sock.get(), buf, 10, 0) == 10);
    REQUIRE(buf[1] == 0);
    REQUIRE(net::sendAll(sock.get(), "socks!"));
    REQUIRE(net::waitReadable(sock.get(), 3000));
    int n = recv(sock.get(), buf, sizeof buf, 0);
    REQUIRE(std::string(buf, static_cast<size_t>(n)) == "socks!");
}

TEST_CASE("ssh: remote port forwarding", "[ssh][forward]") {
    TestEchoServer echo;
    TestSshServer server;
    Harness h;
    SessionConfig c = Harness::session(server);
    PortForward f;
    f.id = "r1";
    f.type = ForwardType::Remote;
    f.sourcePort = 0;  // server picks
    f.destHost = "127.0.0.1";
    f.destPort = echo.port();
    c.ssh.tunnels = {f};
    h.start(c);
    REQUIRE(h.listener.waitConnected());
    REQUIRE(server.waitForEvent("tcpip-forward:", 3000));
    const int port = server.lastRemoteForwardPort();
    REQUIRE(port > 0);
    auto sock = net::connectTcp("127.0.0.1", port, 3000);
    REQUIRE(sock);
    REQUIRE(net::sendAll(sock.get(), "reverse"));
    REQUIRE(net::waitReadable(sock.get(), 5000));
    char buf[64];
    int n = recv(sock.get(), buf, sizeof buf, 0);
    REQUIRE(std::string(buf, static_cast<size_t>(std::max(n, 0))) == "reverse");
}

TEST_CASE("ssh: close() during connect and before start", "[ssh]") {
    TestScriptServer silent;  // accepts TCP but never speaks SSH
    Harness h;
    SessionConfig c = makeDefaultSession();
    c.host = "127.0.0.1";
    c.port = silent.port();
    c.username = "test";
    c.connection.connectTimeoutSeconds = 30;
    h.start(c);
    REQUIRE(silent.waitForConnection(3000));
    const auto t0 = std::chrono::steady_clock::now();
    h.transport->close();
    REQUIRE(h.listener.waitClosed(5s));
    REQUIRE(std::chrono::steady_clock::now() - t0 < 3s);
    REQUIRE(h.listener.closeCount == 1);

    Harness h2;
    auto t = makeSshTransport(h2.context(c));
    t->close();
    t.reset();  // destroying a never-started transport must not hang
}

TEST_CASE("ssh: post() runs tasks with the session, and with nullptr after close", "[ssh]") {
    TestSshServer server;
    Harness h;
    h.start(Harness::session(server));
    REQUIRE(h.listener.waitConnected());
    auto* ssh = h.transport->asSsh();
    REQUIRE(ssh);
    std::promise<bool> got;
    ssh->post([&](ssh_session s) { got.set_value(s != nullptr); });
    REQUIRE(got.get_future().get());
    h.transport->close();
    REQUIRE(h.listener.waitClosed());
    std::promise<bool> after;
    ssh->post([&](ssh_session s) { after.set_value(s == nullptr); });
    REQUIRE(after.get_future().get());
}

TEST_CASE("ssh: jump host tunnel", "[ssh][jump]") {
    TestSshServer jumpServer;
    TestSshServer target;
    setSshJumpOpener(&openSshJumpTunnel);
    Harness h;
    SessionConfig jump = Harness::session(jumpServer);
    jump.name = "jump";
    h.saved[jump.id] = jump;
    SessionConfig c = Harness::session(target);
    c.proxy.type = ProxyType::Ssh;
    c.proxy.jumpSessionId = jump.id;
    c.proxy.proxyLocalhost = true;  // the target is 127.0.0.1, which bypasses proxies by default (PuTTY semantics)
    h.start(c);
    const bool connected = h.listener.waitConnected(15s);
    INFO("logs:\n" << h.listener.allLogs() << "\nclose: " << h.listener.info.reason);
    REQUIRE(connected);
    REQUIRE(jumpServer.waitForEvent("direct-tcpip:127.0.0.1:" + std::to_string(target.port()), 2000));
    REQUIRE(h.listener.waitData("$ "));
    h.transport->write("user\r");
    REQUIRE(h.listener.waitData("test"));
    INFO(h.listener.allLogs());
    REQUIRE(h.listener.allLogs().find("Jump: ") != std::string::npos);
}

TEST_CASE("ssh: authentication through Pageant (agent)", "[ssh][agent]") {
    const std::filesystem::path pageant = L"C:\\Program Files\\PuTTY\\pageant.exe";
    if (!std::filesystem::exists(pageant)) SKIP("pageant.exe not installed");
    if (FindWindowW(L"Pageant", L"Pageant")) SKIP("a Pageant instance is already running; not touching it");

    // A throwaway key in PuTTY's own format (written by our PPK writer).
    TempDir dir;
    KeygenResult kr = keys::generateKey(KeygenRequest{"ed25519", 0, "agent-test", ""});
    const auto keyPath = dir.path() / "agent_key.ppk";
    {
        std::ofstream f(keyPath, std::ios::binary);
        f << kr.privateKeyPpk;
    }
    TestSshServerOptions o;
    o.allowPassword = false;
    o.allowKbdInt = false;
    o.authorizedKeys = {kr.publicKeyOpenSsh};
    TestSshServer server(o);

    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + pageant.wstring() + L"\" \"" + keyPath.wstring() + L"\"";
    REQUIRE(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi));
    struct Kill {
        PROCESS_INFORMATION& pi;
        ~Kill() {
            TerminateProcess(pi.hProcess, 0);
            WaitForSingleObject(pi.hProcess, 3000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    } kill{pi};
    for (int i = 0; i < 50 && !FindWindowW(L"Pageant", L"Pageant"); ++i) std::this_thread::sleep_for(100ms);
    REQUIRE(FindWindowW(L"Pageant", L"Pageant"));
    std::this_thread::sleep_for(500ms);  // key loading

    Harness h;
    SessionConfig c = Harness::session(server);
    c.ssh.auth.tryAgent = true;
    c.ssh.auth.agent = AgentKind::Pageant;
    h.start(c);
    const bool connected = h.listener.waitConnected(40s);
    INFO(h.listener.allLogs());
    REQUIRE(connected);
    REQUIRE(server.waitForEvent("auth:publickey:ok", 1000));
    REQUIRE(h.listener.allLogs().find("Pageant has 1 SSH-2 key") != std::string::npos);
}
