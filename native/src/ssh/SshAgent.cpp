#include "ssh/SshAgent.h"

#include "core/Log.h"
#include "core/Strings.h"
#include "net/Socket.h"

#include <aclapi.h>
#include <sddl.h>

#include <cstdio>
#include <vector>

namespace st::ssh {

namespace {

constexpr size_t kAgentMaxMsgLen = 8192;  // Pageant's shared-memory limit
constexpr ULONG_PTR kAgentCopyDataId = 0x804e50ba;
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

/* ---- Pageant ------------------------------------------------------------- */

class PageantClient final : public AgentClient {
public:
    explicit PageantClient(HWND hwnd) : hwnd_(hwnd) {}
    std::string name() const override { return "Pageant"; }

    std::optional<std::string> request(const std::string& payload) override {
        if (payload.size() + 4 > kAgentMaxMsgLen) return std::nullopt;
        HWND hwnd = FindWindowW(L"Pageant", L"Pageant");
        if (!hwnd) return std::nullopt;

        // The file mapping must be owned by the current user: Pageant verifies the owner SID.
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return std::nullopt;
        std::vector<char> userBuf(256);
        DWORD needed = 0;
        if (!GetTokenInformation(token, TokenUser, userBuf.data(), static_cast<DWORD>(userBuf.size()), &needed)) {
            userBuf.resize(needed);
            if (!GetTokenInformation(token, TokenUser, userBuf.data(), static_cast<DWORD>(userBuf.size()), &needed)) {
                CloseHandle(token);
                return std::nullopt;
            }
        }
        CloseHandle(token);
        PSID userSid = reinterpret_cast<TOKEN_USER*>(userBuf.data())->User.Sid;

        SECURITY_DESCRIPTOR sd;
        InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorOwner(&sd, userSid, FALSE);
        // Grant access to the owner only.
        EXPLICIT_ACCESS_W ea{};
        ea.grfAccessPermissions = GENERIC_ALL;
        ea.grfAccessMode = SET_ACCESS;
        ea.grfInheritance = NO_INHERITANCE;
        ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
        ea.Trustee.ptstrName = static_cast<LPWSTR>(userSid);
        PACL acl = nullptr;
        if (SetEntriesInAclW(1, &ea, nullptr, &acl) == ERROR_SUCCESS) SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE);
        SECURITY_ATTRIBUTES sa{sizeof(sa), &sd, FALSE};

        char mapName[64];
        std::snprintf(mapName, sizeof mapName, "PageantRequest%08lx", GetCurrentThreadId());
        HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, kAgentMaxMsgLen, mapName);
        if (acl) LocalFree(acl);
        if (!map) return std::nullopt;
        auto* view = static_cast<char*>(MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, 0));
        if (!view) {
            CloseHandle(map);
            return std::nullopt;
        }
        const std::string framed = be32(static_cast<uint32_t>(payload.size())) + payload;
        memcpy(view, framed.data(), framed.size());

        COPYDATASTRUCT cds{};
        cds.dwData = kAgentCopyDataId;
        cds.cbData = static_cast<DWORD>(strlen(mapName) + 1);
        cds.lpData = mapName;
        DWORD_PTR result = 0;
        std::optional<std::string> reply;
        if (SendMessageTimeoutW(hwnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 30000, &result) &&
            result != 0) {
            const uint32_t len = getBe32(view);
            if (len > 0 && len + 4 <= kAgentMaxMsgLen) reply = std::string(view + 4, len);
        }
        UnmapViewOfFile(view);
        CloseHandle(map);
        return reply;
    }

private:
    HWND hwnd_;
};

/* ---- Windows OpenSSH agent --------------------------------------------------- */

class OpenSshPipeClient final : public AgentClient {
public:
    std::string name() const override { return "OpenSSH agent"; }

    std::optional<std::string> request(const std::string& payload) override {
        HANDLE pipe = CreateFileW(L"\\\\.\\pipe\\openssh-ssh-agent", GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return std::nullopt;
        std::optional<std::string> reply;
        const std::string framed = be32(static_cast<uint32_t>(payload.size())) + payload;
        DWORD n = 0;
        if (WriteFile(pipe, framed.data(), static_cast<DWORD>(framed.size()), &n, nullptr) && n == framed.size()) {
            char hdr[4];
            if (readAll(pipe, hdr, 4)) {
                const uint32_t len = getBe32(hdr);
                if (len <= 256 * 1024) {
                    std::string body(len, '\0');
                    if (len == 0 || readAll(pipe, body.data(), len)) reply = std::move(body);
                }
            }
        }
        CloseHandle(pipe);
        return reply;
    }

    static bool available() {
        return WaitNamedPipeW(L"\\\\.\\pipe\\openssh-ssh-agent", 0) || GetLastError() == ERROR_SEM_TIMEOUT;
    }

private:
    static bool readAll(HANDLE h, char* buf, DWORD len) {
        DWORD got = 0;
        while (got < len) {
            DWORD n = 0;
            if (!ReadFile(h, buf + got, len - got, &n, nullptr) || n == 0) return false;
            got += n;
        }
        return true;
    }
};

}  // namespace

std::unique_ptr<AgentClient> AgentClient::connect(AgentKind kind, std::string& reason) {
    if (kind == AgentKind::Pageant || kind == AgentKind::Auto) {
        if (HWND hwnd = FindWindowW(L"Pageant", L"Pageant")) return std::make_unique<PageantClient>(hwnd);
        if (kind == AgentKind::Pageant) {
            reason = "Pageant is not running";
            return nullptr;
        }
    }
    if (OpenSshPipeClient::available()) return std::make_unique<OpenSshPipeClient>();
    reason = kind == AgentKind::OpenSsh ? "The OpenSSH Authentication Agent service is not running"
                                        : "No SSH agent (Pageant or OpenSSH agent) is running";
    return nullptr;
}

int AgentClient::identityCount() {
    auto reply = request(std::string(1, static_cast<char>(SSH_AGENTC_REQUEST_IDENTITIES)));
    if (!reply || reply->size() < 5 || static_cast<unsigned char>((*reply)[0]) != SSH_AGENT_IDENTITIES_ANSWER) return -1;
    return static_cast<int>(getBe32(reply->data() + 1));
}

/* ---- relay ---------------------------------------------------------------------------- */

AgentRelay::AgentRelay(std::unique_ptr<AgentClient> client) : client_(std::move(client)) {}

AgentRelay::~AgentRelay() { stop(); }

SOCKET AgentRelay::start(std::string& error) {
    SOCKET a = INVALID_SOCKET, b = INVALID_SOCKET;
    if (!makeSocketPair(a, b, error)) return INVALID_SOCKET;
    mine_ = a;
    thread_ = std::thread([this] { run(); });
    return b;
}

void AgentRelay::stop() {
    stopping_ = true;
    // On Windows shutdown() does not reliably wake a thread blocked in recv(); closing the socket does.
    closeSocket(mine_);
    if (thread_.joinable()) thread_.join();
}

void AgentRelay::run() {
    const SOCKET sock = mine_;  // stop() may reset mine_ concurrently
    std::string buf;
    char tmp[4096];
    while (!stopping_) {
        int n = recv(sock, tmp, sizeof tmp, 0);
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
            if (stopping_ || !sendAll(sock, be32(static_cast<uint32_t>(body.size())) + body)) return;
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
            client_ = AgentClient::connect(kind_, reason);
        }
        std::optional<std::string> reply = client_ ? client_->request(payload) : std::nullopt;
        std::string body = reply ? *reply : std::string(1, static_cast<char>(SSH_AGENT_FAILURE));
        out += be32(static_cast<uint32_t>(body.size())) + body;
    }
    return out;
}

}  // namespace st::ssh
