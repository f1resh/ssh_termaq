// Local proxy command on POSIX: `/bin/sh -c command` started with posix_spawn in a process group of its own; its
// stdio pipes are relayed through a socket pair by three threads, as on Windows. The group is ended (SIGTERM, then
// SIGKILL) when our end of the pair is closed - by anyone, libssh included - or detached with terminate.
#include "platform/sys/Process.h"

#include "platform/sys/Errors.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

extern char** environ;

namespace st::sys {

namespace {

/** Shared state of one command; owned by the registry and the helper threads. */
struct CommandRelay {
    SocketHandle relaySock = kInvalidSocket;  // our end of the socket pair (the caller holds the other end)
    int stdinWrite = -1, stdoutRead = -1, stderrRead = -1;
    pid_t pid = -1;  // also the process group
    dev_t sockDev = 0;
    ino_t sockIno = 0;  // identity of the caller's socket, so a reused descriptor number is not mistaken for it

    std::mutex logMu;
    std::function<void(const std::string&)> onStderrLine;  // empty once detached

    std::atomic<bool> exited{false};
    std::atomic<bool> killed{false};

    ~CommandRelay() {
        closeSocket(relaySock);
        for (int f : {stdinWrite, stdoutRead, stderrRead})
            if (f >= 0) ::close(f);
    }
    void log(const std::string& line) {
        std::lock_guard lock(logMu);
        if (onStderrLine) onStderrLine(line);
    }
    void detach() {
        std::lock_guard lock(logMu);
        onStderrLine = nullptr;
    }
};

/** Ends the command's process group: SIGTERM, and SIGKILL a second later if it is still there. */
void kill(const std::shared_ptr<CommandRelay>& r) {
    if (r->killed.exchange(true) || r->exited) return;
    ::killpg(r->pid, SIGTERM);
    std::thread([r] {
        for (int i = 0; i < 20 && !r->exited; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!r->exited) ::killpg(r->pid, SIGKILL);
    }).detach();
}

std::mutex g_relayMu;
std::map<SocketHandle, std::shared_ptr<CommandRelay>> g_relays;

bool sameSocket(SocketHandle s, const CommandRelay& r) {
    struct stat st {};
    return ::fstat(static_cast<int>(s), &st) == 0 && st.st_dev == r.sockDev && st.st_ino == r.sockIno;
}

/** The relay of socket `s`, when `s` still is that socket (a closed number now reused for another is not). */
std::shared_ptr<CommandRelay> findRelay(SocketHandle s, bool remove) {
    std::lock_guard lock(g_relayMu);
    auto it = g_relays.find(s);
    if (it == g_relays.end()) return nullptr;
    std::shared_ptr<CommandRelay> r = it->second;
    const bool same = sameSocket(s, *r);
    if (remove || !same) g_relays.erase(it);
    return same ? r : nullptr;
}

void forgetRelay(SocketHandle s, const std::shared_ptr<CommandRelay>& r) {
    std::lock_guard lock(g_relayMu);
    auto it = g_relays.find(s);
    if (it != g_relays.end() && it->second == r) g_relays.erase(it);
}

/** A pipe whose parent end is close-on-exec. `childReads`: the child gets the read end. */
bool makePipe(int& parentEnd, int& childEnd, bool childReads, std::string& error) {
    int p[2];
    if (::pipe(p) != 0) {
        error = "Unable to create pipe: " + lastErrorMessage();
        return false;
    }
    parentEnd = childReads ? p[1] : p[0];
    childEnd = childReads ? p[0] : p[1];
    ::fcntl(parentEnd, F_SETFD, FD_CLOEXEC);
    ::fcntl(childEnd, F_SETFD, FD_CLOEXEC);  // posix_spawn's dup2 hands the child a fresh, inheritable copy
    return true;
}

/** The environment for the command: ours, with the usual tool folders on PATH (apps start with a minimal one). */
std::vector<std::string> commandEnvironment() {
    std::vector<std::string> env;
    std::string path;
    for (char** e = environ; e && *e; ++e) {
        if (std::strncmp(*e, "PATH=", 5) == 0) path = *e + 5;
        else env.emplace_back(*e);
    }
    if (path.empty()) path = "/usr/bin:/bin:/usr/sbin:/sbin";
    for (const char* dir : {"/opt/homebrew/bin", "/usr/local/bin"}) {
        if ((":" + path + ":").find(std::string(":") + dir + ":") == std::string::npos) path += std::string(":") + dir;
    }
    env.push_back("PATH=" + path);
    return env;
}

}  // namespace

SocketHandle startProxyCommand(const std::string& command, std::function<void(const std::string& line)> onStderrLine,
                               std::string& error) {
    SocketHandle mine = kInvalidSocket, relay = kInvalidSocket;
    if (!makeSocketPair(mine, relay, error)) return kInvalidSocket;
    auto r = std::make_shared<CommandRelay>();
    r->relaySock = relay;
    r->onStderrLine = std::move(onStderrLine);
    struct stat st {};
    ::fstat(static_cast<int>(mine), &st);
    r->sockDev = st.st_dev;
    r->sockIno = st.st_ino;

    int childIn = -1, childOut = -1, childErr = -1;
    auto closeChildEnds = [&] {
        for (int* f : {&childIn, &childOut, &childErr}) {
            if (*f >= 0) ::close(*f);
            *f = -1;
        }
    };
    if (!makePipe(r->stdinWrite, childIn, true, error) || !makePipe(r->stdoutRead, childOut, false, error) ||
        !makePipe(r->stderrRead, childErr, false, error)) {
        closeChildEnds();
        closeSocket(mine);
        return kInvalidSocket;
    }

    // Only the three pipe ends reach the command (POSIX_SPAWN_CLOEXEC_DEFAULT); it gets a process group of its own
    // and default signal handling (we ignore SIGPIPE, and a child inheriting that would not stop when its reader
    // goes away).
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, childIn, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, childOut, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, childErr, STDERR_FILENO);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF |
                                        POSIX_SPAWN_CLOEXEC_DEFAULT);

    std::vector<std::string> env = commandEnvironment();
    std::vector<char*> envp;
    for (auto& e : env) envp.push_back(e.data());
    envp.push_back(nullptr);
    std::string shell = "/bin/sh", dashC = "-c", cmd = command;
    char* argv[] = {shell.data(), dashC.data(), cmd.data(), nullptr};

    pid_t pid = -1;
    const int rc = posix_spawn(&pid, "/bin/sh", &actions, &attr, argv, envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    closeChildEnds();
    if (rc != 0) {
        error = "Unable to start proxy command: " + errorMessage(static_cast<unsigned long>(rc));
        closeSocket(mine);
        return kInvalidSocket;
    }
    r->pid = pid;
    {
        std::lock_guard lock(g_relayMu);
        g_relays[mine] = r;
    }

    // Reaper: no zombie once the command exits.
    std::thread([r] {
        int status = 0;
        while (::waitpid(r->pid, &status, 0) < 0 && errno == EINTR) {
        }
        r->exited = true;
    }).detach();

    // socket -> process stdin. EOF/error on the socket means our end was closed: end the command.
    std::thread([r, mine] {
        char buf[16384];
        for (;;) {
            const long got = recvSome(r->relaySock, buf, sizeof(buf));
            if (got <= 0) break;
            const char* p = buf;
            size_t left = static_cast<size_t>(got);
            bool ok = true;
            while (left > 0) {
                const ssize_t written = ::write(r->stdinWrite, p, left);
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) {
                    ok = false;  // the command stopped reading (EPIPE: SIGPIPE is ignored, see NetInit)
                    break;
                }
                p += written;
                left -= static_cast<size_t>(written);
            }
            if (!ok) break;
        }
        ::close(r->stdinWrite);
        r->stdinWrite = -1;
        kill(r);
        shutdownBoth(r->relaySock);
        forgetRelay(mine, r);
    }).detach();

    // process stdout -> socket. EOF (process exited) half-closes the socket so the transport sees the close.
    std::thread([r] {
        char buf[16384];
        for (;;) {
            const ssize_t got = ::read(r->stdoutRead, buf, sizeof(buf));
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) break;
            if (!sendAll(r->relaySock, std::string_view(buf, static_cast<size_t>(got)))) break;
        }
        shutdownWrite(r->relaySock);
    }).detach();

    // process stderr -> callback, line by line.
    std::thread([r] {
        std::string pending;
        char buf[4096];
        auto flushLine = [&](std::string line) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (!line.empty()) r->log(line);
        };
        for (;;) {
            const ssize_t got = ::read(r->stderrRead, buf, sizeof(buf));
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) break;
            pending.append(buf, static_cast<size_t>(got));
            size_t pos;
            while ((pos = pending.find('\n')) != std::string::npos) {
                flushLine(pending.substr(0, pos));
                pending.erase(0, pos + 1);
            }
            if (pending.size() > 4096) {
                flushLine(pending);
                pending.clear();
            }
        }
        flushLine(pending);
    }).detach();

    return mine;
}

void detachProxyCommand(SocketHandle s, bool terminate) {
    if (s == kInvalidSocket) return;
    if (std::shared_ptr<CommandRelay> r = findRelay(s, terminate)) {
        r->detach();
        if (terminate) kill(r);
    }
}

void closeProxyCommandSocket(SocketHandle& s) {
    if (s == kInvalidSocket) return;
    std::shared_ptr<CommandRelay> r = findRelay(s, true);
    if (r) r->detach();
    closeSocket(s);
    if (r) kill(r);
}

}  // namespace st::sys
