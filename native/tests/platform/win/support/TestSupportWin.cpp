// Windows parts of the test support (native/tests/support): the raw socket helpers of TestNet.h, posixShell() and
// findTool().
#include "TestNet.h"
#include "TestServers.h"

#include "platform/detail/WinApi.h"
#include "platform/win/sys/WinText.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace st::test {

namespace net {

namespace {
SOCKET raw(Handle s) { return static_cast<SOCKET>(s); }
}  // namespace

bool pollSockets(std::vector<PollItem>& items, int timeoutMs) {
    std::vector<WSAPOLLFD> fds;
    for (const auto& it : items) {
        SHORT ev = 0;
        if (it.read) ev |= POLLRDNORM;
        if (it.write) ev |= POLLWRNORM;
        fds.push_back(WSAPOLLFD{raw(it.sock), ev, 0});
    }
    if (WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeoutMs) < 0) return false;
    for (size_t i = 0; i < items.size(); ++i) {
        const SHORT rev = fds[i].revents;
        items[i].readable = (rev & (POLLRDNORM | POLLHUP | POLLERR)) != 0;
        items[i].writable = (rev & POLLWRNORM) != 0;
        items[i].failed = (rev & POLLERR) != 0;
    }
    return true;
}

int recvUrgent(Handle h, int timeoutMs) {
    const SOCKET s = raw(h);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) return -1;
        fd_set except;
        FD_ZERO(&except);
        FD_SET(s, &except);
        const long slice = static_cast<long>(std::min<long long>(left, 100));
        timeval tv{0, slice * 1000};
        if (::select(0, nullptr, nullptr, &except, &tv) > 0) {
            char b = 0;
            if (::recv(s, &b, 1, MSG_OOB) == 1) return static_cast<unsigned char>(b);
        }
    }
}

void abortiveClose(Socket& s) {
    linger lg{};
    lg.l_onoff = 1;
    lg.l_linger = 0;
    setsockopt(raw(s.get()), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&lg), sizeof(lg));
    s.reset();
}

void setReceiveBuffer(Handle s, int bytes) {
    setsockopt(raw(s), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes));
}

double processCpuSeconds() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    auto seconds = [](const FILETIME& ft) {
        return static_cast<double>((static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 1e7;
    };
    return seconds(kernel) + seconds(user);
}

}  // namespace net

std::filesystem::path posixShell() {
    for (const wchar_t* dir : {L"C:\\Program Files\\Git\\usr\\bin", L"C:\\Program Files (x86)\\Git\\usr\\bin"}) {
        const std::filesystem::path d = dir;
        std::error_code ec;
        if (std::filesystem::exists(d / L"sh.exe", ec) && std::filesystem::exists(d / L"scp.exe", ec) &&
            std::filesystem::exists(d / L"ls.exe", ec)) {
            return d / L"sh.exe";
        }
    }
    return {};
}

std::unique_ptr<TestSshAgent> startSshAgent(const std::filesystem::path&, std::string& why) {
    why = "Windows reaches agents through Pageant and the OpenSSH agent service, not SSH_AUTH_SOCK";
    return nullptr;
}

std::filesystem::path findTool(const std::string& name) {
    for (const wchar_t* dir : {L"C:\\Program Files\\PuTTY", L"C:\\Windows\\System32\\OpenSSH"}) {
        const std::filesystem::path p = std::filesystem::path(dir) / std::filesystem::path(name + ".exe");
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return p;
    }
    return {};
}

ScopedEnv::ScopedEnv(std::string name, const std::string& value) : name_(std::move(name)) {
    // The variables sys::getEnv reads (GetEnvironmentVariableW), not only the C runtime's copy.
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetEnvironmentVariableW(widen(name_).c_str(), buf.data(), static_cast<DWORD>(buf.size()));
    if (n > 0 && n < buf.size()) old_ = narrow(std::wstring_view(buf.data(), n));
    SetEnvironmentVariableW(widen(name_).c_str(), widen(value).c_str());
}

ScopedEnv::~ScopedEnv() {
    SetEnvironmentVariableW(widen(name_).c_str(), old_ ? widen(*old_).c_str() : nullptr);
}

}  // namespace st::test
