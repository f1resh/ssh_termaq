// The user's SSH agent on POSIX: the Unix socket named by SSH_AUTH_SOCK - OpenSSH's ssh-agent (launchd starts one for
// every login on macOS) or any agent serving the same protocol there (PuTTY's Unix Pageant, 1Password, Secretive).
// Every flavor means that socket: on Unix, Pageant serves it too.
#include "platform/sys/Agent.h"

#include "platform/sys/Errors.h"
#include "platform/sys/Net.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace st::sys {

namespace {

std::string be32(uint32_t v) {
    std::string s(4, '\0');
    s[0] = static_cast<char>(v >> 24);
    s[1] = static_cast<char>(v >> 16);
    s[2] = static_cast<char>(v >> 8);
    s[3] = static_cast<char>(v);
    return s;
}

uint32_t getBe32(const char* p) {
    auto u = [&](int i) { return static_cast<uint32_t>(static_cast<unsigned char>(p[i])); };
    return (u(0) << 24) | (u(1) << 16) | (u(2) << 8) | u(3);
}

class SocketAgentClient final : public AgentConnection {
public:
    explicit SocketAgentClient(std::string path) : path_(std::move(path)) {}
    std::string name() const override { return "SSH agent"; }

    std::optional<std::string> request(const std::string& payload) override {
        // A connection per request, as with the Windows agents: nothing to lose when the agent restarts.
        std::string error;
        SocketHandle s = connectUnix(path_, error);
        if (s == kInvalidSocket) return std::nullopt;
        std::optional<std::string> reply;
        if (sendAll(s, be32(static_cast<uint32_t>(payload.size())) + payload)) {
            char hdr[4];
            if (readAll(s, hdr, 4)) {
                const uint32_t len = getBe32(hdr);
                if (len <= 256 * 1024) {
                    std::string body(len, '\0');
                    if (len == 0 || readAll(s, body.data(), len)) reply = std::move(body);
                }
            }
        }
        closeSocket(s);
        return reply;
    }

private:
    static bool readAll(SocketHandle s, char* buf, size_t len) {
        size_t got = 0;
        while (got < len) {
            const long n = recvSome(s, buf + got, len - got);
            if (n <= 0) return false;
            got += static_cast<size_t>(n);
        }
        return true;
    }

    std::string path_;
};

}  // namespace

std::unique_ptr<AgentConnection> connectAgent(AgentFlavor, std::string& reason) {
    const char* path = std::getenv("SSH_AUTH_SOCK");
    if (!path || !*path) {
        reason = "No SSH agent is running (SSH_AUTH_SOCK is not set)";
        return nullptr;
    }
    std::string error;
    SocketHandle probe = connectUnix(path, error);
    if (probe == kInvalidSocket) {
        reason = "The SSH agent at " + std::string(path) + " is not reachable: " + error;
        return nullptr;
    }
    closeSocket(probe);
    return std::make_unique<SocketAgentClient>(path);
}

}  // namespace st::sys
