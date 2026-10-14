#include "TestExecProcess.h"

#include "core/Strings.h"
#include "core/Win.h"

#include <stdexcept>
#include <vector>

namespace st::test {

namespace {

/** MSVCRT / Cygwin command-line quoting of one argument. */
std::wstring quoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(*it);
        } else {
            out.append(backslashes, L'\\');
            out.push_back(*it);
        }
    }
    out.push_back(L'"');
    return out;
}

/** Current environment with `dir` prepended to PATH, as a CreateProcess block. */
std::vector<wchar_t> environmentWithPath(const std::wstring& dir) {
    std::vector<wchar_t> block;
    bool sawPath = false;
    wchar_t* env = GetEnvironmentStringsW();
    for (const wchar_t* p = env; *p; p += wcslen(p) + 1) {
        std::wstring entry = p;
        if (_wcsnicmp(entry.c_str(), L"PATH=", 5) == 0) {
            entry = L"PATH=" + dir + L";" + entry.substr(5);
            sawPath = true;
        }
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    FreeEnvironmentStringsW(env);
    if (!sawPath) {
        const std::wstring entry = L"PATH=" + dir;
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

}  // namespace

std::unique_ptr<ExecProcess> ExecProcess::spawn(const std::filesystem::path& shell, const std::filesystem::path& cwd,
                                                const std::string& command) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
    if (!CreatePipe(&inR, &inW, &sa, 1 << 20) || !CreatePipe(&outR, &outW, &sa, 1 << 20) ||
        !CreatePipe(&errR, &errW, &sa, 1 << 16)) {
        throw std::runtime_error("CreatePipe failed");
    }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = errW;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = quoteArg(shell.wstring()) + L" -c " + quoteArg(widen(command));
    auto env = environmentWithPath(shell.parent_path().wstring());
    const BOOL ok = CreateProcessW(shell.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                   env.data(), cwd.c_str(), &si, &pi);
    const DWORD err = GetLastError();
    CloseHandle(inR);
    CloseHandle(outW);
    CloseHandle(errW);
    if (!ok) {
        CloseHandle(inW);
        CloseHandle(outR);
        CloseHandle(errR);
        throw std::runtime_error("CreateProcess failed: " + winErrorMessage(err));
    }
    CloseHandle(pi.hThread);

    std::unique_ptr<ExecProcess> p(new ExecProcess());
    p->process_ = pi.hProcess;
    p->stdinWrite_ = inW;
    p->stdoutRead_ = outR;
    p->stderrRead_ = errR;
    ExecProcess* self = p.get();
    auto reader = [self](HANDLE h, std::string ExecProcess::*buf, bool ExecProcess::*done) {
        char tmp[65536];
        for (;;) {
            DWORD n = 0;
            if (!ReadFile(h, tmp, sizeof tmp, &n, nullptr) || n == 0) break;
            std::lock_guard lk(self->m_);
            (self->*buf).append(tmp, n);
        }
        std::lock_guard lk(self->m_);
        self->*done = true;
    };
    p->outThread_ = std::thread(reader, outR, &ExecProcess::out_, &ExecProcess::outDone_);
    p->errThread_ = std::thread(reader, errR, &ExecProcess::err_, &ExecProcess::errDone_);
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
                DWORD n = 0;
                if (!WriteFile(self->stdinWrite_, chunk.data() + off, static_cast<DWORD>(chunk.size() - off), &n, nullptr)) {
                    std::lock_guard lk(self->m_);
                    self->stop_ = true;
                    break;
                }
                off += n;
            }
        }
        std::lock_guard lk(self->m_);
        if (self->stdinWrite_) {
            CloseHandle(self->stdinWrite_);
            self->stdinWrite_ = nullptr;
        }
    });
    return p;
}

ExecProcess::~ExecProcess() {
    {
        std::lock_guard lk(m_);
        stop_ = true;
    }
    cv_.notify_all();
    if (process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) TerminateProcess(process_, 1);
    if (inThread_.joinable()) inThread_.join();
    if (outThread_.joinable()) outThread_.join();
    if (errThread_.joinable()) errThread_.join();
    if (stdinWrite_) CloseHandle(stdinWrite_);
    if (stdoutRead_) CloseHandle(stdoutRead_);
    if (stderrRead_) CloseHandle(stderrRead_);
    if (process_) CloseHandle(process_);
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
    if (WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) return false;
    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    exitCode = static_cast<int>(code);
    return true;
}

}  // namespace st::test
