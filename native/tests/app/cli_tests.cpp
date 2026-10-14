// Unit tests for the PuTTY-compatible command line parser (app/Cli) and app path resolution (app/Paths).
#include "app/Cli.h"
#include "app/Paths.h"
#include "core/Strings.h"
#include "store/Store.h"

#include "TestServers.h"

#include <catch2/catch_test_macros.hpp>

using namespace st;
namespace fs = std::filesystem;

namespace {

CliContext makeContext(const fs::path& cwd = {}) {
    CliContext ctx;
    ctx.defaultTemplate = SessionConfig{};
    ctx.defaultTemplate.id = "default";
    ctx.defaultTemplate.connection.terminalType = "vt220";
    ctx.cwd = cwd;
    ctx.findSession = [](const std::string& name) -> std::optional<SessionConfig> {
        if (iequals(name, "my router")) {
            SessionConfig s;
            s.id = "saved-1";
            s.name = "My Router";
            s.host = "router.test";
            s.port = 2222;
            s.username = "admin";
            return s;
        }
        if (iequals(name, "serial box")) {
            SessionConfig s;
            s.id = "saved-2";
            s.name = "Serial Box";
            s.protocol = Protocol::Serial;
            s.port = 0;
            s.serial.line = "COM7";
            return s;
        }
        if (iequals(name, "nohost")) {
            SessionConfig s;
            s.id = "saved-3";
            s.name = "nohost";
            return s;
        }
        return std::nullopt;
    };
    return ctx;
}

CliResult parse(std::vector<std::string> args, const CliContext& ctx = makeContext()) { return parseCommandLine(args, ctx); }

}  // namespace

TEST_CASE("cli: no arguments => no request", "[app][cli]") {
    auto r = parse({});
    REQUIRE_FALSE(r.request.has_value());
    REQUIRE(r.errors.empty());
    r = parse({"-ssh", "-C"});
    REQUIRE_FALSE(r.request.has_value());
}

TEST_CASE("cli: [user@]host[:port] uses the default template", "[app][cli]") {
    auto r = parse({"alice@example.test"});
    REQUIRE(r.request);
    const auto& s = r.request->session;
    REQUIRE(s.protocol == Protocol::Ssh);
    REQUIRE(s.host == "example.test");
    REQUIRE(s.username == "alice");
    REQUIRE(s.port == 22);
    REQUIRE(s.connection.terminalType == "vt220");  // from the template
    REQUIRE(s.id != "default");
    REQUIRE_FALSE(s.id.empty());
    REQUIRE(s.name.empty());
    REQUIRE(r.request->savedSessionId.empty());

    r = parse({"host.test:2200"});
    REQUIRE(r.request->session.port == 2200);
    r = parse({"[::1]:2201"});
    REQUIRE(r.request->session.host == "::1");
    REQUIRE(r.request->session.port == 2201);
    r = parse({"fe80::1"});
    REQUIRE(r.request->session.host == "fe80::1");
    REQUIRE(r.request->session.port == 22);
    r = parse({"user@with@at@host.test"});
    REQUIRE(r.request->session.username == "user@with@at");
    REQUIRE(r.request->session.host == "host.test");
}

TEST_CASE("cli: protocol flags set the default port unless -P is given", "[app][cli]") {
    auto r = parse({"-telnet", "h.test"});
    REQUIRE(r.request->session.protocol == Protocol::Telnet);
    REQUIRE(r.request->session.port == 23);
    r = parse({"h.test", "-rlogin"});
    REQUIRE(r.request->session.protocol == Protocol::Rlogin);
    REQUIRE(r.request->session.port == 513);
    r = parse({"-raw", "-P", "7000", "h.test"});
    REQUIRE(r.request->session.protocol == Protocol::Raw);
    REQUIRE(r.request->session.port == 7000);
    r = parse({"-P", "2022", "-telnet", "h.test"});
    REQUIRE(r.request->session.port == 2022);
    r = parse({"telnet://tn.test:2323/"});
    REQUIRE(r.request->session.protocol == Protocol::Telnet);
    REQUIRE(r.request->session.host == "tn.test");
    REQUIRE(r.request->session.port == 2323);
    r = parse({"telnet://tn.test/"});
    REQUIRE(r.request->session.port == 23);
    r = parse({"ssh://bob@s.test:2222"});
    REQUIRE(r.request->session.username == "bob");
    REQUIRE(r.request->session.port == 2222);
    REQUIRE(r.request->view.empty());
}

TEST_CASE("cli: sftp:// and scp:// open the file manager", "[app][cli]") {
    auto r = parse({"sftp://deploy%40corp@files.test:2200/var/www/my%20site"});
    REQUIRE(r.request);
    const auto& s = r.request->session;
    CHECK(s.protocol == Protocol::Ssh);
    CHECK(s.host == "files.test");
    CHECK(s.port == 2200);
    CHECK(s.username == "deploy@corp");
    CHECK(s.ssh.fileProtocol == FileProtocol::Sftp);
    CHECK(r.request->view == "files");
    CHECK(r.request->remotePath == "/var/www/my site");

    r = parse({"scp://router.test"});
    REQUIRE(r.request);
    CHECK(r.request->session.port == 22);
    CHECK(r.request->session.ssh.fileProtocol == FileProtocol::Scp);
    CHECK(r.request->view == "files");
    CHECK(r.request->remotePath.empty());

    const Json j = toJson(*r.request);
    CHECK(j["view"] == "files");
    CHECK_FALSE(j.contains("remotePath"));
}

TEST_CASE("cli: -l, -pw, -i, flags", "[app][cli]") {
    test::TempDir tmp;
    auto r = parse({"-l", "carol", "-pw", "secret", "-i", "keys\\id.ppk", "-A", "-X", "-C", "-4", "-N", "-T", "-noagent", "h.test"},
                   makeContext(tmp.path()));
    REQUIRE(r.errors.empty());
    const auto& s = r.request->session;
    REQUIRE(s.username == "carol");
    REQUIRE(r.request->password == "secret");
    REQUIRE(fs::path(widen(s.ssh.auth.privateKeyPath)) == (tmp.path() / "keys" / "id.ppk").lexically_normal());
    REQUIRE(s.ssh.auth.agentForwarding);
    REQUIRE(s.ssh.x11.enabled);
    REQUIRE(s.ssh.compression);
    REQUIRE(s.connection.addressFamily == AddressFamily::IPv4);
    REQUIRE(s.ssh.noShell);
    REQUIRE_FALSE(s.ssh.allocatePty);
    REQUIRE_FALSE(s.ssh.auth.tryAgent);

    r = parse({"-a", "-x", "-6", "-t", "u@h.test", "-l", "override"});
    REQUIRE_FALSE(r.request->session.ssh.auth.agentForwarding);
    REQUIRE_FALSE(r.request->session.ssh.x11.enabled);
    REQUIRE(r.request->session.connection.addressFamily == AddressFamily::IPv6);
    REQUIRE(r.request->session.ssh.allocatePty);
    REQUIRE(r.request->session.username == "override");  // -l wins over user@

    // -pw is SSH only
    r = parse({"-telnet", "-pw", "x", "h.test"});
    REQUIRE(r.request->password.empty());
    REQUIRE_FALSE(r.warnings.empty());
}

TEST_CASE("cli: -load merges saved session, explicit options win regardless of order", "[app][cli]") {
    auto r = parse({"-P", "2300", "-load", "MY ROUTER"});
    REQUIRE(r.request);
    REQUIRE(r.request->savedSessionId == "saved-1");
    REQUIRE(r.request->session.id == "saved-1");
    REQUIRE(r.request->session.host == "router.test");
    REQUIRE(r.request->session.port == 2300);
    REQUIRE(r.request->session.username == "admin");

    r = parse({"@my router"});
    REQUIRE(r.request->savedSessionId == "saved-1");
    REQUIRE(r.request->session.port == 2222);

    r = parse({"-load", "my router", "other.test", "-l", "root"});
    REQUIRE(r.request->session.host == "other.test");
    REQUIRE(r.request->session.username == "root");
    REQUIRE(r.request->session.port == 2222);  // same protocol: saved port kept

    r = parse({"-load", "my router", "-telnet"});
    REQUIRE(r.request->session.port == 23);  // protocol changed: default port of the new protocol

    r = parse({"-load", "missing"});
    REQUIRE_FALSE(r.request);
    REQUIRE(r.errors.size() == 1);

    r = parse({"-load", "missing", "fallback.test"});
    REQUIRE(r.request);
    REQUIRE(r.request->savedSessionId.empty());
    REQUIRE(r.request->session.host == "fallback.test");

    r = parse({"-load", "nohost"});
    REQUIRE_FALSE(r.request);

    r = parse({"-load"});
    REQUIRE_FALSE(r.errors.empty());
}

TEST_CASE("cli: port forwarding specs", "[app][cli]") {
    auto r = parse({"-L", "5110:pop.test:110", "-L", "127.0.0.5:23:localhost:23", "-R", "5023:tn.test:23", "-D", "4096", "-D",
                    "[::1]:1080", "-L8080:web.test:80", "h.test"});
    REQUIRE(r.errors.empty());
    const auto& t = r.request->session.ssh.tunnels;
    REQUIRE(t.size() == 6);
    REQUIRE(t[0].type == ForwardType::Local);
    REQUIRE(t[0].sourcePort == 5110);
    REQUIRE(t[0].destHost == "pop.test");
    REQUIRE(t[0].destPort == 110);
    REQUIRE(t[0].bindAddress.empty());
    REQUIRE(t[1].bindAddress == "127.0.0.5");
    REQUIRE(t[1].sourcePort == 23);
    REQUIRE(t[2].type == ForwardType::Remote);
    REQUIRE(t[2].sourcePort == 5023);
    REQUIRE(t[3].type == ForwardType::Dynamic);
    REQUIRE(t[3].sourcePort == 4096);
    REQUIRE(t[4].bindAddress == "::1");
    REQUIRE(t[4].sourcePort == 1080);
    REQUIRE(t[5].sourcePort == 8080);
    REQUIRE(t[5].destHost == "web.test");
    REQUIRE_FALSE(t[0].id.empty());
    REQUIRE(t[0].id != t[1].id);

    auto v6 = parseForwardSpec(ForwardType::Local, "[::1]:8022:[fe80::2]:22");
    REQUIRE(v6.bindAddress == "::1");
    REQUIRE(v6.destHost == "fe80::2");
    REQUIRE(v6.destPort == 22);

    REQUIRE_THROWS(parseForwardSpec(ForwardType::Local, "8080:host"));
    REQUIRE_THROWS(parseForwardSpec(ForwardType::Local, "abc:host:80"));
    REQUIRE_THROWS(parseForwardSpec(ForwardType::Local, "70000:host:80"));
    REQUIRE_THROWS(parseForwardSpec(ForwardType::Local, "80::22"));
    REQUIRE_THROWS(parseForwardSpec(ForwardType::Dynamic, "a:b:c"));
    REQUIRE_THROWS(parseForwardSpec(ForwardType::Local, "[::1:80:h:22"));

    r = parse({"-L", "bad", "h.test"});
    REQUIRE(r.errors.size() == 1);
    REQUIRE(r.request);  // the connection is still requested
    REQUIRE(r.request->session.ssh.tunnels.empty());
}

TEST_CASE("cli: serial line and -sercfg", "[app][cli]") {
    auto r = parse({"-serial", "com3", "-sercfg", "19200,7,e,2,R"});
    REQUIRE(r.errors.empty());
    const auto& s = r.request->session;
    REQUIRE(s.protocol == Protocol::Serial);
    REQUIRE(s.serial.line == "COM3");
    REQUIRE(s.port == 0);
    REQUIRE(s.serial.baudRate == 19200);
    REQUIRE(s.serial.dataBits == 7);
    REQUIRE(s.serial.parity == Parity::Even);
    REQUIRE(s.serial.stopBits == 2);
    REQUIRE(s.serial.flowControl == FlowControl::RtsCts);

    SerialOptions so;
    applySerialConfig(so, "115200,8,n,1.5,N");
    REQUIRE(so.baudRate == 115200);
    REQUIRE(so.stopBits == 1.5);
    REQUIRE(so.flowControl == FlowControl::None);
    applySerialConfig(so, "o,X,1, 5");
    REQUIRE(so.parity == Parity::Odd);
    REQUIRE(so.flowControl == FlowControl::XonXoff);
    REQUIRE(so.stopBits == 1);
    REQUIRE(so.dataBits == 5);
    applySerialConfig(so, "m,D");
    REQUIRE(so.parity == Parity::Mark);
    REQUIRE(so.flowControl == FlowControl::DsrDtr);
    applySerialConfig(so, "s");
    REQUIRE(so.parity == Parity::Space);
    REQUIRE_THROWS(applySerialConfig(so, "fast"));
    REQUIRE_THROWS(applySerialConfig(so, "E"));

    r = parse({"-load", "serial box"});
    REQUIRE(r.request);
    REQUIRE(r.request->session.serial.line == "COM7");
    r = parse({"\\\\.\\COM12", "-serial"});
    REQUIRE(r.request->session.serial.line == "\\\\.\\COM12");
}

TEST_CASE("cli: -m reads the remote command, -pwfile the password", "[app][cli]") {
    test::TempDir tmp;
    writeFileAtomic(tmp.path() / "cmd.txt", "\xEF\xBB\xBFuptime; ls -la\n");
    writeFileAtomic(tmp.path() / "pw.txt", "p@ss word\r\nsecond line\n");
    auto r = parse({"-m", "cmd.txt", "-pwfile", "pw.txt", "h.test"}, makeContext(tmp.path()));
    REQUIRE(r.errors.empty());
    REQUIRE(r.request->session.ssh.remoteCommand == "uptime; ls -la\n");
    REQUIRE(r.request->password == "p@ss word");

    r = parse({"-m", "missing.txt", "h.test"}, makeContext(tmp.path()));
    REQUIRE(r.errors.size() == 1);
    REQUIRE(r.request->session.ssh.remoteCommand.empty());
}

TEST_CASE("cli: logging, misc options, unknown options", "[app][cli]") {
    test::TempDir tmp;
    auto r = parse({"-sshlog", "ssh.log", "-logoverwrite", "-loghost", "logical.test", "-proxycmd", "nc %host %port", "h.test"},
                   makeContext(tmp.path()));
    REQUIRE(r.errors.empty());
    const auto& s = r.request->session;
    REQUIRE(s.logging.mode == LogMode::SshDebug);
    REQUIRE(fs::path(widen(s.logging.path)) == tmp.path() / "ssh.log");
    REQUIRE(s.logging.ifExists == LogIfExists::Overwrite);
    REQUIRE(s.connection.logicalHostname == "logical.test");
    REQUIRE(s.proxy.type == ProxyType::Command);
    REQUIRE(s.proxy.localCommand == "nc %host %port");

    r = parse({"-v", "h.test"});
    REQUIRE(r.verbose);
    REQUIRE(r.request->session.logging.mode == LogMode::SshDebug);

    r = parse({"-batch", "-hostkey", "aa:bb", "-bogus", "-1", "-2", "h.test", "extra"});
    REQUIRE(r.errors.empty());
    REQUIRE(r.warnings.size() == 5);
    REQUIRE(r.request->session.host == "h.test");

    r = parse({"-P", "notaport", "h.test"});
    REQUIRE(r.errors.size() == 1);
    REQUIRE(r.request->session.port == 22);

    r = parse({"-supdup", "h.test"});
    REQUIRE(r.errors.size() == 1);
}

TEST_CASE("cli: host options are split off", "[app][cli]") {
    auto h = parseHostOptions({"--dev-url", "http://localhost:5173", "-ssh", "--devtools", "--data-dir=C:\\x", "h.test"});
    REQUIRE(h.devUrl == "http://localhost:5173");
    REQUIRE(h.devtools);
    REQUIRE(h.dataDir == fs::path(L"C:\\x"));
    REQUIRE(h.rest == std::vector<std::string>{"-ssh", "h.test"});
    REQUIRE(h.errors.empty());

    h = parseHostOptions({"--dev-url=file:///c:/x", "--dev-url"});
    REQUIRE(h.devUrl.empty());
    REQUIRE(h.errors.size() == 2);
}

TEST_CASE("cli: splitCommandLine follows Windows quoting rules", "[app][cli]") {
    auto a = splitCommandLine(L"SSHTerm.exe -load \"my session\" -pw \"a\\\"b\" h\u00e9.test");
    REQUIRE(a == std::vector<std::string>{"-load", "my session", "-pw", "a\"b", "h\xC3\xA9.test"});
    REQUIRE(splitCommandLine(L"").empty());
    REQUIRE(splitCommandLine(L"app.exe").empty());
}

TEST_CASE("cli: parse with a real Store (-load + default template)", "[app][cli]") {
    test::TempDir tmp;
    Store store(tmp.path());
    store.load();
    store.updateSettings({{"defaultSession", {{"username", "templ"}, {"port", 2200}}}});
    Json saved = {{"name", "Lab Box"}, {"host", "lab.test"}, {"folder", "Lab"}};
    std::string id = store.saveSession(saved)["id"];

    auto r = parseCommandLine({"-load", "lab box"}, store, tmp.path());
    REQUIRE(r.request);
    REQUIRE(r.request->savedSessionId == id);
    REQUIRE(r.request->session.host == "lab.test");

    r = parseCommandLine({"adhoc.test"}, store, tmp.path());
    REQUIRE(r.request->session.username == "templ");
    REQUIRE(r.request->session.port == 2200);
}

TEST_CASE("paths: portable vs installed vs override", "[app][paths]") {
    test::TempDir tmp;
    fs::path exe = tmp.path() / "bin";
    fs::create_directories(exe);
    fs::path appData = tmp.path() / "Roaming";

    auto p = resolveAppPaths(exe, appData);
    REQUIRE_FALSE(p.portable);
    REQUIRE(p.dataDir == appData / "SSHTerm");
    REQUIRE(p.logsDir == appData / "SSHTerm" / "logs");
    REQUIRE(p.webViewDataDir == appData / "SSHTerm" / "WebView2");
    REQUIRE(p.uiDir == exe / "ui");

    writeFileAtomic(exe / "portable.flag", "");
    p = resolveAppPaths(exe, appData);
    REQUIRE(p.portable);
    REQUIRE(p.dataDir == exe / "data");

    fs::remove(exe / "portable.flag");
    fs::create_directories(exe / "data");
    p = resolveAppPaths(exe, appData);
    REQUIRE(p.portable);
    REQUIRE(p.dataDir == exe / "data");

    p = resolveAppPaths(exe, appData, tmp.path() / "custom");
    REQUIRE_FALSE(p.portable);
    REQUIRE(p.dataDir == tmp.path() / "custom");
}
