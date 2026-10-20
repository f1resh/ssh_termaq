// POSIX parts of the test support (native/tests/support): the raw socket helpers of TestNet.h, posixShell() and
// findTool().
#include "TestExecProcess.h"
#include "TestNet.h"
#include "TestServers.h"

#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>

namespace st::test {

namespace net {

namespace {
int raw(Handle s) { return static_cast<int>(s); }
}  // namespace

bool pollSockets(std::vector<PollItem>& items, int timeoutMs) {
    std::vector<pollfd> fds;
    for (const auto& it : items) {
        short ev = 0;
        if (it.read) ev |= POLLIN;
        if (it.write) ev |= POLLOUT;
        fds.push_back(pollfd{raw(it.sock), ev, 0});
    }
    int rc;
    do {
        rc = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeoutMs);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0) return false;
    for (size_t i = 0; i < items.size(); ++i) {
        const short rev = fds[i].revents;
        items[i].readable = (rev & (POLLIN | POLLHUP | POLLERR)) != 0;
        items[i].writable = (rev & POLLOUT) != 0;
        items[i].failed = (rev & POLLERR) != 0;
    }
    return true;
}

int recvUrgent(Handle h, int timeoutMs) {
    const int s = raw(h);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        char b = 0;
        if (::recv(s, &b, 1, MSG_OOB) == 1) return static_cast<unsigned char>(b);
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) return -1;
        pollfd p{s, POLLPRI | POLLRDBAND, 0};
        ::poll(&p, 1, static_cast<int>(std::min<long long>(left, 20)));
    }
}

void abortiveClose(Socket& s) {
    linger lg{};
    lg.l_onoff = 1;
    lg.l_linger = 0;
    setsockopt(raw(s.get()), SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
    s.reset();
}

void setReceiveBuffer(Handle s, int bytes) { setsockopt(raw(s), SOL_SOCKET, SO_RCVBUF, &bytes, sizeof bytes); }

double processCpuSeconds() {
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
    auto seconds = [](const timeval& tv) { return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) / 1e6; };
    return seconds(ru.ru_utime) + seconds(ru.ru_stime);
}

}  // namespace net

std::filesystem::path posixShell() {
    const bool tools = ::access("/bin/ls", X_OK) == 0 && !findTool("scp").empty();
    return ::access("/bin/sh", X_OK) == 0 && tools ? std::filesystem::path("/bin/sh") : std::filesystem::path();
}

namespace {

std::string shellQuoted(const std::string& s) {
    std::string out = "'";
    for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

class PosixSshAgent final : public TestSshAgent {
public:
    PosixSshAgent(pid_t pid, const std::string& socket) : pid_(pid) {
        if (const char* old = std::getenv("SSH_AUTH_SOCK")) old_ = old;
        ::setenv("SSH_AUTH_SOCK", socket.c_str(), 1);
    }
    ~PosixSshAgent() override {
        ::kill(pid_, SIGTERM);  // it removes its socket
        if (old_) ::setenv("SSH_AUTH_SOCK", old_->c_str(), 1);
        else ::unsetenv("SSH_AUTH_SOCK");
    }

private:
    pid_t pid_;
    std::optional<std::string> old_;
};

}  // namespace

ScopedEnv::ScopedEnv(std::string name, const std::string& value) : name_(std::move(name)) {
    if (const char* v = std::getenv(name_.c_str())) old_ = v;
    ::setenv(name_.c_str(), value.c_str(), 1);
}

ScopedEnv::~ScopedEnv() {
    if (old_) ::setenv(name_.c_str(), old_->c_str(), 1);
    else ::unsetenv(name_.c_str());
}

std::unique_ptr<TestSshAgent> startSshAgent(const std::filesystem::path& keyFile, std::string& why) {
    if (findTool("ssh-agent").empty() || findTool("ssh-add").empty()) {
        why = "OpenSSH's ssh-agent and ssh-add are not installed";
        return nullptr;
    }
    std::error_code ec;
    std::filesystem::permissions(keyFile, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, ec);
    // In /tmp: a socket path must fit in 104 bytes, which the per-user temp folder nearly fills.
    static std::atomic<int> counter{0};
    const std::string socket = "/tmp/st-agent-" + std::to_string(::getpid()) + "-" + std::to_string(++counter) + ".sock";
    CommandResult r = runCommand("ssh-agent -s -a " + shellQuoted(socket));
    const size_t at = r.out.find("SSH_AGENT_PID=");
    if (r.code != 0 || at == std::string::npos) {
        why = "ssh-agent did not start: " + r.out;
        return nullptr;
    }
    auto agent = std::make_unique<PosixSshAgent>(static_cast<pid_t>(std::atoi(r.out.c_str() + at + 14)), socket);
    r = runCommand("ssh-add -q " + shellQuoted(keyFile.string()));
    if (r.code != 0) {
        why = "ssh-add failed: " + r.out;
        return nullptr;
    }
    return agent;
}

std::filesystem::path findTool(const std::string& name) {
    std::string dirs = std::getenv("PATH") ? std::getenv("PATH") : "";
    dirs += ":/opt/homebrew/bin:/usr/local/bin";
    size_t start = 0;
    while (start <= dirs.size()) {
        const size_t end = std::min(dirs.find(':', start), dirs.size());
        const std::string dir = dirs.substr(start, end - start);
        start = end + 1;
        if (dir.empty()) continue;
        const std::filesystem::path p = std::filesystem::path(dir) / name;
        if (::access(p.c_str(), X_OK) == 0) return p;
    }
    return {};
}

}  // namespace st::test
