// platform/sys/Agent.h on POSIX: the SSH_AUTH_SOCK client against OpenSSH's own ssh-agent (started on a socket of the
// test's, with a throwaway key; skipped when the OpenSSH tools are missing).
#include "Scratch.h"
#include "platform/sys/Agent.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>

using namespace st;
using st::test::Scratch;

namespace {

/** Runs a /bin/sh command line: its exit status (-1 if it did not run) and, if asked, its output. */
int sh(const std::string& command, std::string* out = nullptr) {
    FILE* p = ::popen((command + " 2>&1").c_str(), "r");
    if (!p) return -1;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) text.append(buf, n);
    const int status = ::pclose(p);
    if (out) *out = text;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string shellQuoted(const std::string& s) {
    std::string out = "'";
    for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

/** Sets (or, with nullopt, removes) an environment variable for the scope of a test. */
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::optional<std::string>& value) : name_(name) {
        if (const char* old = std::getenv(name)) old_ = old;
        if (value) ::setenv(name, value->c_str(), 1);
        else ::unsetenv(name);
    }
    ~ScopedEnv() {
        if (old_) ::setenv(name_, old_->c_str(), 1);
        else ::unsetenv(name_);
    }

private:
    const char* name_;
    std::optional<std::string> old_;
};

uint32_t be32At(const std::string& s, size_t at) {
    auto u = [&](size_t i) { return static_cast<uint32_t>(static_cast<unsigned char>(s[at + i])); };
    return (u(0) << 24) | (u(1) << 16) | (u(2) << 8) | u(3);
}

std::string sshString(const std::string& s) {
    const auto n = static_cast<uint32_t>(s.size());
    return std::string{static_cast<char>(n >> 24), static_cast<char>(n >> 16), static_cast<char>(n >> 8),
                       static_cast<char>(n)} +
           s;
}

constexpr char SSH_AGENTC_REQUEST_IDENTITIES = 11;
constexpr char SSH_AGENT_IDENTITIES_ANSWER = 12;
constexpr char SSH_AGENTC_SIGN_REQUEST = 13;
constexpr char SSH_AGENT_SIGN_RESPONSE = 14;

/** ssh-agent on a socket in `dir`, holding one new ed25519 key; stopped when the object goes away. */
class TestAgent {
public:
    explicit TestAgent(const std::filesystem::path& dir) : socket_((dir / "a.sock").string()) {
        const std::string key = (dir / "k").string();
        std::string out;
        REQUIRE(sh("ssh-keygen -q -t ed25519 -N '' -C agent-test -f " + shellQuoted(key), &out) == 0);
        REQUIRE(sh("ssh-agent -s -a " + shellQuoted(socket_), &out) == 0);
        const size_t at = out.find("SSH_AGENT_PID=");
        REQUIRE(at != std::string::npos);
        pid_ = std::atoi(out.c_str() + at + 14);
        REQUIRE(pid_ > 0);
        INFO(out);
        REQUIRE(sh("SSH_AUTH_SOCK=" + shellQuoted(socket_) + " ssh-add -q " + shellQuoted(key), &out) == 0);
    }
    ~TestAgent() {
        if (pid_ > 0) ::kill(pid_, SIGTERM);
    }
    const std::string& socket() const { return socket_; }

private:
    std::string socket_;
    pid_t pid_ = -1;
};

}  // namespace

TEST_CASE("agent (POSIX): requests reach the agent in SSH_AUTH_SOCK", "[platform][agent]") {
    if (sh("command -v ssh-agent && command -v ssh-add && command -v ssh-keygen") != 0)
        SKIP("OpenSSH's ssh-agent, ssh-add and ssh-keygen are not installed");
    Scratch dir;
    TestAgent agent(dir.path());
    ScopedEnv env("SSH_AUTH_SOCK", agent.socket());

    std::string reason;
    auto client = sys::connectAgent(sys::AgentFlavor::Auto, reason);
    INFO(reason);
    REQUIRE(client);
    CHECK(client->name() == "SSH agent");

    // The identities: our one key.
    auto ids = client->request(std::string(1, SSH_AGENTC_REQUEST_IDENTITIES));
    REQUIRE(ids);
    REQUIRE(ids->size() >= 9);
    REQUIRE((*ids)[0] == SSH_AGENT_IDENTITIES_ANSWER);
    REQUIRE(be32At(*ids, 1) == 1);
    const uint32_t blobLen = be32At(*ids, 5);
    REQUIRE(ids->size() >= 9 + blobLen);
    const std::string blob = ids->substr(9, blobLen);

    // ...which signs, over a connection of its own.
    auto sig = client->request(std::string(1, SSH_AGENTC_SIGN_REQUEST) + sshString(blob) + sshString("data") +
                               std::string(4, '\0'));
    REQUIRE(sig);
    REQUIRE(!sig->empty());
    CHECK((*sig)[0] == SSH_AGENT_SIGN_RESPONSE);

    // On Unix, Pageant serves that socket too.
    CHECK(sys::connectAgent(sys::AgentFlavor::Pageant, reason));
}

TEST_CASE("agent (POSIX): no agent, with a reason", "[platform][agent]") {
    std::string reason;
    {
        ScopedEnv env("SSH_AUTH_SOCK", std::nullopt);
        CHECK_FALSE(sys::connectAgent(sys::AgentFlavor::Auto, reason));
        CHECK(reason == "No SSH agent is running (SSH_AUTH_SOCK is not set)");
    }
    Scratch dir;
    const std::string missing = (dir.path() / "gone.sock").string();
    ScopedEnv env("SSH_AUTH_SOCK", missing);
    CHECK_FALSE(sys::connectAgent(sys::AgentFlavor::OpenSsh, reason));
    CHECK(reason == "The SSH agent at " + missing + " is not reachable: No such file or directory");
}
