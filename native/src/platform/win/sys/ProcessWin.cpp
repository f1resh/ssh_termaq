// Local proxy command on Windows: the process' stdio pipes relayed through a loopback socket pair by three threads;
// a job object (KILL_ON_JOB_CLOSE) ends the whole process tree.
#include "platform/sys/Process.h"

#include "platform/detail/WinApi.h"
#include "platform/sys/Errors.h"
#include "platform/win/sys/WinText.h"

#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace st::sys {

namespace {

struct HandleCloser {
    void operator()(HANDLE h) const {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

/** Shared state of one local-command relay; destroyed when the registry and all relay threads released it. */
struct CommandRelay {
    SocketHandle relaySock = kInvalidSocket;  // our end of the socket pair (the caller holds the other end)
    UniqueHandle stdinWrite, stdoutRead, stderrRead, process, job;

    std::mutex logMu;
    std::function<void(const std::string&)> onStderrLine;  // empty once detached

    std::mutex killMu;
    bool killed = false;

    ~CommandRelay() { closeSocket(relaySock); }

    void log(const std::string& line) {
        std::lock_guard lock(logMu);
        if (onStderrLine) onStderrLine(line);
    }
    void detach() {
        std::lock_guard lock(logMu);
        onStderrLine = nullptr;
    }
    /** Terminates the whole process tree (job object with KILL_ON_JOB_CLOSE). */
    void kill() {
        std::lock_guard lock(killMu);
        if (killed) return;
        killed = true;
        if (process) TerminateProcess(process.get(), 1);
        job.reset();
    }
};

std::mutex g_relayMu;
std::map<SocketHandle, std::shared_ptr<CommandRelay>> g_relays;

std::shared_ptr<CommandRelay> takeRelay(SocketHandle s) {
    std::lock_guard lock(g_relayMu);
    auto it = g_relays.find(s);
    if (it == g_relays.end()) return nullptr;
    auto r = it->second;
    g_relays.erase(it);
    return r;
}

bool createPipe(UniqueHandle& parentEnd, UniqueHandle& childEnd, bool childReads, std::string& error) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE r = nullptr, w = nullptr;
    if (!CreatePipe(&r, &w, &sa, 0)) {
        error = "Unable to create pipe: " + lastErrorMessage();
        return false;
    }
    parentEnd.reset(childReads ? w : r);
    childEnd.reset(childReads ? r : w);
    SetHandleInformation(parentEnd.get(), HANDLE_FLAG_INHERIT, 0);
    return true;
}

}  // namespace

SocketHandle startProxyCommand(const std::string& command, std::function<void(const std::string& line)> onStderrLine,
                               std::string& error) {
    SocketHandle mine = kInvalidSocket, relay = kInvalidSocket;
    if (!makeSocketPair(mine, relay, error)) return kInvalidSocket;
    auto r = std::make_shared<CommandRelay>();
    r->relaySock = relay;
    r->onStderrLine = std::move(onStderrLine);

    UniqueHandle childIn, childOut, childErr;
    if (!createPipe(r->stdinWrite, childIn, true, error) || !createPipe(r->stdoutRead, childOut, false, error) ||
        !createPipe(r->stderrRead, childErr, false, error)) {
        closeSocket(mine);
        return kInvalidSocket;
    }

    // Only the three pipe ends may be inherited (other threads may have inheritable handles open).
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<unsigned char> attrBuf(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    HANDLE inherit[3] = {childIn.get(), childOut.get(), childErr.get()};
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr)) {
        error = "Unable to start proxy command: " + lastErrorMessage();
        closeSocket(mine);
        return kInvalidSocket;
    }
    struct AttrGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST a;
        ~AttrGuard() { DeleteProcThreadAttributeList(a); }
    } attrGuard{attrs};

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = childIn.get();
    si.StartupInfo.hStdOutput = childOut.get();
    si.StartupInfo.hStdError = childErr.get();
    si.lpAttributeList = attrs;

    wchar_t comspec[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH);
    std::wstring shell = (n > 0 && n < MAX_PATH) ? std::wstring(comspec, n) : std::wstring(L"cmd.exe");
    std::wstring cmdline = L"\"" + shell + L"\" /d /s /c \"" + widen(command) + L"\"";

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                        &si.StartupInfo, &pi)) {
        error = "Unable to start proxy command: " + lastErrorMessage();
        closeSocket(mine);
        return kInvalidSocket;
    }
    r->process.reset(pi.hProcess);
    UniqueHandle thread(pi.hThread);
    r->job.reset(CreateJobObjectW(nullptr, nullptr));
    if (r->job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(r->job.get(), JobObjectExtendedLimitInformation, &li, sizeof(li));
        AssignProcessToJobObject(r->job.get(), pi.hProcess);
    }
    ResumeThread(pi.hThread);
    childIn.reset();
    childOut.reset();
    childErr.reset();

    {
        std::lock_guard lock(g_relayMu);
        g_relays[mine] = r;
    }

    // socket -> process stdin. EOF/error on the socket means the transport closed its end: kill the command.
    std::thread([r, mine] {
        char buf[16384];
        for (;;) {
            const long got = recvSome(r->relaySock, buf, sizeof(buf));
            if (got <= 0) break;
            const char* p = buf;
            DWORD left = static_cast<DWORD>(got);
            bool ok = true;
            while (left > 0) {
                DWORD written = 0;
                if (!WriteFile(r->stdinWrite.get(), p, left, &written, nullptr)) {
                    ok = false;
                    break;
                }
                p += written;
                left -= written;
            }
            if (!ok) break;
        }
        r->stdinWrite.reset();
        r->kill();
        shutdownBoth(r->relaySock);
        std::lock_guard lock(g_relayMu);
        auto it = g_relays.find(mine);
        if (it != g_relays.end() && it->second == r) g_relays.erase(it);
    }).detach();

    // process stdout -> socket. EOF (process exited) half-closes the socket so the transport sees the close.
    std::thread([r] {
        char buf[16384];
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(r->stdoutRead.get(), buf, sizeof(buf), &got, nullptr) || got == 0) break;
            if (!sendAll(r->relaySock, std::string_view(buf, got))) break;
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
            DWORD got = 0;
            if (!ReadFile(r->stderrRead.get(), buf, sizeof(buf), &got, nullptr) || got == 0) break;
            pending.append(buf, got);
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
    if (terminate) {
        if (std::shared_ptr<CommandRelay> relay = takeRelay(s)) {
            relay->detach();
            relay->kill();
        }
        return;
    }
    std::lock_guard lock(g_relayMu);
    auto it = g_relays.find(s);
    if (it != g_relays.end()) it->second->detach();
}

void closeProxyCommandSocket(SocketHandle& s) {
    if (s == kInvalidSocket) return;
    std::shared_ptr<CommandRelay> relay = takeRelay(s);
    if (relay) relay->detach();
    closeSocket(s);
    if (relay) relay->kill();
}

}  // namespace st::sys
