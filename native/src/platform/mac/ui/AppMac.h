#pragma once
// Private to platform/mac/ui: what the app shell (AppMac.mm) and the main window (HostWindowMac.mm) share. Plain C++,
// so either side may include it.
#include "platform/ui/HostWindow.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace st::ui::mac {

/** What arrives at the app for the main window: a second instance's command line or URLs from Launch Services, and
 *  Quit (Cmd+Q, the Dock, logging out), which closes the window the way its close button does. UI thread. */
struct AppEvents {
    std::function<void(const std::vector<std::string>& args, const std::filesystem::path& cwd)> openRequest;
    std::function<void()> quitRequested;
};

/** The main window registers its handlers while it exists (nullptr when it goes); requests that arrived before it
 *  are handed over then. */
void setAppEvents(AppEvents* events);

/** Ends runEventLoop() with `exitCode` (UI thread). */
void stopEventLoop(int exitCode);

/**
 * The menu bar: the app's own menus with the page's (HostWindow::setMenuBar) - "app" items open the application
 * menu, "File" goes before Edit, the others after it, before Window. `run` gets a chosen command. Empty: the app's own
 * menus only. UI thread.
 */
void setMenuBar(const std::vector<MenuBarMenu>& menus, std::function<void(const std::string& command)> run);

}  // namespace st::ui::mac
