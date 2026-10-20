#pragma once
// The user's SSH agent (Windows: PuTTY's Pageant via WM_COPYDATA + shared memory, or the Windows OpenSSH agent's
// named pipe \\.\pipe\openssh-ssh-agent; macOS: the Unix socket in SSH_AUTH_SOCK, whatever the flavor).
#include <memory>
#include <optional>
#include <string>

namespace st::sys {

class AgentConnection {
public:
    virtual ~AgentConnection() = default;
    /** Sends one agent request (payload WITHOUT the 4-byte length prefix); returns the reply payload. */
    virtual std::optional<std::string> request(const std::string& payload) = 0;
    /** "Pageant", "OpenSSH agent", "SSH agent" (SSH_AUTH_SOCK), ... */
    virtual std::string name() const = 0;
};

enum class AgentFlavor { Auto, Pageant, OpenSsh };

/** Connects to the requested agent ('Auto' = the one available). nullptr + a readable `reason` when unavailable. */
std::unique_ptr<AgentConnection> connectAgent(AgentFlavor flavor, std::string& reason);

}  // namespace st::sys
