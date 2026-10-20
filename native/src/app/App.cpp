#include "app/App.h"

#include "app/BuildInfo.h"
#include "app/Cli.h"
#include "app/CoreHandlers.h"
#include "app/HostController.h"
#include "app/Paths.h"
#include "bridge/Bridge.h"
#include "core/Crypto.h"
#include "core/Log.h"
#include "core/Strings.h"
#include "importers/ImportHandlers.h"
#include "localfs/LocalFsService.h"
#include "platform/sys/Fs.h"
#include "platform/sys/Net.h"
#include "platform/ui/App.h"
#include "platform/ui/Dialogs.h"
#include "platform/ui/DragDrop.h"
#include "platform/ui/HostWindow.h"
#include "session/SessionManager.h"
#include "sftp/SftpService.h"
#include "store/Store.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>

namespace st {

namespace {

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

/** Single-instance key: per user and data directory (portable copies and --data-dir profiles run side by side). */
std::string instanceKey(const std::filesystem::path& dataDir) {
    std::string key = toLower(sys::fromPath(dataDir));
    return hexEncode(crypto::sha256(key)).substr(0, 16);
}

/** Best-effort frame theme/backdrop from saved settings so the window doesn't flash before the UI loads. */
void applyInitialAppearance(ui::HostWindow& window, const Json& settings) {
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

int runApp(const std::vector<std::string>& args) {
    ui::installCrashLogging();
    std::set_terminate(terminateHandler);

    HostOptions host = parseHostOptions(args);
    if (host.version) {  // like PuTTY's -V / --version: a message box, as there is no console
        ui::showMessage("SSHTermaq " + describeBuild(buildInfo()), ui::MessageKind::Info);
        return 0;
    }

    AppPaths paths = initAppPaths(host.dataDir);
    applog::init(paths.logsDir / "sshtermaq.log");
    applog::info("SSHTermaq " + describeBuild(buildInfo()) + " starting, data dir: " +
                 sys::fromPath(paths.dataDir) + (paths.portable ? " (portable)" : ""));
    for (const auto& e : host.errors) applog::warn("Command line: " + e);

    ui::InstanceLock instance(instanceKey(paths.dataDir));
    if (!instance.primary()) {
        if (ui::InstanceLock::forwardToPrimary()) return 0;
        applog::warn("Another instance holds the lock but could not be reached; starting anyway");
    }

    ui::UiThread uiThread;
    sys::NetInit net;

    int exitCode = 0;
    {
        Store store(paths.dataDir);
        store.load();

        ui::HostWindowOptions wopts;
        wopts.webDataDir = paths.webViewDataDir;
        wopts.devUrl = host.devUrl;
        wopts.devtools = host.devtools;
        wopts.automationSocket = host.automation;
        std::unique_ptr<ui::HostWindow> mainWindow = ui::createHostWindow(wopts);
        ui::HostWindow& window = *mainWindow;
        Bridge bridge(window);
        bridge.setSink([&window](const std::string& json) { window.postWebMessage(json); });
        HostController controller(window, bridge, store, host.devUrl);

        SessionManager sessions(store, bridge);
        SftpService sftp(sessions, bridge);
        LocalFsService localFs(bridge, [&window] { return window.nativeHandle(); });
        OpenRequestQueue openRequests(bridge);

        registerConnHandlers(bridge, sessions);
        registerImportHandlers(bridge, store);
        sftp.registerHandlers();
        localFs.registerHandlers();
        registerCoreHandlers({bridge, store, controller, paths, openRequests});

        controller.activeSessionCount = [&sessions] { return sessions.activeCount(); };
        controller.onPageLoading = [&openRequests] { openRequests.reset(); };
        window.onSecondInstance = [&store, &openRequests](const std::vector<std::string>& a,
                                                          const std::filesystem::path& cwd) {
            HostOptions h = parseHostOptions(a);
            queueCliRequest(h.rest, cwd, store, openRequests);
        };

        std::error_code ec;
        queueCliRequest(host.rest, std::filesystem::current_path(ec), store, openRequests);

        applyInitialAppearance(window, store.settings());
        if (!window.create()) {
            ui::showMessage("SSHTermaq could not create its main window.", ui::MessageKind::Error);
            exitCode = 1;
        } else {
            ui::dialogs::init(window.nativeHandle(), &window);
            ui::dnd::init(window.nativeHandle());
            exitCode = ui::runEventLoop();
        }

        applog::info("Shutting down");
        ui::dialogs::init(nullptr, nullptr);
        ui::dnd::init(nullptr);
        bridge.setSink(nullptr);
        sftp.closeAll();
        localFs.closeAll();
        sessions.closeAll();
        store.onSessionsChanged = nullptr;
        store.onSettingsChanged = nullptr;
        store.knownHosts().onChanged = nullptr;
    }

    applog::info("Bye");
    return exitCode;
}

}  // namespace st
