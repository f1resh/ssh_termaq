#include "app/HostController.h"

#include "bridge/Bridge.h"
#include "core/Log.h"
#include "core/Strings.h"
#include "core/Threading.h"
#include "platform/ui/Shell.h"
#include "store/Store.h"

#include <thread>

namespace st {

std::string originOf(const std::string& url) {
    // scheme://host[:port]
    const auto p = url.find("://");
    if (p == std::string::npos) return {};
    const auto end = url.find_first_of("/?#", p + 3);
    return toLower(url.substr(0, end));
}

bool isSafeExternalUrl(const std::string& url) {
    const std::string l = toLower(url);
    return startsWith(l, "http://") || startsWith(l, "https://") || startsWith(l, "mailto:");
}

HostController::HostController(ui::HostWindow& window, Bridge& bridge, Store& store, const std::string& devUrl)
    : openExternal([](const std::string& url) { ui::shell::openExternal(url); }),
      window_(window),
      bridge_(bridge),
      store_(store),
      placementSeq_(std::make_shared<std::atomic<uint64_t>>(0)) {
    if (!devUrl.empty()) {
        appOrigin_ = originOf(devUrl);
    } else {
        appOrigin_ = ui::bundledUiOrigin();
        files_ = UiResources::load();
        applog::info("Embedded UI: " + std::to_string(files_.size()) + " file(s)");
    }

    window_.serveUi = [this](const std::string& uri) { return serve(uri); };
    window_.onWebMessage = [this](const std::string& source, const std::string& json,
                                  const std::vector<std::string>& files) { onWebMessage(source, json, files); };
    window_.onNavigationStarting = [this](const std::string& uri) { return onNavigationStarting(uri); };
    window_.onNewWindowRequested = [this](const std::string& uri) {
        if (isSafeExternalUrl(uri) && openExternal) openExternal(uri);
    };
    window_.onCloseRequested = [this] { return onCloseRequested(); };
    window_.onFocusChanged = [this](bool focused) { bridge_.emit("window:onFocusChanged", Json::array({focused})); };
    window_.onMaximizedChanged = [this](bool maximized) {
        bridge_.emit("window:onMaximizedChanged", Json::array({maximized}));
    };
    window_.onFullscreenChanged = [this](bool fullscreen) {
        bridge_.emit("app:onFullscreenChanged", Json::array({fullscreen}));
    };
    window_.onSystemThemeChanged = [this](bool dark) { bridge_.emit("app:onSystemThemeChanged", Json::array({dark})); };
    window_.onMenuCommand = [this](const std::string& command) { bridge_.emit("window:onMenuCommand", Json::array({command})); };
    window_.onChromeInsetsChanged = [this](const ui::ChromeInsets& insets) {
        insets_ = insets;
        emitInsets();
    };
    window_.loadPlacement = [this] { return loadPlacement(); };
    window_.onPlacementChanged = [this](const ui::WindowPlacement& p, bool final) { onPlacementChanged(p, final); };
}

HostController::~HostController() {
    ++*placementSeq_;  // drop pending debounced saves
    window_.serveUi = nullptr;
    window_.onWebMessage = nullptr;
    window_.onNavigationStarting = nullptr;
    window_.onNewWindowRequested = nullptr;
    window_.onCloseRequested = nullptr;
    window_.onFocusChanged = nullptr;
    window_.onMaximizedChanged = nullptr;
    window_.onFullscreenChanged = nullptr;
    window_.onSystemThemeChanged = nullptr;
    window_.onMenuCommand = nullptr;
    window_.onChromeInsetsChanged = nullptr;
    window_.loadPlacement = nullptr;
    window_.onPlacementChanged = nullptr;
}

/* ------------------------------------------------------------------------ */
/* The page                                                                  */
/* ------------------------------------------------------------------------ */

bool HostController::isAppOrigin(const std::string& uri) const { return !appOrigin_.empty() && originOf(uri) == appOrigin_; }

std::optional<ui::UiFile> HostController::serve(const std::string& uri) const {
    const auto path = uiPathFromUri(uri, appOrigin_);
    if (!path) return std::nullopt;
    const auto data = files_.find(*path);
    if (!data) return std::nullopt;
    return ui::UiFile{*data, uiContentType(*path)};
}

bool HostController::onNavigationStarting(const std::string& uri) {
    if (startsWith(uri, "data:") || uri == "about:blank") return true;
    if (!isAppOrigin(uri)) {
        if (isSafeExternalUrl(uri) && openExternal) openExternal(uri);
        return false;
    }
    pageAlive_ = false;
    if (onPageLoading) onPageLoading();
    return true;
}

void HostController::onWebMessage(const std::string& source, const std::string& json,
                                  const std::vector<std::string>& files) {
    if (!source.empty() && !isAppOrigin(source)) {
        applog::warn("Ignoring web message from a foreign origin");
        return;
    }
    if (!pageAlive_) {
        pageAlive_ = true;
        // A (re)loaded UI starts from scratch: tell it about the current window state.
        bridge_.emit("window:onMaximizedChanged", Json::array({window_.isMaximized()}));
        bridge_.emit("app:onFullscreenChanged", Json::array({window_.isFullscreen()}));
        if (insets_) emitInsets();
    }
    bridge_.onWebMessage(json, files);
}

void HostController::emitInsets() {
    bridge_.emit("window:onChromeInsets", Json::array({Json{{"left", insets_->left}, {"right", insets_->right}}}));
}

/* ------------------------------------------------------------------------ */
/* Closing                                                                   */
/* ------------------------------------------------------------------------ */

bool HostController::onCloseRequested() {
    const int active = activeSessionCount ? activeSessionCount() : 0;
    const auto now = std::chrono::steady_clock::now();
    const bool pendingTooLong = closeRequestedAt_ && now - *closeRequestedAt_ > closeAnswerTimeout;
    if (closeConfirmed_ || !pageAlive_ || active == 0 || pendingTooLong) return true;
    closeRequestedAt_ = now;
    bridge_.emit("app:onCloseRequested", Json::array());
    return false;
}

void HostController::confirmClose(bool allow) {
    closeRequestedAt_.reset();
    if (!allow) return;
    closeConfirmed_ = true;
    window_.requestClose();
}

void HostController::quit() {
    closeConfirmed_ = true;
    window_.requestClose();
}

/* ------------------------------------------------------------------------ */
/* Placement                                                                 */
/* ------------------------------------------------------------------------ */

std::optional<ui::WindowPlacement> HostController::loadPlacement() const {
    const Json st = store_.getState("window");
    if (!st.is_object() || !st.contains("w") || !st.contains("h")) return std::nullopt;
    ui::WindowPlacement p;
    p.x = st.value("x", 100);
    p.y = st.value("y", 100);
    p.width = st.value("w", 1280);
    p.height = st.value("h", 800);
    p.maximized = st.value("maximized", false);
    return p;
}

void HostController::onPlacementChanged(const ui::WindowPlacement& p, bool final) {
    const Json st = {{"x", p.x}, {"y", p.y}, {"w", p.width}, {"h", p.height}, {"maximized", p.maximized}};
    if (final) {
        ++*placementSeq_;  // drop pending debounced saves
        store_.setState("window", st);
        return;
    }
    // Debounce: resizing reports many changes.
    const uint64_t seq = ++*placementSeq_;
    auto seqPtr = placementSeq_;
    Store* store = &store_;
    const auto delay = placementDelay;
    ThreadPool::shared().submit([seq, seqPtr, store, st, delay] {
        std::this_thread::sleep_for(delay);
        if (seqPtr->load() == seq) store->setState("window", st);
    });
}

}  // namespace st
