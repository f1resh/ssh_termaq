// Self-tests for the test servers, validated with independent clients (PuTTY's plink / psftp).
#include "TestExecProcess.h"
#include "TestNet.h"
#include "TestServers.h"

#include "core/Strings.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace st::test;
namespace fs = std::filesystem;

namespace {

const fs::path kPlink = findTool("plink");
const fs::path kPsftp = findTool("psftp");

CommandResult run(const std::string& cmd) { return runCommand(cmd); }

std::string plinkBase(const TestSshServer& s, const std::string& pw = "secret") {
    return "\"" + kPlink.string() + "\" -batch -ssh -P " + std::to_string(s.port()) + " -l test -pw " + pw + " -hostkey " +
           s.hostKeyFingerprint() + " 127.0.0.1";
}

}  // namespace

TEST_CASE("TestEchoServer echoes data", "[support]") {
    TestEchoServer echo;
    auto sock = net::connectTcp("127.0.0.1", echo.port(), 2000);
    REQUIRE(sock);
    REQUIRE(net::sendAll(sock.get(), "ping"));
    REQUIRE(net::waitReadable(sock.get(), 2000));
    char buf[16] = {};
    const long n = st::sys::recvSome(sock.get(), buf, sizeof buf);
    REQUIRE(std::string(buf, static_cast<size_t>(n)) == "ping");
}

TEST_CASE("TestScriptServer records and sends", "[support]") {
    TestScriptServer srv;
    auto sock = net::connectTcp("127.0.0.1", srv.port(), 2000);
    REQUIRE(sock);
    REQUIRE(srv.waitForConnection(2000));
    REQUIRE(net::sendAll(sock.get(), "hello"));
    REQUIRE(srv.waitForReceived("hello", 2000));
    srv.send("world");
    REQUIRE(net::waitReadable(sock.get(), 2000));
    char buf[16] = {};
    const long n = st::sys::recvSome(sock.get(), buf, sizeof buf);
    REQUIRE(std::string(buf, static_cast<size_t>(n)) == "world");
}

TEST_CASE("TestSshServer basics", "[support]") {
    TestSshServer s;
    REQUIRE(s.port() > 0);
    REQUIRE(s.hostKeyType() == "ssh-ed25519");
    REQUIRE(s.hostKeyFingerprint().rfind("SHA256:", 0) == 0);
}

TEST_CASE("TestSshServer works with plink (exec, exit codes, password)", "[support][plink]") {
    if (kPlink.empty()) SKIP("plink not installed");
    TestSshServer s;

    auto r = run(plinkBase(s) + " echo hi there");
    INFO(r.out);
    REQUIRE(r.code == 0);
    REQUIRE(r.out.find("hi there") != std::string::npos);

    r = run(plinkBase(s) + " exit 3");
    INFO(r.out);
    REQUIRE(r.code == 3);

    r = run(plinkBase(s, "wrong") + " echo nope");
    INFO(r.out);
    REQUIRE(r.code != 0);
    REQUIRE(r.out.find("nope") == std::string::npos);

    // plink prefers keyboard-interactive (answered with -pw).
    REQUIRE(s.waitForEvent("auth:kbdint:ok", 1000));
    REQUIRE(s.waitForEvent("exec:echo hi there", 1000));
    REQUIRE(s.waitForEvent("auth:kbdint:fail", 1000));
}

TEST_CASE("TestSshServer password-only authentication with plink", "[support][plink]") {
    if (kPlink.empty()) SKIP("plink not installed");
    TestSshServerOptions o;
    o.allowKbdInt = false;
    TestSshServer s(o);
    auto r = run(plinkBase(s) + " echo pw");
    INFO(r.out);
    REQUIRE(r.code == 0);
    REQUIRE(s.waitForEvent("auth:password:ok", 1000));
}

TEST_CASE("TestSshServer works with plink for RSA and ECDSA host keys", "[support][plink]") {
    if (kPlink.empty()) SKIP("plink not installed");
    for (const char* type : {"rsa", "ecdsa"}) {
        TestSshServerOptions o;
        o.hostKeyType = type;
        TestSshServer s(o);
        auto r = run(plinkBase(s) + " echo ok");
        INFO(type << ": " << r.out);
        REQUIRE(r.code == 0);
        REQUIRE(r.out.find("ok") != std::string::npos);
    }
}

TEST_CASE("TestSshServer serves SFTP to psftp", "[support][plink]") {
    if (kPsftp.empty()) SKIP("psftp not installed");
    TempDir root, local;
    fs::create_directories(root.path() / "sub");
    { std::ofstream(root.path() / "remote.txt") << "remote content"; }
    { std::ofstream(local.path() / "upload.txt") << "uploaded content"; }

    TestSshServerOptions o;
    o.sftpRoot = root.path();
    TestSshServer s(o);

    const fs::path script = local.path() / "batch.txt";
    {
        std::ofstream f(script);
        f << "lcd \"" << local.path().string() << "\"\n";
        f << "ls\n";
        f << "get remote.txt got.txt\n";
        f << "put upload.txt\n";
        f << "mkdir newdir\n";
        f << "rename upload.txt sub/moved.txt\n";
    }
    auto r = run("\"" + kPsftp.string() + "\" -batch -P " + std::to_string(s.port()) + " -l test -pw secret -hostkey " +
                 s.hostKeyFingerprint() + " -b \"" + script.string() + "\" 127.0.0.1");
    INFO(r.out);
    REQUIRE(r.code == 0);
    REQUIRE(r.out.find("remote.txt") != std::string::npos);
    std::ifstream got(local.path() / "got.txt");
    std::string content((std::istreambuf_iterator<char>(got)), std::istreambuf_iterator<char>());
    REQUIRE(content == "remote content");
    REQUIRE(fs::exists(root.path() / "newdir"));
    REQUIRE(fs::exists(root.path() / "sub" / "moved.txt"));
    REQUIRE(s.waitForEvent("subsystem:sftp", 1000));
}

TEST_CASE("TestSshServer honours local forwarding from plink", "[support][plink]") {
    if (kPlink.empty()) SKIP("plink not installed");
    TestEchoServer echo;
    TestSshServer s;
    // plink -L with -N would block; run a command that sleeps briefly instead is not available, so verify the
    // direct-tcpip path with plink's -nc (stdio forwarding) which opens a direct-tcpip channel.
    auto r = run("echo forwarded| \"" + kPlink.string() + "\" -batch -ssh -P " + std::to_string(s.port()) +
                 " -l test -pw secret -hostkey " + s.hostKeyFingerprint() + " -nc 127.0.0.1:" +
                 std::to_string(echo.port()) + " 127.0.0.1");
    INFO(r.out);
    REQUIRE(r.out.find("forwarded") != std::string::npos);
    REQUIRE(s.waitForEvent("direct-tcpip:127.0.0.1:" + std::to_string(echo.port()), 1000));
}
