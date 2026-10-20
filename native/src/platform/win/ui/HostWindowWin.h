#pragma once
// The host window on Windows (platform/ui/HostWindow.h): a Win32 window with a custom frame (the web UI draws the
// title bar) hosting a WebView2 control that fills the client area. Also the UI-thread Dispatcher.
//
// Frame: WS_OVERLAPPEDWINDOW (native resize borders, DWM shadow, Aero Snap, min/max animations); WM_NCCALCSIZE
// removes the caption so the client area extends to the top edge; WM_NCHITTEST provides the top resize band and
// treats points the WebView hands back (CSS "app-region: drag", WebView2 non-client region support) as caption.
// Threading: everything except post()/isUiThread() must be called on the UI thread.
#include "platform/ui/HostWindow.h"

#include "platform/detail/WinApi.h"

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace st::ui {

/** Window class of the main window; a second instance finds the first one by it (AppWin.cpp). */
inline constexpr wchar_t kWindowClass[] = L"SSHTermaqMainWindow";
/** WM_COPYDATA identifier for command lines forwarded by a second instance ("cwd \0 command line"). */
inline constexpr ULONG_PTR kCopyDataCommandLine = 0x53545243;  // 'STRC'

/** Splits a Windows command line (CommandLineToArgvW rules) into UTF-8 arguments, excluding argv[0] (AppWin.cpp). */
std::vector<std::string> splitCommandLine(const std::wstring& commandLine);

class HostWindowWin final : public HostWindow {
public:
    explicit HostWindowWin(HostWindowOptions options);
    ~HostWindowWin() override;
    HostWindowWin(const HostWindowWin&) = delete;
    HostWindowWin& operator=(const HostWindowWin&) = delete;

    bool create() override;
    void* nativeHandle() const override { return hwnd_; }
    std::string browserVersion() const override { return browserVersion_; }

    void postWebMessage(const std::string& json) override;

    /* ---- Dispatcher ---- */
    void post(std::function<void()> fn) override;
    bool isUiThread() const override;

    /* ---- window control (UI thread) ---- */
    void minimize() override;
    void toggleMaximize() override;
    bool isMaximized() const override;
    void toggleFullscreen() override;
    bool isFullscreen() const override { return fullscreen_; }
    void requestClose() override;
    void setTitle(const std::string& title) override;
    void flash() override;
    void setThemeSource(const std::string& source) override;
    void setBackgroundMaterial(const std::string& material) override;
    void setUiScale(double factor) override;

private:
    struct WebView;  // WebView2 objects (keeps WebView2 headers out of this header)

    /** Brings the window to the foreground (restores it when minimized). */
    void activate();

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
    /** Answers a WebView2 request for the bundled UI through the serveUi hook (UI thread). */
    void serveUiResource(void* args);
    void handleWebMessage(void* args);
    void resizeWebView();
    void applyFrameTheme();
    void applyBackdrop();
    void applyControllerBackground();

    void restorePlacement(int showCommand);
    void savePlacement(bool synchronous);
    UINT dpi() const;
    bool effectiveDark() const;
    COLORREF backgroundColor() const;

    HostWindowOptions options_;
    HWND hwnd_ = nullptr;
    DWORD uiThreadId_ = 0;
    std::unique_ptr<WebView> web_;
    std::string browserVersion_;

    std::mutex dispatchMutex_;
    std::deque<std::function<void()>> dispatchQueue_;
    bool dispatchPosted_ = false;

    std::string themeSource_ = "system";
    std::string backdrop_ = "none";
    double uiScale_ = 1.0;
    bool fullscreen_ = false;
    WINDOWPLACEMENT preFullscreen_{};
    bool lastMaximized_ = false;
    bool forceClose_ = false;  // the web view failed: close without asking
    bool webViewFailed_ = false;
};

}  // namespace st::ui
