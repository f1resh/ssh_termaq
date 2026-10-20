#pragma once
// SSH agent support: the user's agent (platform/sys/Agent.h; on Windows PuTTY's Pageant or the Windows OpenSSH agent)
// for authentication and agent forwarding.
//
// libssh only talks to an agent through a socket, so AgentRelay exposes an agent as one end of a socket pair served
// by a background thread (used with ssh_set_agent_socket for authentication). For agent forwarding, AgentForwarder
// parses framed requests arriving on an "auth-agent@openssh.com" channel.
#include "core/Types.h"
#include "platform/sys/Agent.h"
#include "platform/sys/Net.h"

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace st::ssh {

using AgentClient = sys::AgentConnection;

/** Connects to the requested agent ('auto' = Pageant, then OpenSSH). nullptr + reason when unavailable. */
std::unique_ptr<AgentClient> connectAgent(AgentKind kind, std::string& reason);
/** Number of identities offered by the agent (-1 when the query failed). */
int agentIdentityCount(AgentClient& client);

/** Serves an AgentClient on one end of a socket pair; libssh gets the other end (and owns/closes it). */
class AgentRelay {
public:
    explicit AgentRelay(std::unique_ptr<AgentClient> client);
    ~AgentRelay();
    AgentRelay(const AgentRelay&) = delete;
    AgentRelay& operator=(const AgentRelay&) = delete;

    /** Creates the pair and starts the thread. Returns the socket to hand to libssh, kInvalidSocket on failure. */
    sys::SocketHandle start(std::string& error);
    void stop();

private:
    void run();
    std::unique_ptr<AgentClient> client_;
    sys::SocketHandle mine_ = sys::kInvalidSocket;
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

}  // namespace st::ssh
