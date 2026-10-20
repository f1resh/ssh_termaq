// Process and UI-thread services on Windows: SEH logging, OLE, the message loop, a named mutex per data folder and
// WM_COPYDATA forwarding to the running instance, MessageBox.
#include "platform/ui/App.h"

#include "core/Log.h"
#include "platform/detail/WinApi.h"
#include "platform/win/sys/WinText.h"
#include "platform/win/ui/HostWindowWin.h"

#include <objbase.h>
#include <ole2.h>
#include <shellapi.h>

#include <cstdio>
#include <iterator>

namespace st::ui {

namespace {

LONG WINAPI crashFilter(EXCEPTION_POINTERS* info) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "Unhandled SEH exception 0x%08lX at %p",
                  info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0UL,
                  info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr);
    applog::error(buf);
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void installCrashLogging() { SetUnhandledExceptionFilter(crashFilter); }

UiThread::UiThread() {
    // OLE (not just COM): drag & drop needs it on the UI thread.
    initialized_ = SUCCEEDED(OleInitialize(nullptr));
}

UiThread::~UiThread() {
    if (initialized_) OleUninitialize();
}

int runEventLoop() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

InstanceLock::InstanceLock(const std::string& key) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, widen("Local\\SSHTermaq-" + key).c_str());
    primary_ = mutex && GetLastError() != ERROR_ALREADY_EXISTS;
    handle_ = mutex;
}

InstanceLock::~InstanceLock() {
    if (handle_) {
        if (primary_) ReleaseMutex(static_cast<HANDLE>(handle_));
        CloseHandle(static_cast<HANDLE>(handle_));
    }
}

bool InstanceLock::forwardToPrimary() {
    HWND target = nullptr;
    for (int i = 0; i < 40 && !target; ++i) {
        target = FindWindowW(kWindowClass, nullptr);
        if (!target) Sleep(50);
    }
    if (!target) return false;
    wchar_t cwd[MAX_PATH * 4] = {};
    GetCurrentDirectoryW(static_cast<DWORD>(std::size(cwd)), cwd);
    std::wstring payload = std::wstring(cwd) + L'\0' + GetCommandLineW();
    COPYDATASTRUCT cds{};
    cds.dwData = kCopyDataCommandLine;
    cds.cbData = static_cast<DWORD>((payload.size() + 1) * sizeof(wchar_t));
    cds.lpData = payload.data();
    DWORD_PTR result = 0;
    // Let the running instance take the foreground.
    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    AllowSetForegroundWindow(pid);
    return SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 5000,
                               &result) != 0;
}

void showMessage(const std::string& text, MessageKind kind) {
    MessageBoxW(nullptr, widen(text).c_str(), L"SSHTermaq", kind == MessageKind::Error ? MB_ICONERROR : MB_OK | MB_ICONINFORMATION);
}

std::vector<std::string> splitCommandLine(const std::wstring& commandLine) {
    std::vector<std::string> out;
    if (commandLine.empty()) return out;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(commandLine.c_str(), &argc);
    if (!argv) return out;
    for (int i = 1; i < argc; ++i) out.push_back(narrow(argv[i]));
    LocalFree(argv);
    return out;
}

}  // namespace st::ui
