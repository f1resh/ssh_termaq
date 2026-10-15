#include "app/App.h"

#include "app/BuildInfo.h"
#include "app/Cli.h"
#include "app/CoreHandlers.h"
#include "app/Dialogs.h"
#include "app/MainWindow.h"
#include "app/Paths.h"
#include "bridge/Bridge.h"
#include "core/Crypto.h"
#include "core/Log.h"
#include "core/Strings.h"
#include "core/Win.h"
#include "dnd/DragDrop.h"
#include "importers/ImportHandlers.h"
#include "localfs/LocalFsService.h"
#include "net/Socket.h"
#include "session/SessionManager.h"
#include "sftp/SftpService.h"
#include "store/Store.h"

#include <objbase.h>
#include <ole2.h>

#include <exception>
#include <filesystem>

namespace st {

namespace {

constexpr ULONG_PTR kCopyDataCommandLine = 0x53545243;  // must match MainWindow.cpp

LONG WINAPI crashFilter(EXCEPTION_POINTERS* info) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "Unhandled SEH exception 0x%08lX at %p",
                  info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0UL,
                  info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr);
    applog::error(buf);
    return EXCEPTION_CONTINUE_SEARCH;
}

void terminateHandler() {
    try {
        if (auto e = std::current_exception()) std::rethrow_exception(e);
        applog::error("std::terminate called without an active exception");
    } catch (const std::exception& e) {
        applog::error(std::string("std::terminate: ") + e.what());
    } catch (...) {
        applog::error("std::terminate: unknown exception");
    }
    std::abort();
}

/** Per-user + per-data-directory instance lock (portable copies and --data-dir profiles run side by side). */
class SingleInstance {
public:
    explicit SingleInstance(const std::filesystem::path& dataDir) {
        std::string key = toLower(narrow(dataDir.wstring()));
        std::string hash = hexEncode(crypto::sha256(key)).substr(0, 16);
        mutex_ = CreateMutexW(nullptr, TRUE, widen("Local\\SSHTermaq-" + hash).c_str());
        primary_ = mutex_ && GetLastError() != ERROR_ALREADY_EXISTS;
    }
    ~SingleInstance() {
        if (mutex_) {
            if (primary_) ReleaseMutex(mutex_);
            CloseHandle(mutex_);
        }
    }
    bool primary() const { return primary_; }

    /** Forwards this process' command line to the running instance. */
    static bool forward() {
        HWND target = nullptr;
        for (int i = 0; i < 40 && !target; ++i) {
            target = FindWindowW(L"SSHTermaqMainWindow", nullptr);
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

private:
    HANDLE mutex_ = nullptr;
    bool primary_ = false;
};

/** Best-effort frame theme/backdrop from saved settings so the window doesn't flash before the UI loads. */
void applyInitialAppearance(MainWindow& window, const Json& settings) {
    const Json appearance = settings.value("appearance", Json::object());
    const std::string theme = appearance.value("theme", std::string("system"));
    if (theme == "system")
        window.setThemeSource("system");
    else if (theme.find("light") != std::string::npos || theme.find("latte") != std::string::npos)
        window.setThemeSource("light");
    else
        window.setThemeSource("dark");
    window.setBackgroundMaterial(appearance.value("backgroundMaterial", std::string("none")));
    const double scale = appearance.value("uiScale", 1.0);
    if (scale > 0) window.setUiScale(scale);
}

void queueCliRequest(const std::vector<std::string>& args, const std::filesystem::path& cwd, Store& store,
                     OpenRequestQueue& queue) {
    if (args.empty()) return;
    CliResult r = parseCommandLine(args, store, cwd);
    for (const auto& w : r.warnings) applog::warn("Command line: " + w);
    for (const auto& e : r.errors) applog::warn("Command line error: " + e);
    if (r.request) queue.push(*r.request);
}

}  // namespace

int runApp(int argc, wchar_t** argv) {
    SetUnhandledExceptionFilter(crashFilter);
    std::set_terminate(terminateHandler);

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(narrow(argv[i]));
    HostOptions host = parseHostOptions(args);
    if (host.version) {  // like PuTTY's -V / --version: a message box, as there is no console
        const std::wstring text = widen("SSHTermaq " + describeBuild(buildInfo()));
        MessageBoxW(nullptr, text.c_str(), L"SSHTermaq", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    AppPaths paths = initAppPaths(host.dataDir);
    applog::init(paths.logsDir / "sshtermaq.log");
    applog::info("SSHTermaq " + describeBuild(buildInfo()) + " starting, data dir: " +
                 narrow(paths.dataDir.wstring()) + (paths.portable ? " (portable)" : ""));
    for (const auto& e : host.errors) applog::warn("Command line: " + e);

    SingleInstance instance(paths.dataDir);
    if (!instance.primary()) {
        if (SingleInstance::forward()) return 0;
        applog::warn("Another instance holds the lock but could not be reached; starting anyway");
    }

    // OLE (not just COM): drag & drop needs it on the UI thread.
    HRESULT com = OleInitialize(nullptr);
    WsaInit wsa;

    int exitCode = 0;
    {
        Store store(paths.dataDir);
        store.load();

        MainWindowOptions wopts;
        wopts.devUrl = host.devUrl;
        wopts.devtools = host.devtools;
        MainWindow window(store, paths, wopts);
        Bridge bridge(window);
        window.setBridge(&bridge);
        bridge.setSink([&window](const std::string& json) { window.postWebMessage(json); });

        SessionManager sessions(store, bridge);
        SftpService sftp(sessions, bridge);
        LocalFsService localFs(bridge, [&window] { return window.hwnd(); });
        OpenRequestQueue openRequests(bridge);

        registerConnHandlers(bridge, sessions);
        registerImportHandlers(bridge, store);
        sftp.registerHandlers();
        localFs.registerHandlers();
        registerCoreHandlers({bridge, store, window, paths, openRequests});

        window.activeSessionCount = [&sessions] { return sessions.activeCount(); };
        window.onNavigationStarting = [&openRequests] { openRequests.reset(); };
        window.onSecondInstance = [&store, &openRequests](const std::vector<std::string>& a, const std::wstring& cwd) {
            HostOptions h = parseHostOptions(a);
            queueCliRequest(h.rest, std::filesystem::path(cwd), store, openRequests);
        };

        wchar_t cwd[MAX_PATH * 4] = {};
        GetCurrentDirectoryW(static_cast<DWORD>(std::size(cwd)), cwd);
        queueCliRequest(host.rest, std::filesystem::path(cwd), store, openRequests);

        applyInitialAppearance(window, store.settings());
        if (!window.create(SW_SHOWDEFAULT)) {
            MessageBoxW(nullptr, L"SSHTermaq could not create its main window.", L"SSHTermaq", MB_ICONERROR);
            exitCode = 1;
        } else {
            dialogs::init(window.hwnd(), &window);
            dnd::init(window.hwnd());
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            exitCode = static_cast<int>(msg.wParam);
        }

        applog::info("Shutting down");
        dialogs::init(nullptr, nullptr);
        dnd::init(nullptr);
        bridge.setSink(nullptr);
        sftp.closeAll();
        localFs.closeAll();
        sessions.closeAll();
        store.onSessionsChanged = nullptr;
        store.onSettingsChanged = nullptr;
        store.knownHosts().onChanged = nullptr;
    }

    if (SUCCEEDED(com)) OleUninitialize();
    applog::info("Bye");
    return exitCode;
}

}  // namespace st
