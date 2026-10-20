// The shared part of TestExecProcess.h: a child's pipes pumped by background threads. Starting processes and the pipes
// themselves are per OS (native/tests/platform/<os>/support/TestExecProcess<Os>.cpp).
#include "TestExecProcess.h"

namespace st::test {

TestProcess::TestProcess(const std::filesystem::path& program, const std::vector<std::string>& args)
    : child_(detail::startProgram(program, args)) {}

TestProcess::~TestProcess() {
    detail::terminateProcess(child_);
    detail::releaseProcess(child_);
}

std::unique_ptr<ExecProcess> ExecProcess::spawn(const std::filesystem::path& shell, const std::filesystem::path& cwd,
                                                const std::string& command) {
    std::unique_ptr<ExecProcess> p(new ExecProcess());
    p->child_ = detail::spawnShellCommand(shell, cwd, command);
    ExecProcess* self = p.get();
    auto reader = [self](std::intptr_t pipe, std::string ExecProcess::*buf, bool ExecProcess::*done) {
        char tmp[65536];
        for (;;) {
            const long n = detail::readPipe(pipe, tmp, sizeof tmp);
            if (n <= 0) break;
            std::lock_guard lk(self->m_);
            (self->*buf).append(tmp, static_cast<size_t>(n));
        }
        std::lock_guard lk(self->m_);
        self->*done = true;
    };
    p->outThread_ = std::thread(reader, p->child_.stdoutRead, &ExecProcess::out_, &ExecProcess::outDone_);
    p->errThread_ = std::thread(reader, p->child_.stderrRead, &ExecProcess::err_, &ExecProcess::errDone_);
    p->inThread_ = std::thread([self] {
        for (;;) {
            std::string chunk;
            {
                std::unique_lock lk(self->m_);
                self->cv_.wait(lk, [&] { return self->stop_ || self->inEof_ || !self->in_.empty(); });
                if (self->stop_) break;
                if (self->in_.empty() && self->inEof_) break;
                chunk.swap(self->in_);
            }
            size_t off = 0;
            while (off < chunk.size()) {
                const long n = detail::writePipe(self->child_.stdinWrite, chunk.data() + off, chunk.size() - off);
                if (n <= 0) {
                    std::lock_guard lk(self->m_);
                    self->stop_ = true;
                    break;
                }
                off += static_cast<size_t>(n);
            }
        }
        std::lock_guard lk(self->m_);
        detail::closePipe(self->child_.stdinWrite);
    });
    return p;
}

ExecProcess::~ExecProcess() {
    {
        std::lock_guard lk(m_);
        stop_ = true;
    }
    cv_.notify_all();
    detail::terminateProcess(child_);
    if (inThread_.joinable()) inThread_.join();
    if (outThread_.joinable()) outThread_.join();
    if (errThread_.joinable()) errThread_.join();
    detail::closePipe(child_.stdinWrite);
    detail::closePipe(child_.stdoutRead);
    detail::closePipe(child_.stderrRead);
    detail::releaseProcess(child_);
}

void ExecProcess::writeInput(const std::string& data) {
    {
        std::lock_guard lk(m_);
        in_ += data;
    }
    cv_.notify_all();
}

void ExecProcess::closeInput() {
    {
        std::lock_guard lk(m_);
        inEof_ = true;
    }
    cv_.notify_all();
}

void ExecProcess::takeOutput(std::string& out, std::string& err, size_t maxOut) {
    std::lock_guard lk(m_);
    const size_t n = std::min(out_.size(), maxOut);
    out.append(out_, 0, n);
    out_.erase(0, n);
    err += err_;
    err_.clear();
}

bool ExecProcess::finished(int& exitCode) {
    {
        std::lock_guard lk(m_);
        if (!outDone_ || !errDone_ || !out_.empty() || !err_.empty()) return false;
    }
    if (!detail::processExited(child_)) return false;
    exitCode = child_.exitCode;
    return true;
}

}  // namespace st::test
