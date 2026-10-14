#pragma once
// A child process whose stdin / stdout / stderr are pipes, pumped by background threads, for TestSshServer exec
// channels that run real commands (e.g. through Git for Windows' sh.exe, which brings GNU ls and OpenSSH's scp).
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace sc::test {

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
    void* process_ = nullptr;
    void* stdinWrite_ = nullptr;
    void* stdoutRead_ = nullptr;
    void* stderrRead_ = nullptr;
    std::thread outThread_, errThread_, inThread_;
    std::mutex m_;
    std::condition_variable cv_;
    std::string out_, err_, in_;
    bool inEof_ = false, outDone_ = false, errDone_ = false, stop_ = false;
};

}  // namespace sc::test
