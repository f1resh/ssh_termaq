#pragma once
// The application's single top-level window: a Win32 window with a custom frame (the web UI draws the title bar)
// hosting a WebView2 control that fills the client area. Also the UI-thread Dispatcher.
//
// Frame: WS_OVERLAPPEDWINDOW (native resize borders, DWM shadow, Aero Snap, min/max animations); WM_NCCALCSIZE
// removes the caption so the client area extends to the top edge; WM_NCHITTEST provides the top resize band and
// treats points the WebView hands back (CSS "app-region: drag", WebView2 non-client region support) as caption.
// Threading: everything except post()/isUiThread() must be called on the UI thread.
#include "app/Paths.h"
#include "core/Json.h"
#include "core/Threading.h"
#include "core/Win.h"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sc {

class Bridge;
class Store;

struct MainWindowOptions {
    std::string devUrl;     // navigate here instead of the bundled UI
    bool devtools = false;  // allow DevTools (F12 / Ctrl+Shift+I) in Release builds
};

class MainWindow final : public Dispatcher {
public:
    MainWindow(Store& store, AppPaths paths, MainWindowOptions options);
    ~MainWindow() override;
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    /** Bridge used for web messages / events (must be set before create()). */
    void setBridge(Bridge* bridge) { bridge_ = bridge; }

    /** Creates and shows the window and starts WebView2 initialization. Returns false on fatal failure. */
    bool create(int showCommand);
    HWND hwnd() const { return hwnd_; }

    /** Delivers a bridge message to the page (UI thread; dropped while the WebView is not ready). */
    void postWebMessage(const std::string& json);

    /* ---- Dispatcher ---- */
    void post(std::function<void()> fn) override;
    bool isUiThread() const override;

    /* ---- window control (UI thread) ---- */
    void minimize();
    void toggleMaximize();
    /** Goes through the WM_CLOSE flow (may ask the UI to confirm). */
    void requestClose();
    bool isMaximized() const;
    bool isFullscreen() const { return fullscreen_; }
    void setTitle(const std::string& title);
    /** Flashes the taskbar button when the window is not in the foreground. */
    void flash();
    void toggleFullscreen();
    /** 'system' | 'light' | 'dark' -> dark/light window frame. */
    void setThemeSource(const std::string& source);
    /** 'none' | 'mica' | 'acrylic'. */
    void setBackgroundMaterial(const std::string& material);
    /** Controller zoom factor (UI scale), clamped to [0.25, 5]. */
    void setUiScale(double factor);
    /** Answer to app:onCloseRequested. */
    void confirmClose(bool allow);
    /** Closes without asking. */
    void quit();
    /** Brings the window to the foreground (restores it when minimized). */
    void activate();

    /** WebView2 browser version ('' until initialized). */
    std::string browserVersion() const { return browserVersion_; }
    /** HKCU ...\Themes\Personalize AppsUseLightTheme == 0. */
    static bool systemPrefersDark();

    /** Number of connecting/connected sessions (WM_CLOSE asks the UI to confirm when > 0). */
    std::function<int()> activeSessionCount;
    /** Fired when the top-level document starts (re)loading (UI state must be re-synchronised). */
    std::function<void()> onNavigationStarting;
    /** A second instance forwarded its command line (args exclude argv[0]). */
    std::function<void(const std::vector<std::string>& args, const std::wstring& cwd)> onSecondInstance;

private:
    struct WebView;  // WebView2 objects (keeps WebView2 headers out of this header)

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handleMessage(UINT msg, WPARAM wp, LPARAM lp);
    LRESULT onNcCalcSize(WPARAM wp, LPARAM lp);
    LRESULT onNcHitTest(LPARAM lp);
    void onSize(WPARAM wp);
    void drainDispatchQueue();

    void initWebView();
    void onWebViewEnvironment(HRESULT hr, void* environment);
    void onWebViewController(HRESULT hr, void* controller);
    void configureWebView();
    void navigateToUi();
    void handleWebMessage(void* args);
    void showFatalWebViewError(const std::wstring& message);
    void resizeWebView();
    void applyFrameTheme();
    void applyBackdrop();
    void applyControllerBackground();
    bool isAppOrigin(const std::wstring& uri) const;
    void openInBrowser(const std::wstring& uri) const;

    void restorePlacement(int showCommand);
    void savePlacement(bool synchronous);
    UINT dpi() const;
    bool effectiveDark() const;
    COLORREF backgroundColor() const;

    Store& store_;
    AppPaths paths_;
    MainWindowOptions options_;
    Bridge* bridge_ = nullptr;
    HWND hwnd_ = nullptr;
    DWORD uiThreadId_ = 0;
    std::unique_ptr<WebView> web_;
    std::string browserVersion_;
    std::wstring appOrigin_;  // "https://app.sshcrawler.local" or the dev server origin

    std::mutex dispatchMutex_;
    std::deque<std::function<void()>> dispatchQueue_;
    bool dispatchPosted_ = false;

    std::string themeSource_ = "system";
    std::string backdrop_ = "none";
    double uiScale_ = 1.0;
    bool fullscreen_ = false;
    WINDOWPLACEMENT preFullscreen_{};
    bool lastMaximized_ = false;
    bool closeConfirmed_ = false;
    ULONGLONG closeRequestedAt_ = 0;  // GetTickCount64 of an unanswered app:onCloseRequested
    bool uiAlive_ = false;  // the page talked to us since the last navigation
    bool webViewFailed_ = false;
    std::shared_ptr<std::atomic<uint64_t>> placementSeq_;
};

}  // namespace sc
