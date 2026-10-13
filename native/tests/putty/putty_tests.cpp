// PuTTY import/export, ssh config import, known_hosts parsing and import apply.
#include "TestServers.h"

#include "core/Crypto.h"
#include "core/Strings.h"
#include "importers/Importers.h"
#include "putty/Putty.h"
#include "putty/RegFile.h"
#include "store/Store.h"

#include <catch2/catch_test_macros.hpp>

#include <fstream>

using namespace sc;
using namespace sc::putty;
using namespace sc::test;

namespace {

// Known-answer vectors produced with OpenSSH ssh-keygen (see the generation script in docs/notes/keys-import.md).
const char* ED25519_DATA = "0x3a5157895e099b03da859a3195215e7a916f8d26038f336a72e090ace856cc7c,0x5db635f3135198f7cb25651971011442feef1f248c5c971f380cf6324a11c36a";
const char* ED25519_FP = "SHA256:y6W7g52ahWf8PxKD/ea1kreQujZyE9r8DbZiXyh65NE";
const char* RSA_DATA = "0x10001,0xb274880b3280020e9db7207be6c9a735c215cbbd492a0177863666ff22205126e8ee53384f4fe5056d84896b143c83481fd927a33d57269c1d71de6e42b0818831f2a8a9ebbf03c792ccf3f0327ac28157803cfd845a2bff9de3987e671ab6f7d5a1a8725307094afccd986fa3216f0f08fb4be4aa0dda5e3574e79c7589d5780569f4e7aeca9564d8a7fdaefd35cd53b2160b17ca4b224056f7c87545c161b3ea758b79afe8e171a91127d2926e73f373d80287146770abd8f22c7c8b92dbd42221229f0d9f60b2a2d446c6c61a2adca01c57683d418e47dccfbd4e9ddc07543cc1a4b955dd78140ff208aa2ab1bbc7222a56f38739485153b2a114c7bb53df";
const char* RSA_FP = "SHA256:VveAw5P6mOhCtVdppbOqa/EfuXhjaQqUmI2nm/oZgVM";
const char* EC_DATA = "0x1491f1f7f6401a0cb64cfb9d2ec8279c11ce12b77c9f0fee15c65bdf32cb977c,0x693ec17a0531b74b1ac9882785e599313d075cae4048ab63ac3956a5227992fb";
const char* EC_FP = "SHA256:pkS84RezXGwPUryvSLVyVkBP2hby9WlpeYAxjaCmS1Q";

RegValue S(const std::string& s) {
    RegValue v;
    v.type = RegValue::Type::String;
    v.str = s;
    return v;
}
RegValue D(uint32_t n) {
    RegValue v;
    v.type = RegValue::Type::Dword;
    v.num = n;
    return v;
}

void writeFile(const std::filesystem::path& p, const std::string& bytes) {
    std::ofstream f(p, std::ios::binary);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

TEST_CASE("putty: session names escape like mungestr", "[putty]") {
    REQUIRE(escapeSessionName("My Server") == "My%20Server");
    REQUIRE(escapeSessionName("100% sure?") == "100%25%20sure%3F");
    REQUIRE(unescapeSessionName("My%20Server") == "My Server");
    REQUIRE(unescapeSessionName(escapeSessionName("a\\b*c d%e")) == "a\\b*c d%e");
    REQUIRE(unescapeSessionName("Default%20Settings") == "Default Settings");
}

TEST_CASE("putty: host keys convert to the same fingerprints as ssh-keygen", "[putty][hostkeys]") {
    std::string reason;
    auto ed = convertHostKey("ssh-ed25519@22:server.example", ED25519_DATA, reason);
    REQUIRE(ed);
    REQUIRE(ed->fingerprint == ED25519_FP);
    REQUIRE(ed->host == "server.example");
    REQUIRE(ed->port == 22);
    REQUIRE(ed->keyType == "ssh-ed25519");
    REQUIRE(ed->source == "putty-import");

    auto rsa = convertHostKey("rsa2@2222:10.0.0.1", RSA_DATA, reason);
    REQUIRE(rsa);
    REQUIRE(rsa->fingerprint == RSA_FP);
    REQUIRE(rsa->keyType == "ssh-rsa");
    REQUIRE(rsa->port == 2222);

    auto ec = convertHostKey("ecdsa-sha2-nistp256@22:h", EC_DATA, reason);
    REQUIRE(ec);
    REQUIRE(ec->fingerprint == EC_FP);
    auto ec2 = convertHostKey("ecdsa-sha2-nistp256@22:h", std::string("nistp256,") + EC_DATA, reason);
    REQUIRE(ec2);
    REQUIRE(ec2->fingerprint == EC_FP);

    REQUIRE_FALSE(convertHostKey("ssh-ed448@22:h", "0x1,0x2", reason));
    REQUIRE_FALSE(convertHostKey("garbage", "0x1", reason));
}

TEST_CASE("putty: missing values take PuTTY's defaults", "[putty][mapping]") {
    RegValues v;
    v["HostName"] = S("router.lan");
    std::vector<std::string> w;
    SessionConfig s = puttyToSession("router", v, w);
    REQUIRE(s.protocol == Protocol::Ssh);
    REQUIRE(s.port == 22);
    REQUIRE(s.connection.keepaliveSeconds == 0);
    REQUIRE(s.connection.terminalType == "xterm");
    REQUIRE(s.keyboard.backspace == BackspaceKey::Del);
    REQUIRE(s.keyboard.homeEnd == HomeEndMode::Standard);
    REQUIRE(s.keyboard.functionKeys == FunctionKeyMode::EscN);
    REQUIRE(s.terminal.bell == BellMode::System);
    REQUIRE(s.terminal.closeOnExit == CloseOnExit::Clean);
    REQUIRE(s.terminal.localEcho == TriState::Auto);
    REQUIRE(s.terminal.answerback == "PuTTY");
    REQUIRE(s.ssh.ciphers.empty());
    REQUIRE(s.serial.flowControl == FlowControl::XonXoff);
    REQUIRE_FALSE(s.appearance.contains("fontFamily"));
    REQUIRE_FALSE(s.appearance.contains("customScheme"));
}

TEST_CASE("putty: every mapped setting", "[putty][mapping]") {
    RegValues v;
    v["Protocol"] = S("ssh");
    v["HostName"] = S("bob@db.example.com");
    v["PortNumber"] = D(2200);
    v["PingIntervalSecs"] = D(45);
    v["TCPNoDelay"] = D(0);
    v["TCPKeepalives"] = D(1);
    v["AddressFamily"] = D(2);
    v["LogHost"] = S("db-logical");
    v["TerminalType"] = S("vt220");
    v["TerminalSpeed"] = S("9600,9600");
    v["Environment"] = S("LANG\ten_US.UTF-8,EDITOR\tvim");
    v["ProxyMethod"] = D(2);
    v["ProxyHost"] = S("proxy.corp");
    v["ProxyPort"] = D(1080);
    v["ProxyUsername"] = S("pu");
    v["ProxyPassword"] = S("pp");
    v["ProxyExcludeList"] = S("*.local");
    v["ProxyLocalhost"] = D(1);
    v["ProxyDNS"] = D(2);
    v["RemoteCommand"] = S("tmux attach");
    v["Compression"] = D(1);
    v["Cipher"] = S("chacha20,aes,WARN,3des");
    v["TryAgent"] = D(0);
    v["AgentFwd"] = D(1);
    v["AuthKI"] = D(0);
    v["PublicKeyFile"] = S("C:\\keys\\id.ppk");
    v["NoPTY"] = D(1);
    v["X11Forward"] = D(1);
    v["X11Display"] = S("localhost:10");
    v["PortForwardings"] = S("L8080=localhost:80,R2222=10.0.0.5:22,D1080,4L127.0.0.1:5432=db:5432");
    v["LocalPortAcceptAll"] = D(1);
    v["LogType"] = D(2);
    v["LogFileName"] = S("C:\\logs\\&H-&Y.log");
    v["LogFileClash"] = D(1);
    v["Font"] = S("Consolas");
    v["FontHeight"] = D(12);
    v["CurType"] = D(2);
    v["BlinkCur"] = D(1);
    v["ScrollbackLines"] = D(50000);
    v["BellType"] = D(2);
    v["LocalEcho"] = D(0);
    v["LocalEdit"] = D(1);
    v["WinTitle"] = S("DB");
    v["BackspaceIsDelete"] = D(0);
    v["RXVTHomeEnd"] = D(1);
    v["LinuxFunctionKeys"] = D(2);
    v["NoApplicationKeys"] = D(1);
    v["LFImpliesCR"] = D(1);
    v["AutoWrapMode"] = D(0);
    v["LineCodePage"] = S("ISO-8859-1:1998 (Latin-1, West Europe)");
    v["CloseOnExit"] = D(2);
    v["Colour2"] = S("10,20,30");
    v["TelnetRet"] = D(0);
    std::vector<std::string> w;
    SessionConfig s = puttyToSession("db", v, w);
    REQUIRE(s.host == "db.example.com");
    REQUIRE(s.username == "bob");
    REQUIRE(s.port == 2200);
    REQUIRE(s.connection.keepaliveSeconds == 45);
    REQUIRE_FALSE(s.connection.tcpNoDelay);
    REQUIRE(s.connection.tcpKeepalive);
    REQUIRE(s.connection.addressFamily == AddressFamily::IPv6);
    REQUIRE(s.connection.logicalHostname == "db-logical");
    REQUIRE(s.connection.terminalType == "vt220");
    REQUIRE(s.connection.terminalSpeed == "9600,9600");
    REQUIRE(s.connection.environment.size() == 2);
    REQUIRE(s.connection.environment[1].name == "EDITOR");
    REQUIRE(s.proxy.type == ProxyType::Socks5);
    REQUIRE(s.proxy.host == "proxy.corp");
    REQUIRE(s.proxy.password == "pp");
    REQUIRE(s.proxy.proxyLocalhost);
    REQUIRE(s.proxy.dnsAtProxy == TriState::On);
    REQUIRE(s.ssh.remoteCommand == "tmux attach");
    REQUIRE(s.ssh.compression);
    REQUIRE(s.ssh.ciphers.front() == "chacha20-poly1305@openssh.com");
    REQUIRE(std::find(s.ssh.ciphers.begin(), s.ssh.ciphers.end(), "3des-cbc") == s.ssh.ciphers.end());  // below WARN
    REQUIRE_FALSE(s.ssh.auth.tryAgent);
    REQUIRE(s.ssh.auth.agentForwarding);
    REQUIRE_FALSE(s.ssh.auth.keyboardInteractive);
    REQUIRE(s.ssh.auth.privateKeyPath == "C:\\keys\\id.ppk");
    REQUIRE_FALSE(s.ssh.allocatePty);
    REQUIRE(s.ssh.x11.enabled);
    REQUIRE(s.ssh.x11.display == "localhost:10");
    REQUIRE(s.ssh.tunnels.size() == 4);
    REQUIRE(s.ssh.tunnels[0].type == ForwardType::Local);
    REQUIRE(s.ssh.tunnels[0].sourcePort == 8080);
    REQUIRE(s.ssh.tunnels[0].destHost == "localhost");
    REQUIRE(s.ssh.tunnels[1].type == ForwardType::Remote);
    REQUIRE(s.ssh.tunnels[1].destPort == 22);
    REQUIRE(s.ssh.tunnels[2].type == ForwardType::Dynamic);
    REQUIRE(s.ssh.tunnels[3].addressFamily == AddressFamily::IPv4);
    REQUIRE(s.ssh.tunnels[3].bindAddress == "127.0.0.1");
    REQUIRE(s.ssh.localPortsAcceptAll);
    REQUIRE(s.logging.mode == LogMode::All);
    REQUIRE(s.logging.path == "C:\\logs\\&H-&Y.log");
    REQUIRE(s.logging.ifExists == LogIfExists::Overwrite);
    REQUIRE(s.appearance["fontFamily"] == "'Consolas', monospace");
    REQUIRE(s.appearance["fontSize"] == 16);
    REQUIRE(s.appearance["cursorStyle"] == "bar");
    REQUIRE(s.appearance["cursorBlink"] == true);
    REQUIRE(s.appearance["customScheme"]["background"] == "#0a141e");
    REQUIRE(s.terminal.scrollbackLines == 50000);
    REQUIRE(s.terminal.bell == BellMode::Visual);
    REQUIRE(s.terminal.localEcho == TriState::On);
    REQUIRE(s.terminal.localLineEditing == TriState::Off);
    REQUIRE(s.terminal.windowTitle == "DB");
    REQUIRE(s.keyboard.backspace == BackspaceKey::CtrlH);
    REQUIRE(s.keyboard.homeEnd == HomeEndMode::Rxvt);
    REQUIRE(s.keyboard.functionKeys == FunctionKeyMode::XtermR6);
    REQUIRE(s.keyboard.disableAppKeypad);
    REQUIRE(s.terminal.implicitCrInLf);
    REQUIRE_FALSE(s.terminal.autoWrap);
    REQUIRE(s.translation.charset == "ISO-8859-1");
    REQUIRE(s.terminal.closeOnExit == CloseOnExit::Always);
    REQUIRE_FALSE(s.telnet.returnSendsNewline);
}

TEST_CASE("putty: serial settings", "[putty][mapping]") {
    RegValues v;
    v["Protocol"] = S("serial");
    v["SerialLine"] = S("COM7");
    v["SerialSpeed"] = D(115200);
    v["SerialDataBits"] = D(7);
    v["SerialStopHalfbits"] = D(4);
    v["SerialParity"] = D(2);
    v["SerialFlowControl"] = D(2);
    std::vector<std::string> w;
    SessionConfig s = puttyToSession("console", v, w);
    REQUIRE(s.protocol == Protocol::Serial);
    REQUIRE(s.serial.line == "COM7");
    REQUIRE(s.serial.baudRate == 115200);
    REQUIRE(s.serial.dataBits == 7);
    REQUIRE(s.serial.stopBits == 2.0);
    REQUIRE(s.serial.parity == Parity::Even);
    REQUIRE(s.serial.flowControl == FlowControl::RtsCts);
}

TEST_CASE("putty: .reg export parses back to equivalent sessions", "[putty][regfile]") {
    SessionConfig a = makeDefaultSession();
    a.name = "Web 01";
    a.host = "web01.example.com";
    a.port = 2022;
    a.username = "deploy";
    a.ssh.tunnels = {PortForward{"x", ForwardType::Local, true, "", 8443, "localhost", 443, AddressFamily::Auto, ""}};
    a.keyboard.functionKeys = FunctionKeyMode::Linux;
    a.terminal.bell = BellMode::None;
    SessionConfig b = makeDefaultSession();
    b.name = "Console";
    b.protocol = Protocol::Serial;
    b.serial.line = "COM4";
    b.serial.baudRate = 57600;

    const std::string bytes = importers::exportPuttyReg({a, b});
    REQUIRE(static_cast<unsigned char>(bytes[0]) == 0xFF);
    TempDir dir;
    writeFile(dir.path() / "export.reg", bytes);

    TempDir storeDir;
    Store store(storeDir.path());
    store.load();
    ImportPreview p = importers::previewPuttyRegFile(narrow((dir.path() / "export.reg").wstring()), store);
    REQUIRE(p.candidates.size() == 2);
    const auto& web = p.candidates[1].name == "Web 01" ? p.candidates[1] : p.candidates[0];
    REQUIRE(web.name == "Web 01");
    REQUIRE(web.session.host == "web01.example.com");
    REQUIRE(web.session.port == 2022);
    REQUIRE(web.session.username == "deploy");
    REQUIRE(web.session.ssh.tunnels.size() == 1);
    REQUIRE(web.session.ssh.tunnels[0].sourcePort == 8443);
    REQUIRE(web.session.keyboard.functionKeys == FunctionKeyMode::Linux);
    REQUIRE(web.session.terminal.bell == BellMode::None);
    const auto& con = p.candidates[1].name == "Console" ? p.candidates[1] : p.candidates[0];
    REQUIRE(con.session.protocol == Protocol::Serial);
    REQUIRE(con.session.serial.baudRate == 57600);
}

TEST_CASE("putty: ANSI REGEDIT4 files with continuations and host keys", "[putty][regfile]") {
    const std::string reg =
        "REGEDIT4\r\n\r\n"
        "[HKEY_CURRENT_USER\\Software\\SimonTatham\\PuTTY\\Sessions\\Default%20Settings]\r\n"
        "\"TerminalType\"=\"vt100\"\r\n\r\n"
        "[HKEY_CURRENT_USER\\Software\\SimonTatham\\PuTTY\\Sessions\\Jump%20Box]\r\n"
        "\"HostName\"=\"jump.example\"\r\n"
        "\"Binary\"=hex:01,02,\\\r\n  03\r\n\r\n"
        "[HKEY_CURRENT_USER\\Software\\SimonTatham\\PuTTY\\Sessions\\Inner]\r\n"
        "\"HostName\"=\"10.1.1.1\"\r\n"
        "\"ProxyMethod\"=dword:00000006\r\n"
        "\"ProxyHost\"=\"Jump Box\"\r\n\r\n"
        "[HKEY_CURRENT_USER\\Software\\SimonTatham\\PuTTY\\SshHostKeys]\r\n"
        "\"ssh-ed25519@22:jump.example\"=\"" + std::string(ED25519_DATA) + "\"\r\n";
    TempDir dir;
    writeFile(dir.path() / "a.reg", reg);
    TempDir storeDir;
    Store store(storeDir.path());
    store.load();
    ImportPreview p = importers::previewPuttyRegFile(narrow((dir.path() / "a.reg").wstring()), store);
    REQUIRE(p.hasDefaultSettings);
    REQUIRE(p.defaultSettings.connection.terminalType == "vt100");
    REQUIRE(p.candidates.size() == 2);
    REQUIRE(p.hostKeys.size() == 1);
    REQUIRE(p.hostKeys[0].fingerprint == ED25519_FP);
    const auto& inner = p.candidates[0].name == "Inner" ? p.candidates[0] : p.candidates[1];
    REQUIRE(inner.session.proxy.type == ProxyType::Ssh);
    REQUIRE(inner.session.proxy.jumpSessionId == "import:Jump%20Box");

    // Apply: jump reference resolved, host key stored, defaults replaced.
    ImportRequest req;
    req.candidates = p.candidates;
    req.hostKeys = p.hostKeys;
    req.targetFolder = "PuTTY";
    req.hasDefaultSettings = true;
    req.defaultSettings = p.defaultSettings;
    ImportResult r = importers::applyImport(store, req);
    REQUIRE(r.imported == 2);
    REQUIRE(r.hostKeysImported == 1);
    auto all = store.listSessions();
    REQUIRE(all.size() == 2);
    const auto jumpIt = std::find_if(all.begin(), all.end(), [](const SessionConfig& s) { return s.name == "Jump Box"; });
    const auto innerIt = std::find_if(all.begin(), all.end(), [](const SessionConfig& s) { return s.name == "Inner"; });
    REQUIRE(jumpIt != all.end());
    REQUIRE(innerIt != all.end());
    REQUIRE(innerIt->folder == "PuTTY");
    REQUIRE(innerIt->proxy.jumpSessionId == jumpIt->id);
    REQUIRE(store.defaultSessionTemplate().connection.terminalType == "vt100");

    // Conflicts: skip / rename / overwrite.
    req.hostKeys.clear();
    req.hasDefaultSettings = false;
    req.conflict = "skip";
    REQUIRE(importers::applyImport(store, req).skipped == 2);
    req.conflict = "rename";
    REQUIRE(importers::applyImport(store, req).imported == 2);
    REQUIRE(store.sessionNameExists("PuTTY", "Inner (2)"));
    req.conflict = "overwrite";
    ImportResult ow = importers::applyImport(store, req);
    REQUIRE(ow.overwritten == 2);
    REQUIRE(store.listSessions().size() == 4);
}

TEST_CASE("ssh config: hosts, wildcards, jump hosts and forwards", "[importers][sshconfig]") {
    TempDir dir;
    const std::string cfg =
        "Host bastion\n  HostName bastion.example.com\n  Port 2222\n  User admin\n  IdentityFile ~/.ssh/id_bastion\n\n"
        "Host app app-alias\n  HostName 10.0.0.7\n  ProxyJump bastion\n  LocalForward 8080 localhost:80\n"
        "  DynamicForward 1080\n  ForwardAgent yes\n  SetEnv TZ=UTC\n  ControlMaster auto\n\n"
        "Host *.wild\n  User nobody\n\n"
        "Host *\n  ServerAliveInterval 25\n  User defaultuser\n";
    writeFile(dir.path() / "config", cfg);
    TempDir storeDir;
    Store store(storeDir.path());
    store.load();
    ImportPreview p = importers::previewSshConfig(narrow((dir.path() / "config").wstring()), store);
    REQUIRE(p.candidates.size() == 3);  // bastion, app, app-alias (no wildcards)
    auto find = [&](const std::string& n) {
        return *std::find_if(p.candidates.begin(), p.candidates.end(), [&](const ImportCandidate& c) { return c.name == n; });
    };
    const auto b = find("bastion");
    REQUIRE(b.session.host == "bastion.example.com");
    REQUIRE(b.session.port == 2222);
    REQUIRE(b.session.username == "admin");  // first match wins (Host * is last)
    REQUIRE(b.session.connection.keepaliveSeconds == 25);
    REQUIRE(b.session.ssh.auth.privateKeyPath.find("id_bastion") != std::string::npos);
    const auto a = find("app");
    REQUIRE(a.session.host == "10.0.0.7");
    REQUIRE(a.session.username == "defaultuser");
    REQUIRE(a.session.proxy.type == ProxyType::Ssh);
    REQUIRE(a.session.proxy.jumpSessionId == "import:bastion");
    REQUIRE(a.session.ssh.tunnels.size() == 2);
    REQUIRE(a.session.ssh.auth.agentForwarding);
    REQUIRE(a.session.connection.environment.size() == 1);
    REQUIRE(std::any_of(a.warnings.begin(), a.warnings.end(), [](const std::string& w) { return w.find("controlmaster") != std::string::npos; }));
}

TEST_CASE("known_hosts: plain, bracketed and hashed entries", "[importers][knownhosts]") {
    const std::string blob = [] {
        std::string b;
        crypto::base64Decode("AAAAC3NzaC1lZDI1NTE5AAAAIOZ9uXxXgM3VZT9tO8n7c8qkA9mX0hY1LYhQe6gk0xQ0", b);
        return b;
    }();
    const std::string b64 = crypto::base64Encode(blob);
    const std::string content = "# comment\n"
                                "host1.example,192.0.2.1 ssh-ed25519 " + b64 + " a comment\n"
                                "[host2.example]:2222 ssh-ed25519 " + b64 + "\n"
                                "|1|abc=|def= ssh-ed25519 " + b64 + "\n"
                                "@cert-authority *.example ssh-ed25519 " + b64 + "\n"
                                "broken line\n";
    int skipped = 0;
    auto hosts = importers::parseOpenSshKnownHosts(content, skipped);
    REQUIRE(hosts.size() == 3);
    REQUIRE(hosts[0].host == "host1.example");
    REQUIRE(hosts[1].host == "192.0.2.1");
    REQUIRE(hosts[2].host == "host2.example");
    REQUIRE(hosts[2].port == 2222);
    REQUIRE(skipped == 3);
}

TEST_CASE("putty: real registry smoke test (read-only, counts only)", "[.registry]") {
    TempDir storeDir;
    Store store(storeDir.path());
    store.load();
    ImportPreview p = importers::previewPuttyRegistry(store);
    size_t warned = 0;
    for (const auto& c : p.candidates) warned += c.warnings.empty() ? 0 : 1;
    WARN("sessions: " << p.candidates.size() << " (with warnings: " << warned << "), default settings: " << p.hasDefaultSettings
                      << ", host keys converted: " << p.hostKeys.size() << ", errors: " << p.errors.size());
    for (const auto& e : p.errors) WARN(e);
    for (const auto& k : p.hostKeys) REQUIRE(k.fingerprint.rfind("SHA256:", 0) == 0);
}
