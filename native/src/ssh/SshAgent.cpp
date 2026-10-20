// SSH agent support: the user's agent (platform/sys/Agent.h) served to libssh over a socket pair, the identity count
// probe and agent forwarding (framed requests from an auth-agent@openssh.com channel).
#include "ssh/SshAgent.h"

#include <cstdint>

namespace st::ssh {

namespace {

constexpr unsigned char SSH_AGENTC_REQUEST_IDENTITIES = 11;
constexpr unsigned char SSH_AGENT_IDENTITIES_ANSWER = 12;
constexpr unsigned char SSH_AGENT_FAILURE = 5;

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

sys::AgentFlavor flavor(AgentKind kind) {
    switch (kind) {
        case AgentKind::Pageant: return sys::AgentFlavor::Pageant;
        case AgentKind::OpenSsh: return sys::AgentFlavor::OpenSsh;
        default: return sys::AgentFlavor::Auto;
    }
}

}  // namespace

std::unique_ptr<AgentClient> connectAgent(AgentKind kind, std::string& reason) {
    return sys::connectAgent(flavor(kind), reason);
}

int agentIdentityCount(AgentClient& client) {
    auto reply = client.request(std::string(1, static_cast<char>(SSH_AGENTC_REQUEST_IDENTITIES)));
    if (!reply || reply->size() < 5 || static_cast<unsigned char>((*reply)[0]) != SSH_AGENT_IDENTITIES_ANSWER) return -1;
    return static_cast<int>(getBe32(reply->data() + 1));
}

/* ---- relay ---------------------------------------------------------------------------- */

AgentRelay::AgentRelay(std::unique_ptr<AgentClient> client) : client_(std::move(client)) {}

AgentRelay::~AgentRelay() { stop(); }

sys::SocketHandle AgentRelay::start(std::string& error) {
    sys::SocketHandle a = sys::kInvalidSocket, b = sys::kInvalidSocket;
    if (!sys::makeSocketPair(a, b, error)) return sys::kInvalidSocket;
    mine_ = a;
    thread_ = std::thread([this] { run(); });
    return b;
}

void AgentRelay::stop() {
    stopping_ = true;
    sys::interruptSocket(mine_);  // wakes run() when it is blocked in recv (Windows: by closing the socket)
    if (thread_.joinable()) thread_.join();
    sys::closeSocket(mine_);
}

void AgentRelay::run() {
    const sys::SocketHandle sock = mine_;  // stop() may reset mine_ concurrently
    std::string buf;
    char tmp[4096];
    while (!stopping_) {
        const long n = sys::recvSome(sock, tmp, sizeof tmp);
        if (n <= 0) break;
        buf.append(tmp, static_cast<size_t>(n));
        while (buf.size() >= 4) {
            const uint32_t len = getBe32(buf.data());
            if (len > 256 * 1024) return;
            if (buf.size() < 4 + len) break;
            std::string payload = buf.substr(4, len);
            buf.erase(0, 4 + len);
            auto reply = client_->request(payload);
            std::string body = reply ? *reply : std::string(1, static_cast<char>(SSH_AGENT_FAILURE));
            if (stopping_ || !sys::sendAll(sock, be32(static_cast<uint32_t>(body.size())) + body)) return;
        }
    }
}

/* ---- forwarding ------------------------------------------------------------------------- */

std::string AgentForwarder::feed(const char* data, size_t len) {
    buf_.append(data, len);
    std::string out;
    while (buf_.size() >= 4) {
        const uint32_t n = getBe32(buf_.data());
        if (n > 256 * 1024) {
            buf_.clear();
            break;
        }
        if (buf_.size() < 4 + n) break;
        std::string payload = buf_.substr(4, n);
        buf_.erase(0, 4 + n);
        if (!client_) {
            std::string reason;
            client_ = connectAgent(kind_, reason);
        }
        std::optional<std::string> reply = client_ ? client_->request(payload) : std::nullopt;
        std::string body = reply ? *reply : std::string(1, static_cast<char>(SSH_AGENT_FAILURE));
        out += be32(static_cast<uint32_t>(body.size())) + body;
    }
    return out;
}

}  // namespace st::ssh
