#pragma once
// Processes for tests: a child process whose stdin / stdout / stderr are pipes, pumped by background threads, for
// TestSshServer exec channels that run real commands (through posixShell(): Git for Windows' sh.exe, which brings GNU ls
// and OpenSSH's scp; /bin/sh elsewhere); shell command lines run to completion; programs that live as long as a test (an
// SSH agent). Pumping is shared; starting processes and their pipes are per OS (the `detail` functions below, in
// native/tests/platform/<os>/support/TestExecProcess<Os>.cpp).
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace st::test {

struct CommandResult {
    int code = -1;    // exit code; -1 when the command could not be started
    std::string out;  // stdout and stderr
};
/** Runs a command line through the system shell (Windows: cmd.exe, via _popen; POSIX: /bin/sh, via popen) and waits for
 *  it. */
CommandResult runCommand(const std::string& commandLine);

namespace detail {

constexpr std::intptr_t kNoHandle = -1;

/** A started process (Windows: process and pipe HANDLEs; POSIX: pid, the leader of its own process group, and pipe
 *  descriptors). */
struct ChildProcess {
    std::intptr_t process = kNoHandle;
    std::intptr_t stdinWrite = kNoHandle, stdoutRead = kNoHandle, stderrRead = kNoHandle;
    bool exited = false;
    int exitCode = -1;
};

/** Starts `program` with `args` (each passed as one argument) and our stdio. Throws if it cannot start. */
ChildProcess startProgram(const std::filesystem::path& program, const std::vector<std::string>& args);
/** Starts `<shell> -c <command>` in cwd with pipes for stdio and the shell's folder first on PATH. Throws on failure. */
ChildProcess spawnShellCommand(const std::filesystem::path& shell, const std::filesystem::path& cwd,
                               const std::string& command);
/** Blocking pipe I/O: bytes moved, 0 = end of the pipe, < 0 = error. */
long readPipe(std::intptr_t pipe, char* buf, size_t n);
long writePipe(std::intptr_t pipe, const char* data, size_t n);
void closePipe(std::intptr_t& pipe);
/** True once the process has exited (sets exited and exitCode). */
bool processExited(ChildProcess& child);
/** Ends the process if it still runs (POSIX: its process group), waiting up to 3 s for it. */
void terminateProcess(ChildProcess& child);
/** Lets go of an ended process. */
void releaseProcess(ChildProcess& child);

}  // namespace detail

/** A program started for a test (each argument quoted) and killed when the object goes away. Throws if it cannot start. */
class TestProcess {
public:
    TestProcess(const std::filesystem::path& program, const std::vector<std::string>& args);
    ~TestProcess();
    TestProcess(const TestProcess&) = delete;
    TestProcess& operator=(const TestProcess&) = delete;

private:
    detail::ChildProcess child_;
};

/** True while PuTTY's Pageant runs (its window exists; never on other systems). */
bool pageantRunning();

class ExecProcess {
public:
    /** Runs `<shell> -c <command>` in cwd with the shell's folder first on PATH. Throws on failure. */
    static std::unique_ptr<ExecProcess> spawn(const std::filesystem::path& shell, const std::filesystem::path& cwd,
                                              const std::string& command);
    ~ExecProcess();
    ExecProcess(const ExecProcess&) = delete;
    ExecProcess& operator=(const ExecProcess&) = delete;

    /** Queues data for the child's stdin. */
    void writeInput(const std::string& data);
    /** Closes the child's stdin once queued input is written. */
    void closeInput();
    /** Moves available stdout / stderr into the given strings. */
    void takeOutput(std::string& out, std::string& err, size_t maxOut);
    /** True once both output pipes are drained and the process has exited; sets exitCode. */
    bool finished(int& exitCode);

private:
    ExecProcess() = default;
    detail::ChildProcess child_;
    std::thread outThread_, errThread_, inThread_;
    std::mutex m_;
    std::condition_variable cv_;
    std::string out_, err_, in_;
    bool inEof_ = false, outDone_ = false, errDone_ = false, stop_ = false;
};

}  // namespace st::test
