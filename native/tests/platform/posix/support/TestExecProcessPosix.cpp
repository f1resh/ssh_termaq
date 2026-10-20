// POSIX parts of TestExecProcess.h: /bin/sh command lines (popen), posix_spawn with pipes, each child the leader of a
// process group of its own (so ending it ends what it started, e.g. both sides of `ls | cat`).
#include "TestExecProcess.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

extern char** environ;

namespace st::test {

namespace {

/** A pipe whose ends are not inherited (the child gets its end through posix_spawn's dup2). */
void makePipe(int& readEnd, int& writeEnd) {
    int p[2];
    if (::pipe(p) != 0) throw std::runtime_error(std::string("pipe failed: ") + std::strerror(errno));
    ::fcntl(p[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(p[1], F_SETFD, FD_CLOEXEC);
    readEnd = p[0];
    writeEnd = p[1];
}

/** Our environment, with `dir` first on PATH when given. */
std::vector<std::string> environmentWithPath(const std::string& dir) {
    std::vector<std::string> env;
    bool sawPath = false;
    for (char** e = environ; e && *e; ++e) {
        std::string entry = *e;
        if (!dir.empty() && entry.rfind("PATH=", 0) == 0) {
            entry = "PATH=" + dir + ":" + entry.substr(5);
            sawPath = true;
        }
        env.push_back(std::move(entry));
    }
    if (!dir.empty() && !sawPath) env.push_back("PATH=" + dir);
    return env;
}

/** Starts `args[0]` (a path) in a process group of its own with default signal handling. stdio[i] < 0: ours. */
pid_t spawnProcess(const std::vector<std::string>& args, const std::vector<std::string>& env,
                   const std::filesystem::path& cwd, const int (&stdio)[3]) {
    // A write to a pipe whose reader is gone fails with EPIPE instead of ending the test run.
    ::signal(SIGPIPE, SIG_IGN);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    for (int i = 0; i < 3; ++i) {
        if (stdio[i] >= 0) posix_spawn_file_actions_adddup2(&actions, stdio[i], i);
        else posix_spawn_file_actions_addinherit_np(&actions, i);
    }
    if (!cwd.empty()) posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setpgroup(&attr, 0);
    // Only stdio reaches the child (POSIX_SPAWN_CLOEXEC_DEFAULT): no other test's pipes or sockets.
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_CLOEXEC_DEFAULT);

    std::vector<char*> argv, envp;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    for (const auto& e : env) envp.push_back(const_cast<char*>(e.c_str()));
    envp.push_back(nullptr);
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, args[0].c_str(), &actions, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) throw std::runtime_error("Cannot start " + args[0] + ": " + std::strerror(rc));
    return pid;
}

}  // namespace

CommandResult runCommand(const std::string& commandLine) {
    CommandResult r;
    FILE* p = ::popen(("exec 2>&1\n" + commandLine).c_str(), "r");  // stderr too, for every command of the line
    if (!p) return r;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) r.out.append(buf, n);
    const int status = ::pclose(p);
    r.code = status >= 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return r;
}

bool pageantRunning() { return false; }

namespace detail {

ChildProcess startProgram(const std::filesystem::path& program, const std::vector<std::string>& args) {
    std::vector<std::string> argv{program.string()};
    argv.insert(argv.end(), args.begin(), args.end());
    ChildProcess c;
    c.process = spawnProcess(argv, environmentWithPath({}), {}, {-1, -1, -1});
    return c;
}

ChildProcess spawnShellCommand(const std::filesystem::path& shell, const std::filesystem::path& cwd,
                               const std::string& command) {
    int fds[6] = {-1, -1, -1, -1, -1, -1};  // stdin read/write, stdout read/write, stderr read/write
    auto closeAll = [&](std::initializer_list<int> which) {
        for (int i : which)
            if (fds[i] >= 0) ::close(fds[i]);
    };
    pid_t pid = -1;
    try {
        makePipe(fds[0], fds[1]);
        makePipe(fds[2], fds[3]);
        makePipe(fds[4], fds[5]);
        pid = spawnProcess({shell.string(), "-c", command}, environmentWithPath(shell.parent_path().string()), cwd,
                           {fds[0], fds[3], fds[5]});
    } catch (...) {
        closeAll({0, 1, 2, 3, 4, 5});
        throw;
    }
    closeAll({0, 3, 5});
    ChildProcess c;
    c.process = pid;
    c.stdinWrite = fds[1];
    c.stdoutRead = fds[2];
    c.stderrRead = fds[4];
    return c;
}

long readPipe(std::intptr_t pipe, char* buf, size_t n) {
    for (;;) {
        const ssize_t got = ::read(static_cast<int>(pipe), buf, n);
        if (got >= 0 || errno != EINTR) return static_cast<long>(got);
    }
}

long writePipe(std::intptr_t pipe, const char* data, size_t n) {
    for (;;) {
        const ssize_t put = ::write(static_cast<int>(pipe), data, n);
        if (put >= 0 || errno != EINTR) return static_cast<long>(put);
    }
}

void closePipe(std::intptr_t& pipe) {
    if (pipe != kNoHandle) ::close(static_cast<int>(pipe));
    pipe = kNoHandle;
}

bool processExited(ChildProcess& child) {
    if (child.exited) return true;
    if (child.process == kNoHandle) return false;
    int status = 0;
    pid_t r;
    do {
        r = ::waitpid(static_cast<pid_t>(child.process), &status, WNOHANG);
    } while (r < 0 && errno == EINTR);
    if (r == 0) return false;
    child.exited = true;
    child.exitCode = r < 0 ? -1 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return true;
}

void terminateProcess(ChildProcess& child) {
    if (child.process == kNoHandle || processExited(child)) return;
    const auto group = static_cast<pid_t>(child.process);
    ::killpg(group, SIGTERM);
    for (int i = 0; i < 60 && !processExited(child); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (processExited(child)) return;
    ::killpg(group, SIGKILL);
    int status = 0;
    while (::waitpid(group, &status, 0) < 0 && errno == EINTR) {
    }
    child.exited = true;
    child.exitCode = 128 + SIGKILL;
}

void releaseProcess(ChildProcess& child) {
    if (child.process != kNoHandle && !child.exited) {
        int status = 0;
        while (::waitpid(static_cast<pid_t>(child.process), &status, 0) < 0 && errno == EINTR) {
        }
    }
    child.process = kNoHandle;
}

}  // namespace detail

}  // namespace st::test
