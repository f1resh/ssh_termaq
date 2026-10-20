#pragma once
// Shared SSH connection setup used by the main SSH transport and by jump hosts:
// libssh session options -> key exchange over an already-connected socket -> host key verification ->
// authentication (PuTTY order: none, agent, key file, keyboard-interactive, password).
#include "platform/sys/Net.h"
#include "ssh/SshUtil.h"
#include "transport/Transport.h"

#include <atomic>
#include <memory>
#include <string>

namespace st::ssh {

class AgentRelay;

struct ConnectResult {
    SessionPtr session;
    std::string error;    // set on failure
    bool cancelled = false;  // the user cancelled a prompt / the connection was closed
    /**
     * On failure: the session that still owns the socket (freeing it closes the socket). Free it only once nothing
     * can reach the socket's number any more (cancellation, the proxy command registry): the OS reuses numbers.
     */
    SessionPtr failed;
};

class SshConnector {
public:
    /**
     * @param ctx      prompter / known hosts / credentials / listener of the owning transport
     * @param session  configuration to connect with (the main session or a jump host)
     * @param logPrefix prefix for event-log lines ("" or "Jump: ")
     */
    SshConnector(const TransportContext& ctx, const SessionConfig& session, std::string logPrefix,
                 const std::atomic<bool>& cancel);
    ~SshConnector();

    /** Connects over `sock` (ownership passes to libssh) and authenticates. Blocking. */
    ConnectResult run(sys::SocketHandle sock, int logLevel);

    /** Informational summary after key exchange, e.g. "SSH-2.0-OpenSSH_9.6 | aes256-gcm@openssh.com | curve25519-sha256". */
    const std::string& serverInfo() const { return serverInfo_; }

private:
    void log(LogLevel level, const std::string& msg) const;
    void local(const std::string& text) const;
    bool establish(ssh_session s, sys::SocketHandle sock, int logLevel, ConnectResult& r);
    bool applyOptions(ssh_session s, sys::SocketHandle sock, int logLevel, std::string& error);
    bool verifyHostKey(ssh_session s, std::string& error, bool& cancelled);
    bool authenticate(ssh_session s, std::string& error, bool& cancelled);
    int tryAgent(ssh_session s);
    int tryKeyFile(ssh_session s, bool& cancelled);
    int tryKbdInt(ssh_session s, bool& cancelled);
    int tryPassword(ssh_session s, bool& cancelled);
    /** Saved (or one-time) password not yet tried. */
    std::optional<std::string> takeStoredPassword();

    const TransportContext& ctx_;
    SessionConfig cfg_;
    std::string prefix_;
    const std::atomic<bool>& cancel_;
    std::string user_;
    std::string serverInfo_;
    bool storedPasswordUsed_ = false;
    bool bannerShown_ = false;
    std::unique_ptr<AgentRelay> agentRelay_;
};

}  // namespace st::ssh
