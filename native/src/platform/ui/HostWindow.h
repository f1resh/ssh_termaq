#pragma once
// The application's single top-level window hosting the web UI, and the UI-thread Dispatcher.
// Windows: a Win32 window with a custom frame (the web UI draws the title bar) and a WebView2 control filling it
// (platform/win/ui/HostWindowWin.h). macOS: an NSWindow with a transparent, full-size title bar (the traffic lights
// stay over the page) and a WKWebView as its content (platform/mac/ui/HostWindowMac.mm).
//
// The window does the OS side only. What it may load, what it tells the page, when it may close and where its
// placement is kept are decided through the hooks below (app/HostController sets them before create()). Hooks run on
// the UI thread. Threading: everything except post()/isUiThread() must be called on the UI thread.
#include "core/Threading.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st::ui {

struct HostWindowOptions {
    std::filesystem::path webDataDir;  // the web view's profile folder
    std::string devUrl;                // navigate here instead of the bundled UI
    bool devtools = false;             // allow DevTools (F12 / Ctrl+Shift+I) in Release builds
    /** Debug builds, macOS: a Unix socket over which ui/scripts/app.mjs drives the window (Windows: WebView2's
     *  remote debugging, which needs nothing from the host). */
    std::string automationSocket;
};

/** A file of the bundled UI, as served to the web view. `body` must stay valid while the window exists. */
struct UiFile {
    std::string_view body;
    std::string contentType;
};

/** A rectangle in the page's CSS pixels. */
struct PageRect {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
};

/** The title bar the page draws: its height, and where pressing the mouse moves the window instead. */
struct TitleBarLayout {
    double height = 0;              // CSS px
    std::vector<PageRect> drag;     // areas that move the window...
    std::vector<PageRect> noDrag;   // ...except these (buttons, tabs)
};

/** Room the OS keeps over the page's title bar (macOS: the traffic lights), in CSS px from each edge. */
struct ChromeInsets {
    double left = 0;
    double right = 0;
    bool operator==(const ChromeInsets&) const = default;
};

/** One entry of an OS menu bar menu the page fills with its commands. */
struct MenuBarItem {
    std::string command;   // handed back through HostWindow::onMenuCommand
    std::string label;
    std::string shortcut;  // the page's chord ("Meta+Shift+T"), '' for none
    bool separator = false;
    bool checked = false;
    bool enabled = true;
};
/** A menu of the OS menu bar; title "app" adds its items to the application menu. */
struct MenuBarMenu {
    std::string title;
    std::vector<MenuBarItem> items;
};

/** Size and position of the window when it is not maximized (screen pixels), and whether it is maximized. */
struct WindowPlacement {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool maximized = false;
};

class HostWindow : public Dispatcher {
public:
    ~HostWindow() override = default;

    /** Creates and shows the window and starts loading the UI. Returns false on fatal failure. */
    virtual bool create() = 0;
    /** The native window (Windows: HWND) for dialogs, drag & drop and shell operations; null before create(). */
    virtual void* nativeHandle() const = 0;
    /** Web engine version ('' until initialized). */
    virtual std::string browserVersion() const = 0;

    /** Delivers a bridge message to the page (dropped while the web view is not ready). */
    virtual void postWebMessage(const std::string& json) = 0;

    /* ---- window control ---- */
    virtual void minimize() = 0;
    virtual void toggleMaximize() = 0;
    virtual bool isMaximized() const = 0;
    virtual void toggleFullscreen() = 0;
    virtual bool isFullscreen() const = 0;
    /** Starts closing: asks onCloseRequested, and closes when it agrees (or is not set). */
    virtual void requestClose() = 0;
    virtual void setTitle(const std::string& title) = 0;
    /** Draws attention to the window (Windows: flashes the taskbar button) when it is not in the foreground. */
    virtual void flash() = 0;
    /** 'system' | 'light' | 'dark' -> dark/light window frame. */
    virtual void setThemeSource(const std::string& source) = 0;
    /** 'none' | 'mica' | 'acrylic' (Windows 11 backdrops). */
    virtual void setBackgroundMaterial(const std::string& material) = 0;
    /** Page zoom factor (UI scale), clamped to [0.25, 5]. */
    virtual void setUiScale(double factor) = 0;
    /** The page's title bar, where the OS does not take drag areas from CSS (macOS); Windows uses app-region. */
    virtual void setTitleBarLayout(const TitleBarLayout& layout) {}
    /** The page's commands for the OS menu bar (macOS; Windows has none); a choice comes back via onMenuCommand. */
    virtual void setMenuBar(const std::vector<MenuBarMenu>& menus) {}

    /* ---- hooks (set before create()) ---- */
    /** The bundled UI's file for a request URI under bundledUiOrigin(); nullopt -> 404. */
    std::function<std::optional<UiFile>(const std::string& uri)> serveUi;
    /** A message from the page: its sender's origin ('' when unknown), the JSON text, paths of attached files. */
    std::function<void(const std::string& source, const std::string& json, const std::vector<std::string>& files)>
        onWebMessage;
    /** The top-level document is about to load `uri`; false cancels the navigation. */
    std::function<bool(const std::string& uri)> onNavigationStarting;
    /** The page asked for a new window (target=_blank, window.open); the window opens none itself. */
    std::function<void(const std::string& uri)> onNewWindowRequested;
    /** Closing was asked for (close button, Alt+F4, requestClose()); true closes now, false keeps the window open. */
    std::function<bool()> onCloseRequested;
    std::function<void(bool focused)> onFocusChanged;
    std::function<void(bool maximized)> onMaximizedChanged;
    std::function<void(bool fullscreen)> onFullscreenChanged;
    /** The OS switched between light and dark appearance. */
    std::function<void(bool dark)> onSystemThemeChanged;
    /** The placement to restore when the window is created; nullopt: a default size, centred. */
    std::function<std::optional<WindowPlacement>()> loadPlacement;
    /** The placement changed (often, while resizing); `final` when the window is closing. */
    std::function<void(const WindowPlacement& placement, bool final)> onPlacementChanged;
    /** A command of the page's chosen in the OS menu bar (setMenuBar). */
    std::function<void(const std::string& command)> onMenuCommand;
    /** The room the OS keeps over the title bar changed (macOS: the traffic lights moved, full screen). */
    std::function<void(const ChromeInsets& insets)> onChromeInsetsChanged;
    /** A second instance forwarded its command line (args exclude argv[0]) and working directory. */
    std::function<void(const std::vector<std::string>& args, const std::filesystem::path& cwd)> onSecondInstance;
};

/** The main window (not yet shown: call create()). Call on the UI thread, after the UI thread is set up (ui/App.h). */
std::unique_ptr<HostWindow> createHostWindow(HostWindowOptions options);

/** Origin the bundled UI is served from on this OS ("https://app.sshtermaq.local"), lower-case. */
std::string bundledUiOrigin();

/** The OS appearance is dark (Windows: Personalize\AppsUseLightTheme == 0). */
bool systemPrefersDark();

}  // namespace st::ui
