// Windows parts of TestExecProcess.h: cmd.exe command lines, CreateProcess with pipes, Pageant's window.
#include "TestExecProcess.h"

#include "platform/detail/WinApi.h"
#include "platform/sys/Errors.h"
#include "platform/win/sys/WinText.h"

#include <cstdio>
#include <stdexcept>
#include <vector>

namespace st::test {

namespace {

HANDLE handle(std::intptr_t h) { return reinterpret_cast<HANDLE>(h); }
std::intptr_t value(HANDLE h) { return reinterpret_cast<std::intptr_t>(h); }

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

CommandResult runCommand(const std::string& commandLine) {
    CommandResult r;
    // cmd.exe needs the whole command line wrapped in one extra pair of quotes.
    FILE* p = _popen(("\"" + commandLine + " 2>&1\"").c_str(), "r");
    if (!p) return r;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) r.out.append(buf, n);
    r.code = _pclose(p);
    return r;
}

bool pageantRunning() { return FindWindowW(L"Pageant", L"Pageant") != nullptr; }

namespace detail {

ChildProcess startProgram(const std::filesystem::path& program, const std::vector<std::string>& args) {
    std::wstring cmd = L"\"" + program.wstring() + L"\"";
    for (const auto& a : args) cmd += L" \"" + widen(a) + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        throw std::runtime_error("Cannot start " + narrow(program.wstring()) + ": " + sys::lastErrorMessage());
    CloseHandle(pi.hThread);
    ChildProcess c;
    c.process = value(pi.hProcess);
    return c;
}

ChildProcess spawnShellCommand(const std::filesystem::path& shell, const std::filesystem::path& cwd,
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
        throw std::runtime_error("CreateProcess failed: " + sys::errorMessage(err));
    }
    CloseHandle(pi.hThread);

    ChildProcess c;
    c.process = value(pi.hProcess);
    c.stdinWrite = value(inW);
    c.stdoutRead = value(outR);
    c.stderrRead = value(errR);
    return c;
}

long readPipe(std::intptr_t pipe, char* buf, size_t n) {
    DWORD got = 0;
    if (!ReadFile(handle(pipe), buf, static_cast<DWORD>(n), &got, nullptr)) return -1;
    return static_cast<long>(got);
}

long writePipe(std::intptr_t pipe, const char* data, size_t n) {
    DWORD written = 0;
    if (!WriteFile(handle(pipe), data, static_cast<DWORD>(n), &written, nullptr)) return -1;
    return static_cast<long>(written);
}

void closePipe(std::intptr_t& pipe) {
    if (pipe != kNoHandle) CloseHandle(handle(pipe));
    pipe = kNoHandle;
}

bool processExited(ChildProcess& child) {
    if (child.exited) return true;
    if (WaitForSingleObject(handle(child.process), 0) != WAIT_OBJECT_0) return false;
    DWORD code = 0;
    GetExitCodeProcess(handle(child.process), &code);
    child.exitCode = static_cast<int>(code);
    child.exited = true;
    return true;
}

void terminateProcess(ChildProcess& child) {
    if (child.process == kNoHandle || processExited(child)) return;
    TerminateProcess(handle(child.process), 1);
    WaitForSingleObject(handle(child.process), 3000);
}

void releaseProcess(ChildProcess& child) {
    if (child.process != kNoHandle) CloseHandle(handle(child.process));
    child.process = kNoHandle;
}

}  // namespace detail

}  // namespace st::test
