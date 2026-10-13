#pragma once
// SSH agent access on Windows: PuTTY's Pageant (WM_COPYDATA + shared memory) and the Windows OpenSSH agent
// (named pipe \\.\pipe\openssh-ssh-agent).
//
// libssh only talks to an agent through a socket, so AgentRelay exposes an agent as one end of a loopback
// socket pair served by a background thread (used with ssh_set_agent_socket for authentication). For agent
// forwarding, AgentForwarder parses framed requests arriving on an "auth-agent@openssh.com" channel.
#include "core/Types.h"
#include "core/Win.h"

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace sc::ssh {

class AgentClient {
public:
    virtual ~AgentClient() = default;
    /** Sends one agent request (payload WITHOUT the 4-byte length prefix); returns the reply payload. */
    virtual std::optional<std::string> request(const std::string& payload) = 0;
    virtual std::string name() const = 0;

    /** Connects to the requested agent ('auto' = Pageant, then OpenSSH). nullptr + reason when unavailable. */
    static std::unique_ptr<AgentClient> connect(AgentKind kind, std::string& reason);
    /** Number of identities offered by the agent (-1 when the query failed). */
    int identityCount();
};

/** Serves an AgentClient on one end of a socket pair; libssh gets the other end (and owns/closes it). */
class AgentRelay {
public:
    explicit AgentRelay(std::unique_ptr<AgentClient> client);
    ~AgentRelay();
    AgentRelay(const AgentRelay&) = delete;
    AgentRelay& operator=(const AgentRelay&) = delete;

    /** Creates the pair and starts the thread. Returns the socket to hand to libssh, INVALID_SOCKET on failure. */
    SOCKET start(std::string& error);
    void stop();

private:
    void run();
    std::unique_ptr<AgentClient> client_;
    SOCKET mine_ = INVALID_SOCKET;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
};

/** Reassembles framed agent requests from a forwarded channel and produces framed replies. */
class AgentForwarder {
public:
    explicit AgentForwarder(AgentKind kind) : kind_(kind) {}
    /** Consumes channel data; returns framed reply bytes for every complete request. */
    std::string feed(const char* data, size_t len);

private:
    AgentKind kind_;
    std::unique_ptr<AgentClient> client_;
    std::string buf_;
};

}  // namespace sc::ssh
