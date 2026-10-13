#include "app/MainWindow.h"

#include "app/Cli.h"
#include "bridge/Bridge.h"
#include "core/Log.h"
#include "core/Strings.h"
#include "store/Store.h"

#include <objbase.h>
#include <unknwn.h>
#include <WebView2.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>
#include <wrl.h>

#include <algorithm>
#include <cmath>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace sc {

namespace {

constexpr wchar_t kWindowClass[] = L"SSHCrawlerMainWindow";
constexpr wchar_t kAppHost[] = L"app.sshcrawler.local";
constexpr UINT WM_APP_DISPATCH = WM_APP + 1;
/** WM_COPYDATA identifier for command lines forwarded by a second instance. */
constexpr ULONG_PTR kCopyDataCommandLine = 0x53435243;  // 'SCRC'
constexpr int kMinWidth = 640;
constexpr int kMinHeight = 400;

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif

int scale(int v, UINT dpi) { return MulDiv(v, static_cast<int>(dpi), 96); }

std::wstring toLowerW(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

std::wstring hrMessage(HRESULT hr) {
    return widen(winErrorMessage(static_cast<unsigned long>(hr))) + L" (0x" + [&] {
        wchar_t buf[16];
        swprintf_s(buf, L"%08X", static_cast<unsigned>(hr));
        return std::wstring(buf);
    }() + L")";
}

std::wstring htmlEscape(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        switch (c) {
            case L'<': out += L"&lt;"; break;
            case L'>': out += L"&gt;"; break;
            case L'&': out += L"&amp;"; break;
            case L'"': out += L"&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

std::wstring originOf(const std::wstring& uri) {
    // scheme://host[:port]
    auto p = uri.find(L"://");
    if (p == std::wstring::npos) return {};
    auto end = uri.find_first_of(L"/?#", p + 3);
    return toLowerW(uri.substr(0, end));
}

}  // namespace

struct MainWindow::WebView {
    ComPtr<ICoreWebView2Environment> env;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> view;
};

MainWindow::MainWindow(Store& store, AppPaths paths, MainWindowOptions options)
    : store_(store),
      paths_(std::move(paths)),
      options_(std::move(options)),
      web_(std::make_unique<WebView>()),
      placementSeq_(std::make_shared<std::atomic<uint64_t>>(0)) {
    uiThreadId_ = GetCurrentThreadId();
    if (!options_.devUrl.empty())
        appOrigin_ = originOf(widen(options_.devUrl));
    else
        appOrigin_ = std::wstring(L"https://") + kAppHost;
}

MainWindow::~MainWindow() {
    if (web_ && web_->controller) web_->controller->Close();
    web_.reset();
    if (hwnd_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
    }
}

/* ------------------------------------------------------------------------ */
/* Dispatcher                                                                */
/* ------------------------------------------------------------------------ */

void MainWindow::post(std::function<void()> fn) {
    bool needPost = false;
    {
        std::lock_guard lock(dispatchMutex_);
        dispatchQueue_.push_back(std::move(fn));
        if (!dispatchPosted_) {
            dispatchPosted_ = true;
            needPost = true;
        }
    }
    if (needPost && hwnd_) PostMessageW(hwnd_, WM_APP_DISPATCH, 0, 0);
}

void MainWindow::postWebMessage(const std::string& json) {
    if (web_ && web_->view && !webViewFailed_) web_->view->PostWebMessageAsJson(widen(json).c_str());
}

bool MainWindow::isUiThread() const { return GetCurrentThreadId() == uiThreadId_; }

void MainWindow::drainDispatchQueue() {
    std::deque<std::function<void()>> batch;
    {
        std::lock_guard lock(dispatchMutex_);
        batch.swap(dispatchQueue_);
        dispatchPosted_ = false;
    }
    for (auto& fn : batch) {
        try {
            fn();
        } catch (const std::exception& e) {
            applog::error(std::string("Dispatched task threw: ") + e.what());
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Window creation                                                           */
/* ------------------------------------------------------------------------ */

bool MainWindow::create(int showCommand) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MainWindow::wndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), 0));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        applog::error("RegisterClassEx failed: " + lastErrorMessage());
        return false;
    }

    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, L"SSH Crawler", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800, nullptr, nullptr, inst, this);
    if (!hwnd_) {
        applog::error("CreateWindowEx failed: " + lastErrorMessage());
        return false;
    }
    // Make DWM extend the frame (shadow + Win11 rounded corners) and recompute the client area.
    MARGINS m{0, 0, 1, 0};
    DwmExtendFrameIntoClientArea(hwnd_, &m);
    applyFrameTheme();
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    restorePlacement(showCommand);
    UpdateWindow(hwnd_);
    initWebView();
    return true;
}

LRESULT CALLBACK MainWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    try {
        return self->handleMessage(msg, wp, lp);
    } catch (const std::exception& e) {
        applog::error(std::string("Window message handler threw: ") + e.what());
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

UINT MainWindow::dpi() const { return hwnd_ ? GetDpiForWindow(hwnd_) : 96; }

LRESULT MainWindow::handleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_APP_DISPATCH:
            drainDispatchQueue();
            return 0;

        case WM_NCCALCSIZE:
            return onNcCalcSize(wp, lp);

        case WM_NCHITTEST:
            return onNcHitTest(lp);

        case WM_SIZE:
            onSize(wp);
            return 0;

        case WM_MOVE:
        case WM_MOVING:
            if (web_->controller) web_->controller->NotifyParentWindowPositionChanged();
            break;

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = scale(kMinWidth, dpi());
            mmi->ptMinTrackSize.y = scale(kMinHeight, dpi());
            return 0;
        }

        case WM_DPICHANGED: {
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd_, &rc);
            HBRUSH b = CreateSolidBrush(backgroundColor());
            FillRect(reinterpret_cast<HDC>(wp), &rc, b);
            DeleteObject(b);
            return 1;
        }

        case WM_ACTIVATE: {
            const bool focused = LOWORD(wp) != WA_INACTIVE;
            if (bridge_) bridge_->emit("window:onFocusChanged", Json::array({focused}));
            if (focused && web_->controller) web_->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            break;
        }

        case WM_SETFOCUS:
            if (web_->controller) web_->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            return 0;

        case WM_SETTINGCHANGE:
            if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
                applyFrameTheme();
                if (bridge_) bridge_->emit("app:onSystemThemeChanged", Json::array({systemPrefersDark()}));
                InvalidateRect(hwnd_, nullptr, TRUE);
            }
            break;

        case WM_COPYDATA: {
            auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
            if (cds && cds->dwData == kCopyDataCommandLine && cds->lpData && cds->cbData >= sizeof(wchar_t)) {
                std::wstring payload(static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t));
                while (!payload.empty() && payload.back() == L'\0') payload.pop_back();
                // payload = cwd \0 command line
                std::wstring cwd, cmd;
                auto nul = payload.find(L'\0');
                if (nul == std::wstring::npos) {
                    cmd = payload;
                } else {
                    cwd = payload.substr(0, nul);
                    cmd = payload.substr(nul + 1);
                }
                std::vector<std::string> args = splitCommandLine(cmd);
                activate();
                if (onSecondInstance) {
                    // Defer: don't run app logic inside SendMessage from another process.
                    post([this, args, cwd] { onSecondInstance(args, cwd); });
                }
                return TRUE;
            }
            break;
        }

        case WM_CLOSE: {
            const int active = activeSessionCount ? activeSessionCount() : 0;
            const ULONGLONG now = GetTickCount64();
            const bool pendingTooLong = closeRequestedAt_ && now - closeRequestedAt_ > 5000;
            if (closeConfirmed_ || !uiAlive_ || active == 0 || !bridge_ || pendingTooLong) {
                savePlacement(true);
                DestroyWindow(hwnd_);
                return 0;
            }
            closeRequestedAt_ = now;
            bridge_->emit("app:onCloseRequested", Json::array());
            return 0;
        }

        case WM_DESTROY:
            savePlacement(true);
            if (web_->controller) {
                web_->controller->Close();
                web_->controller.Reset();
                web_->view.Reset();
            }
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

/* ------------------------------------------------------------------------ */
/* Custom frame                                                              */
/* ------------------------------------------------------------------------ */

LRESULT MainWindow::onNcCalcSize(WPARAM wp, LPARAM lp) {
    if (!wp) return DefWindowProcW(hwnd_, WM_NCCALCSIZE, wp, lp);
    auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
    if (fullscreen_) return 0;  // whole window is client
    const LONG originalTop = params->rgrc[0].top;
    LRESULT r = DefWindowProcW(hwnd_, WM_NCCALCSIZE, wp, lp);
    if (r != 0) return r;
    // Remove the caption: the client area extends to the top edge of the window.
    params->rgrc[0].top = originalTop;
    if (IsZoomed(hwnd_)) {
        // Maximized windows hang over the monitor by the frame thickness on every side.
        const UINT d = dpi();
        params->rgrc[0].top += GetSystemMetricsForDpi(SM_CYFRAME, d) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, d);
    }
    return 0;
}

LRESULT MainWindow::onNcHitTest(LPARAM lp) {
    // Left/right/bottom borders are real non-client area.
    LRESULT hit = DefWindowProcW(hwnd_, WM_NCHITTEST, 0, lp);
    if (hit != HTCLIENT || fullscreen_) return hit;

    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ScreenToClient(hwnd_, &pt);
    if (!IsZoomed(hwnd_)) {
        const UINT d = dpi();
        const int border = GetSystemMetricsForDpi(SM_CYFRAME, d) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, d);
        if (pt.y < border) {
            RECT rc;
            GetClientRect(hwnd_, &rc);
            if (pt.x < border * 2) return HTTOPLEFT;
            if (pt.x >= rc.right - border * 2) return HTTOPRIGHT;
            return HTTOP;
        }
    }
    // Points reach the host window only where the web content hands hit-testing back (CSS app-region: drag).
    return HTCAPTION;
}

void MainWindow::onSize(WPARAM wp) {
    resizeWebView();
    const bool maximized = wp == SIZE_MAXIMIZED;
    if (wp != SIZE_MINIMIZED && maximized != lastMaximized_) {
        lastMaximized_ = maximized;
        if (bridge_) bridge_->emit("window:onMaximizedChanged", Json::array({maximized}));
    }
    if (wp != SIZE_MINIMIZED) savePlacement(false);
}

void MainWindow::resizeWebView() {
    if (!web_->controller) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    web_->controller->put_Bounds(rc);
}

/* ------------------------------------------------------------------------ */
/* WebView2                                                                  */
/* ------------------------------------------------------------------------ */

void MainWindow::initWebView() {
    std::error_code ec;
    std::filesystem::create_directories(paths_.webViewDataDir, ec);
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, paths_.webViewDataDir.wstring().c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                onWebViewEnvironment(result, env);
                return S_OK;
            })
            .Get());
    if (FAILED(hr)) onWebViewEnvironment(hr, nullptr);
}

void MainWindow::onWebViewEnvironment(HRESULT hr, void* environment) {
    if (FAILED(hr) || !environment) {
        webViewFailed_ = true;
        applog::error("WebView2 environment creation failed: " + narrow(hrMessage(hr)));
        if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
            int r = MessageBoxW(hwnd_,
                                L"SSH Crawler needs the Microsoft Edge WebView2 Runtime, which is not installed.\n\n"
                                L"Open the download page now?",
                                L"SSH Crawler", MB_ICONERROR | MB_YESNO);
            if (r == IDYES)
                ShellExecuteW(nullptr, L"open", L"https://developer.microsoft.com/microsoft-edge/webview2/", nullptr,
                              nullptr, SW_SHOWNORMAL);
        } else {
            MessageBoxW(hwnd_, (L"Could not start the WebView2 runtime:\n" + hrMessage(hr)).c_str(), L"SSH Crawler",
                        MB_ICONERROR);
        }
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        closeConfirmed_ = true;
        return;
    }
    web_->env = static_cast<ICoreWebView2Environment*>(environment);
    ComPtr<ICoreWebView2Environment> env = web_->env;
    hr = env->CreateCoreWebView2Controller(
        hwnd_, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                   [this](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                       onWebViewController(result, controller);
                       return S_OK;
                   })
                   .Get());
    if (FAILED(hr)) onWebViewController(hr, nullptr);
}

void MainWindow::onWebViewController(HRESULT hr, void* controller) {
    if (FAILED(hr) || !controller) {
        webViewFailed_ = true;
        applog::error("WebView2 controller creation failed: " + narrow(hrMessage(hr)));
        MessageBoxW(hwnd_, (L"Could not create the WebView2 control:\n" + hrMessage(hr)).c_str(), L"SSH Crawler",
                    MB_ICONERROR);
        closeConfirmed_ = true;
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        return;
    }
    web_->controller = static_cast<ICoreWebView2Controller*>(controller);
    web_->controller->get_CoreWebView2(&web_->view);

    ComPtr<ICoreWebView2Environment> env = web_->env;
    LPWSTR version = nullptr;
    if (SUCCEEDED(env->get_BrowserVersionString(&version)) && version) {
        browserVersion_ = narrow(version);
        CoTaskMemFree(version);
    }
    configureWebView();
    applyControllerBackground();
    resizeWebView();
    web_->controller->put_IsVisible(TRUE);
    web_->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    navigateToUi();
}

void MainWindow::configureWebView() {
    ICoreWebView2* view = web_->view.Get();
    if (!view) return;

#ifdef NDEBUG
    const bool devtools = options_.devtools;
#else
    const bool devtools = true;
#endif

    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(view->get_Settings(&settings))) {
        settings->put_AreDevToolsEnabled(devtools ? TRUE : FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_AreDefaultScriptDialogsEnabled(TRUE);
        settings->put_IsBuiltInErrorPageEnabled(TRUE);
        ComPtr<ICoreWebView2Settings3> s3;
        if (SUCCEEDED(settings.As(&s3))) s3->put_AreBrowserAcceleratorKeysEnabled(devtools ? TRUE : FALSE);
        ComPtr<ICoreWebView2Settings4> s4;
        if (SUCCEEDED(settings.As(&s4))) {
            s4->put_IsPasswordAutosaveEnabled(FALSE);
            s4->put_IsGeneralAutofillEnabled(FALSE);
        }
        ComPtr<ICoreWebView2Settings5> s5;
        if (SUCCEEDED(settings.As(&s5))) s5->put_IsPinchZoomEnabled(FALSE);
        ComPtr<ICoreWebView2Settings6> s6;
        if (SUCCEEDED(settings.As(&s6))) s6->put_IsSwipeNavigationEnabled(FALSE);
        ComPtr<ICoreWebView2Settings9> s9;
        if (SUCCEEDED(settings.As(&s9))) s9->put_IsNonClientRegionSupportEnabled(TRUE);
    }

    // Bundled UI served from <exe>/ui through a virtual host (no file:// URLs).
    ComPtr<ICoreWebView2_3> v3;
    if (SUCCEEDED(web_->view.As(&v3)))
        v3->SetVirtualHostNameToFolderMapping(kAppHost, paths_.uiDir.wstring().c_str(),
                                              COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);

    EventRegistrationToken token{};

    view->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                     [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                         handleWebMessage(args);
                                         return S_OK;
                                     })
                                     .Get(),
                                 &token);

    view->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                LPWSTR uri = nullptr;
                args->get_Uri(&uri);
                std::wstring u = uri ? uri : L"";
                if (uri) CoTaskMemFree(uri);
                if (u.rfind(L"data:", 0) == 0 || u == L"about:blank") return S_OK;
                if (!isAppOrigin(u)) {
                    args->put_Cancel(TRUE);
                    openInBrowser(u);
                    return S_OK;
                }
                uiAlive_ = false;
                if (onNavigationStarting) onNavigationStarting();
                return S_OK;
            })
            .Get(),
        &token);

    view->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                     [this](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                         args->put_Handled(TRUE);
                                         LPWSTR uri = nullptr;
                                         if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
                                             openInBrowser(uri);
                                             CoTaskMemFree(uri);
                                         }
                                         return S_OK;
                                     })
                                     .Get(),
                                 &token);

    view->add_PermissionRequested(
        Callback<ICoreWebView2PermissionRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                COREWEBVIEW2_PERMISSION_KIND kind{};
                args->get_PermissionKind(&kind);
                const bool allow =
                    kind == COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ || kind == COREWEBVIEW2_PERMISSION_KIND_LOCAL_FONTS;
                args->put_State(allow ? COREWEBVIEW2_PERMISSION_STATE_ALLOW : COREWEBVIEW2_PERMISSION_STATE_DENY);
                return S_OK;
            })
            .Get(),
        &token);

    // Native context menu only for editable fields (cut/copy/paste); the app draws its own menus elsewhere.
    ComPtr<ICoreWebView2_11> v11;
    if (SUCCEEDED(web_->view.As(&v11))) {
        v11->add_ContextMenuRequested(
            Callback<ICoreWebView2ContextMenuRequestedEventHandler>(
                [devtools](ICoreWebView2*, ICoreWebView2ContextMenuRequestedEventArgs* args) -> HRESULT {
                    ComPtr<ICoreWebView2ContextMenuTarget> target;
                    BOOL editable = FALSE;
                    if (SUCCEEDED(args->get_ContextMenuTarget(&target)) && target) target->get_IsEditable(&editable);
                    if (!editable && !(devtools && (GetKeyState(VK_SHIFT) & 0x8000) && (GetKeyState(VK_CONTROL) & 0x8000)))
                        args->put_Handled(TRUE);
                    return S_OK;
                })
                .Get(),
            &token);
    }

    view->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>(
                                [this](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                                    COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
                                    args->get_ProcessFailedKind(&kind);
                                    applog::error("WebView2 process failed, kind " + std::to_string(kind));
                                    if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED ||
                                        kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE) {
                                        if (web_->view) web_->view->Reload();
                                    } else if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
                                        web_->controller.Reset();
                                        web_->view.Reset();
                                        initWebView();
                                    }
                                    return S_OK;
                                })
                                .Get(),
                            &token);
}

void MainWindow::navigateToUi() {
    if (!web_->view) return;
    if (!options_.devUrl.empty()) {
        web_->view->Navigate(widen(options_.devUrl).c_str());
        return;
    }
    std::error_code ec;
    if (!std::filesystem::exists(paths_.uiDir / "index.html", ec)) {
        std::wstring html =
            L"<!doctype html><meta charset=utf-8><body style=\"font:14px 'Segoe UI';background:#1b1d22;color:#e6e8ec;"
            L"padding:40px\"><h2>SSH Crawler</h2><p>The user interface files were not found in:</p><pre>" +
            htmlEscape(paths_.uiDir.wstring()) +
            L"</pre><p>Build them with <code>npm run build</code> in the <code>ui</code> folder (the build copies them "
            L"next to the executable), or start with <code>--dev-url http://localhost:5173</code>.</p></body>";
        web_->view->NavigateToString(html.c_str());
        return;
    }
    web_->view->Navigate((std::wstring(L"https://") + kAppHost + L"/index.html").c_str());
}

bool MainWindow::isAppOrigin(const std::wstring& uri) const { return !appOrigin_.empty() && originOf(uri) == appOrigin_; }

void MainWindow::openInBrowser(const std::wstring& uri) const {
    const std::wstring lower = toLowerW(uri);
    if (lower.rfind(L"http://", 0) == 0 || lower.rfind(L"https://", 0) == 0 || lower.rfind(L"mailto:", 0) == 0)
        ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void MainWindow::handleWebMessage(void* argsPtr) {
    auto* args = static_cast<ICoreWebView2WebMessageReceivedEventArgs*>(argsPtr);
    if (!bridge_ || !args) return;
    LPWSTR source = nullptr;
    if (SUCCEEDED(args->get_Source(&source)) && source) {
        const bool ok = isAppOrigin(source);
        CoTaskMemFree(source);
        if (!ok) {
            applog::warn("Ignoring web message from a foreign origin");
            return;
        }
    }
    LPWSTR json = nullptr;
    if (FAILED(args->get_WebMessageAsJson(&json)) || !json) return;
    std::string text = narrow(json);
    CoTaskMemFree(json);

    std::vector<std::string> files;
    ComPtr<ICoreWebView2WebMessageReceivedEventArgs2> args2;
    if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&args2)))) {
        ComPtr<ICoreWebView2ObjectCollectionView> objects;
        if (SUCCEEDED(args2->get_AdditionalObjects(&objects)) && objects) {
            UINT32 count = 0;
            objects->get_Count(&count);
            for (UINT32 i = 0; i < count; ++i) {
                ComPtr<IUnknown> obj;
                if (FAILED(objects->GetValueAtIndex(i, &obj)) || !obj) continue;
                ComPtr<ICoreWebView2File> file;
                if (FAILED(obj.As(&file))) continue;
                LPWSTR path = nullptr;
                if (SUCCEEDED(file->get_Path(&path)) && path) {
                    files.push_back(narrow(path));
                    CoTaskMemFree(path);
                }
            }
        }
    }
    if (!uiAlive_) {
        uiAlive_ = true;
        // A (re)loaded UI starts from scratch: tell it about the current window state.
        bridge_->emit("window:onMaximizedChanged", Json::array({isMaximized()}));
        bridge_->emit("app:onFullscreenChanged", Json::array({fullscreen_}));
    }
    bridge_->onWebMessage(text, files);
}

void MainWindow::applyControllerBackground() {
    ComPtr<ICoreWebView2Controller2> c2;
    if (!web_->controller || FAILED(web_->controller.As(&c2))) return;
    COREWEBVIEW2_COLOR color{};
    if (backdrop_ != "none" && !fullscreen_) {
        color = {0, 0, 0, 0};  // transparent: let the DWM backdrop show through
    } else {
        COLORREF bg = backgroundColor();
        color = {255, GetRValue(bg), GetGValue(bg), GetBValue(bg)};
    }
    c2->put_DefaultBackgroundColor(color);
}

/* ------------------------------------------------------------------------ */
/* Theme / backdrop                                                          */
/* ------------------------------------------------------------------------ */

bool MainWindow::systemPrefersDark() {
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return false;
    return value == 0;
}

bool MainWindow::effectiveDark() const {
    if (themeSource_ == "dark") return true;
    if (themeSource_ == "light") return false;
    return systemPrefersDark();
}

COLORREF MainWindow::backgroundColor() const { return effectiveDark() ? RGB(0x1b, 0x1d, 0x22) : RGB(0xff, 0xff, 0xff); }

void MainWindow::applyFrameTheme() {
    if (!hwnd_) return;
    BOOL dark = effectiveDark() ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

void MainWindow::applyBackdrop() {
    if (!hwnd_) return;
    int type = 1;  // DWMSBT_NONE
    if (backdrop_ == "mica") type = 2;          // DWMSBT_MAINWINDOW
    else if (backdrop_ == "acrylic") type = 3;  // DWMSBT_TRANSIENTWINDOW
    if (fullscreen_) type = 1;
    MARGINS m = type == 1 ? MARGINS{0, 0, 1, 0} : MARGINS{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd_, &m);
    DwmSetWindowAttribute(hwnd_, DWMWA_SYSTEMBACKDROP_TYPE, &type, sizeof(type));
    applyControllerBackground();
}

void MainWindow::setThemeSource(const std::string& source) {
    themeSource_ = (source == "light" || source == "dark") ? source : "system";
    applyFrameTheme();
    applyControllerBackground();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void MainWindow::setBackgroundMaterial(const std::string& material) {
    backdrop_ = (material == "mica" || material == "acrylic") ? material : "none";
    applyBackdrop();
}

void MainWindow::setUiScale(double factor) {
    if (!std::isfinite(factor)) return;
    uiScale_ = std::clamp(factor, 0.25, 5.0);
    if (web_->controller) web_->controller->put_ZoomFactor(uiScale_);
}

/* ------------------------------------------------------------------------ */
/* Window control                                                            */
/* ------------------------------------------------------------------------ */

void MainWindow::minimize() { ShowWindow(hwnd_, SW_MINIMIZE); }

void MainWindow::toggleMaximize() {
    if (fullscreen_) toggleFullscreen();
    ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
}

void MainWindow::requestClose() { PostMessageW(hwnd_, WM_CLOSE, 0, 0); }

bool MainWindow::isMaximized() const { return hwnd_ && IsZoomed(hwnd_); }

void MainWindow::setTitle(const std::string& title) { SetWindowTextW(hwnd_, widen(title.empty() ? "SSH Crawler" : title).c_str()); }

void MainWindow::flash() {
    if (GetForegroundWindow() == hwnd_) return;
    FLASHWINFO fi{sizeof(fi), hwnd_, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
    FlashWindowEx(&fi);
}

void MainWindow::toggleFullscreen() {
    if (!fullscreen_) {
        preFullscreen_.length = sizeof(preFullscreen_);
        GetWindowPlacement(hwnd_, &preFullscreen_);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        fullscreen_ = true;
        SetWindowLongPtrW(hwnd_, GWL_STYLE, (GetWindowLongPtrW(hwnd_, GWL_STYLE) & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        fullscreen_ = false;
        SetWindowLongPtrW(hwnd_, GWL_STYLE, (GetWindowLongPtrW(hwnd_, GWL_STYLE) & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd_, &preFullscreen_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    applyBackdrop();
    if (bridge_) bridge_->emit("app:onFullscreenChanged", Json::array({fullscreen_}));
}

void MainWindow::confirmClose(bool allow) {
    closeRequestedAt_ = 0;
    if (!allow) return;
    closeConfirmed_ = true;
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

void MainWindow::quit() {
    closeConfirmed_ = true;
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

void MainWindow::activate() {
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    // Allowed because the request came from the foreground process (the second instance).
    SetForegroundWindow(hwnd_);
}

/* ------------------------------------------------------------------------ */
/* Placement                                                                 */
/* ------------------------------------------------------------------------ */

void MainWindow::restorePlacement(int showCommand) {
    Json st = store_.getState("window");
    bool restored = false;
    if (st.is_object() && st.contains("w") && st.contains("h")) {
        RECT r{st.value("x", 100), st.value("y", 100), 0, 0};
        r.right = r.left + std::max(st.value("w", 1280), kMinWidth);
        r.bottom = r.top + std::max(st.value("h", 800), kMinHeight);
        // Only restore if a reasonable part of the window is on a monitor.
        RECT probe = r;
        probe.bottom = probe.top + 40;
        if (MonitorFromRect(&probe, MONITOR_DEFAULTTONULL)) {
            WINDOWPLACEMENT wp{sizeof(wp)};
            wp.rcNormalPosition = r;
            wp.showCmd = (st.value("maximized", false) && showCommand != SW_SHOWMINIMIZED && showCommand != SW_MINIMIZE)
                             ? SW_SHOWMAXIMIZED
                             : (showCommand == SW_SHOWDEFAULT ? SW_SHOWNORMAL : static_cast<UINT>(showCommand));
            SetWindowPlacement(hwnd_, &wp);
            restored = true;
        }
    }
    if (!restored) {
        // Center a comfortable default size on the primary work area.
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &mi);
        const UINT d = dpi();
        const int w = std::min(scale(1280, d), static_cast<int>(mi.rcWork.right - mi.rcWork.left) - 40);
        const int h = std::min(scale(800, d), static_cast<int>(mi.rcWork.bottom - mi.rcWork.top) - 40);
        SetWindowPos(hwnd_, nullptr, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2,
                     mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2, w, h, SWP_NOZORDER);
        ShowWindow(hwnd_, showCommand == SW_SHOWDEFAULT ? SW_SHOWNORMAL : showCommand);
    }
    lastMaximized_ = IsZoomed(hwnd_) != FALSE;
}

void MainWindow::savePlacement(bool synchronous) {
    if (!hwnd_ || fullscreen_) return;
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!GetWindowPlacement(hwnd_, &wp)) return;
    const RECT& r = wp.rcNormalPosition;
    Json st = {{"x", r.left},
               {"y", r.top},
               {"w", r.right - r.left},
               {"h", r.bottom - r.top},
               {"maximized", wp.showCmd == SW_SHOWMAXIMIZED || IsZoomed(hwnd_)}};
    if (synchronous) {
        ++*placementSeq_;  // drop pending debounced saves
        store_.setState("window", st);
        return;
    }
    // Debounce: resizing fires many WM_SIZE messages.
    const uint64_t seq = ++*placementSeq_;
    auto seqPtr = placementSeq_;
    Store* store = &store_;
    ThreadPool::shared().submit([seq, seqPtr, store, st] {
        Sleep(600);
        if (seqPtr->load() == seq) store->setState("window", st);
    });
}

}  // namespace sc
