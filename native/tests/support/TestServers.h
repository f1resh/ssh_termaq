#pragma once
// In-process test servers for integration tests (built on libssh's server API + Winsock).
// All servers listen on 127.0.0.1 with an ephemeral port and run on their own threads.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sc::test {

struct TestSshServerOptions {
    std::string username = "test";
    std::string password = "secret";
    bool allowPassword = true;
    bool allowPublicKey = true;
    bool allowKbdInt = true;
    /** OpenSSH public key lines ("ssh-ed25519 AAAA... comment") accepted for publickey auth. */
    std::vector<std::string> authorizedKeys;
    /** keyboard-interactive: (prompt, expected answer) pairs asked in one round. Empty => single "Password: " prompt. */
    std::vector<std::pair<std::string, std::string>> kbdIntQuestions;
    /** Pre-auth banner ('' => none). */
    std::string banner;
    /** Host key type: "ed25519" | "rsa" | "ecdsa". A fresh key is generated per server instance unless hostKeyFile is set. */
    std::string hostKeyType = "ed25519";
    std::filesystem::path hostKeyFile;
    bool enableSftp = true;
    /** SFTP root directory (required for SFTP tests; paths are confined to it). */
    std::filesystem::path sftpRoot;
    bool allowTcpForwarding = true;
};

/**
 * Minimal SSH server for tests. Behaviour:
 *  - shell channel (with or without pty): line-oriented; for each received line (terminated by \r or \n) replies
 *      "echo: <line>\r\n", except these commands:
 *        "exit <n>"     -> sends exit-status n, EOF, closes the channel
 *        "env <NAME>"   -> value of env var NAME set via env requests ("<unset>" if missing)
 *        "size"         -> "<cols>x<rows>" from pty-req / window-change
 *        "term"         -> TERM from pty-req ("<none>" without pty)
 *        "user"         -> authenticated username
 *        "big <n>"      -> n bytes of 'x' followed by "\r\nEND\r\n"
 *        "signal"       -> waits for a signal request and replies "got signal <NAME>\r\n"
 *  - exec channel: "echo <text>" -> "<text>\n" exit 0; "exit <n>" -> exit n; anything else -> exit 127
 *  - direct-tcpip channels (local/dynamic forwarding) are connected to the requested host:port
 *  - tcpip-forward global requests (remote forwarding) are honoured on 127.0.0.1
 *  - "sftp" subsystem served from sftpRoot
 */
class TestSshServer {
public:
    explicit TestSshServer(TestSshServerOptions opts = {});
    ~TestSshServer();
    TestSshServer(const TestSshServer&) = delete;
    TestSshServer& operator=(const TestSshServer&) = delete;

    int port() const;
    std::string hostKeyType() const;  // e.g. "ssh-ed25519"
    /** Base64 of the wire-format host public key blob. */
    std::string hostKeyBase64() const;
    std::string hostKeyFingerprint() const;  // "SHA256:..."
    /** Port of the most recent remote-forward listener opened by the server (0 if none). */
    int lastRemoteForwardPort() const;
    /** Server-side event log for assertions, e.g. "auth:password:ok", "auth:publickey:fail", "pty:xterm-256color:80x24",
     *  "env:FOO=bar", "window-change:100x30", "signal:INT", "exec:echo hi", "subsystem:sftp", "direct-tcpip:127.0.0.1:1234",
     *  "tcpip-forward:127.0.0.1:0", "x11-req", "auth-agent-req". */
    std::vector<std::string> events() const;
    bool waitForEvent(const std::string& prefix, int timeoutMs) const;
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/** TCP echo server (for port-forwarding tests). Echoes everything back on each connection. */
class TestEchoServer {
public:
    TestEchoServer();
    ~TestEchoServer();
    int port() const;
    int connections() const;
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * Scriptable raw TCP server for telnet / rlogin / raw / proxy protocol tests: accepts ONE connection, records all
 * received bytes, and lets the test send bytes.
 */
class TestScriptServer {
public:
    TestScriptServer();
    ~TestScriptServer();
    int port() const;
    bool waitForConnection(int timeoutMs);
    /** All bytes received so far. */
    std::string received() const;
    /** Wait until received() contains `needle`. */
    bool waitForReceived(const std::string& needle, int timeoutMs) const;
    void send(const std::string& bytes);
    void closeClient();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/** Temporary directory removed on destruction. */
class TempDir {
public:
    TempDir();
    ~TempDir();
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace sc::test
