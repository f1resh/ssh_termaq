// SSH agents on Windows: PuTTY's Pageant (WM_COPYDATA + a shared memory section owned by the user) and the Windows
// OpenSSH agent (named pipe \\.\pipe\openssh-ssh-agent).
#include "platform/sys/Agent.h"

#include "platform/detail/WinApi.h"

#include <aclapi.h>
#include <sddl.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace st::sys {

namespace {

constexpr size_t kAgentMaxMsgLen = 8192;  // Pageant's shared-memory limit
constexpr ULONG_PTR kAgentCopyDataId = 0x804e50ba;

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

class PageantClient final : public AgentConnection {
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

class OpenSshPipeClient final : public AgentConnection {
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

std::unique_ptr<AgentConnection> connectAgent(AgentFlavor kind, std::string& reason) {
    if (kind == AgentFlavor::Pageant || kind == AgentFlavor::Auto) {
        if (HWND hwnd = FindWindowW(L"Pageant", L"Pageant")) return std::make_unique<PageantClient>(hwnd);
        if (kind == AgentFlavor::Pageant) {
            reason = "Pageant is not running";
            return nullptr;
        }
    }
    if (OpenSshPipeClient::available()) return std::make_unique<OpenSshPipeClient>();
    reason = kind == AgentFlavor::OpenSsh ? "The OpenSSH Authentication Agent service is not running"
                                        : "No SSH agent (Pageant or OpenSSH agent) is running";
    return nullptr;
}

}  // namespace st::sys
