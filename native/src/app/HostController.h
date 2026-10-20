#pragma once
// The host window's portable policy, connected through its hooks (platform/ui/HostWindow.h does the OS side):
//  - serves the bundled UI (app/UiResources); only the app's own origin may load in the window and talk to the
//    bridge, other links open in the browser (http, https and mailto only);
//  - window events become bridge events (menu bar commands and the title bar insets too); a (re)loaded page first
//    gets the window state;
//  - closing while sessions are active asks the page (app:onCloseRequested -> app:confirmClose), and closes anyway
//    on a close request that comes more than closeAnswerTimeout after an unanswered one;
//  - the window placement is kept in state.json ("window": x, y, w, h, maximized), debounced while resizing.
// Everything runs on the UI thread, except that the debounced placement is written on the thread pool.
#include "app/UiResources.h"
#include "platform/ui/HostWindow.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace st {

class Bridge;
class Store;

/** "scheme://host[:port]" of a URL in lower case; '' when it has no "://". */
std::string originOf(const std::string& url);
/** Links the app may hand to the OS (app:openExternal, links leaving the app): http, https and mailto. */
bool isSafeExternalUrl(const std::string& url);

class HostController {
public:
    /** Takes over `window`'s hooks (before window.create()). devUrl: load the dev server instead of the bundled UI. */
    HostController(ui::HostWindow& window, Bridge& bridge, Store& store, const std::string& devUrl);
    /** Releases the window's hooks. */
    ~HostController();
    HostController(const HostController&) = delete;
    HostController& operator=(const HostController&) = delete;

    ui::HostWindow& window() { return window_; }
    /** Origin of the page's own content: the bundled UI's or the dev server's. */
    const std::string& appOrigin() const { return appOrigin_; }

    /** app:confirmClose: the page's answer to app:onCloseRequested. */
    void confirmClose(bool allow);
    /** app:quit: closes without asking. */
    void quit();

    /** Number of connecting/connected sessions: closing asks the page to confirm while > 0. */
    std::function<int()> activeSessionCount;
    /** The page starts (re)loading: what was handed to it must be offered again (open requests). */
    std::function<void()> onPageLoading;
    /** Hands a link that leaves the app to the OS (ui::shell::openExternal; tests replace it). */
    std::function<void(const std::string& url)> openExternal;

    /** A close request this long after an unanswered app:onCloseRequested closes without asking again. */
    std::chrono::milliseconds closeAnswerTimeout{5000};
    /** Placement changes are written once they have been quiet for this long. */
    std::chrono::milliseconds placementDelay{600};

private:
    bool isAppOrigin(const std::string& uri) const;
    std::optional<ui::UiFile> serve(const std::string& uri) const;
    void onWebMessage(const std::string& source, const std::string& json, const std::vector<std::string>& files);
    bool onNavigationStarting(const std::string& uri);
    bool onCloseRequested();
    std::optional<ui::WindowPlacement> loadPlacement() const;
    void onPlacementChanged(const ui::WindowPlacement& placement, bool final);
    void emitInsets();

    ui::HostWindow& window_;
    Bridge& bridge_;
    Store& store_;
    std::string appOrigin_;
    UiResources files_;
    bool pageAlive_ = false;  // the page talked to us since the last navigation
    std::optional<ui::ChromeInsets> insets_;  // the last ones the window reported
    bool closeConfirmed_ = false;
    std::optional<std::chrono::steady_clock::time_point> closeRequestedAt_;  // of an unanswered app:onCloseRequested
    std::shared_ptr<std::atomic<uint64_t>> placementSeq_;
};

}  // namespace st
