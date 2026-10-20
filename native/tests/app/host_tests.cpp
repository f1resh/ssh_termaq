// HostController (app/HostController.h), the host window's portable policy, against a fake window: which pages may
// load and talk to the bridge, window events for the page, confirming close while sessions are active, the saved
// placement, and serving the bundled UI (the files embedded into this test executable, see ui_tests.cpp).
#include "app/HostController.h"
#include "store/Store.h"

#include "TestBridge.h"
#include "TestServers.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

using namespace st;
using namespace st::test;
using namespace std::chrono_literals;

namespace {

constexpr const char* kDevUrl = "http://localhost:5173/";
constexpr const char* kDevOrigin = "http://localhost:5173";

/** A window that only records what the controller asks of it; requestClose() runs the close hook like the OS does. */
struct FakeWindow final : ui::HostWindow {
    bool maximized = false;
    bool fullscreen = false;
    int closeRequests = 0;
    bool closed = false;

    void post(std::function<void()> fn) override { fn(); }
    bool isUiThread() const override { return true; }
    bool create() override { return true; }
    void* nativeHandle() const override { return nullptr; }
    std::string browserVersion() const override { return "fake"; }
    void postWebMessage(const std::string&) override {}
    void minimize() override {}
    void toggleMaximize() override { maximized = !maximized; }
    bool isMaximized() const override { return maximized; }
    void toggleFullscreen() override { fullscreen = !fullscreen; }
    bool isFullscreen() const override { return fullscreen; }
    void requestClose() override {
        ++closeRequests;
        if (!onCloseRequested || onCloseRequested()) closed = true;
    }
    void setTitle(const std::string&) override {}
    void flash() override {}
    void setThemeSource(const std::string&) override {}
    void setBackgroundMaterial(const std::string&) override {}
    void setUiScale(double) override {}
};

struct HostFixture {
    TempDir data;
    Store store{data.path()};
    BridgeHarness h;
    FakeWindow window;
    HostController host;
    std::vector<std::string> opened;  // links handed to the OS
    int pings = 0;
    int pageLoads = 0;

    explicit HostFixture(const std::string& devUrl = kDevUrl) : host(window, h.bridge, store, devUrl) {
        store.load();
        host.openExternal = [this](const std::string& url) { opened.push_back(url); };
        host.onPageLoading = [this] { ++pageLoads; };
        h.bridge.on("test:ping", [this](const Json&) { ++pings; });
    }

    /** A message from the page (a bridge "send"), as the window reports it. */
    void pageSends(const std::string& source) {
        window.onWebMessage(source, Json{{"t", "send"}, {"ch", "test:ping"}, {"args", Json::array()}}.dump(), {});
    }
};

}  // namespace

TEST_CASE("host: only the app's own pages load; http(s) and mailto links go to the browser", "[app][host]") {
    HostFixture f;
    REQUIRE(f.host.appOrigin() == kDevOrigin);
    CHECK(f.window.onNavigationStarting(std::string(kDevOrigin) + "/index.html"));
    CHECK(f.window.onNavigationStarting("HTTP://LOCALHOST:5173/other?x=1"));  // origins compare case-insensitively
    CHECK(f.pageLoads == 2);
    CHECK(f.window.onNavigationStarting("about:blank"));
    CHECK(f.window.onNavigationStarting("data:text/html,<p>x"));
    CHECK(f.pageLoads == 2);  // neither is the app's page

    CHECK_FALSE(f.window.onNavigationStarting("https://example.com/docs"));
    CHECK_FALSE(f.window.onNavigationStarting("http://localhost:5174/"));
    CHECK_FALSE(f.window.onNavigationStarting("file:///C:/Windows/win.ini"));
    f.window.onNewWindowRequested("mailto:someone@example.com");
    f.window.onNewWindowRequested("javascript:alert(1)");
    CHECK(f.opened == std::vector<std::string>{"https://example.com/docs", "http://localhost:5174/", "mailto:someone@example.com"});
}

TEST_CASE("host: messages from other origins are ignored; a new page first gets the window state", "[app][host]") {
    HostFixture f;
    f.window.maximized = true;
    f.pageSends("https://evil.example");
    CHECK(f.pings == 0);
    CHECK(f.h.eventsOn("window:onMaximizedChanged").empty());

    f.pageSends(kDevOrigin);
    f.pageSends("");  // origin unknown: accepted, as before
    CHECK(f.pings == 2);
    auto maximized = f.h.eventsOn("window:onMaximizedChanged");
    REQUIRE(maximized.size() == 1);
    CHECK(maximized[0]["args"] == Json::array({true}));
    CHECK(f.h.eventsOn("app:onFullscreenChanged").size() == 1);

    // The page reloads: it gets the state again with its first message.
    CHECK(f.window.onNavigationStarting(std::string(kDevOrigin) + "/"));
    f.pageSends(kDevOrigin);
    CHECK(f.h.eventsOn("window:onMaximizedChanged").size() == 2);

    // Window events reach the page.
    f.window.onFocusChanged(false);
    f.window.onSystemThemeChanged(true);
    CHECK(f.h.eventsOn("window:onFocusChanged").at(0)["args"] == Json::array({false}));
    CHECK(f.h.eventsOn("app:onSystemThemeChanged").at(0)["args"] == Json::array({true}));
}

TEST_CASE("host: closing asks the page while sessions are active", "[app][host]") {
    HostFixture f;
    int active = 0;
    f.host.activeSessionCount = [&] { return active; };

    SECTION("no sessions, or no page yet: closes at once") {
        f.window.requestClose();
        CHECK(f.window.closed);
        f.window.closed = false;
        active = 1;
        f.window.requestClose();  // the page never talked to us
        CHECK(f.window.closed);
    }
    SECTION("the page answers") {
        active = 1;
        f.pageSends(kDevOrigin);
        f.window.requestClose();
        CHECK_FALSE(f.window.closed);
        CHECK(f.h.eventsOn("app:onCloseRequested").size() == 1);

        f.host.confirmClose(false);
        CHECK(f.window.closeRequests == 1);
        f.window.requestClose();  // asks again
        CHECK_FALSE(f.window.closed);
        CHECK(f.h.eventsOn("app:onCloseRequested").size() == 2);

        f.host.confirmClose(true);
        CHECK(f.window.closed);
        CHECK(f.window.closeRequests == 3);
    }
    SECTION("the page does not answer: the next close request after the timeout closes") {
        f.host.closeAnswerTimeout = 50ms;
        active = 1;
        f.pageSends(kDevOrigin);
        f.window.requestClose();
        f.window.requestClose();  // still within the timeout: asks again
        CHECK_FALSE(f.window.closed);
        std::this_thread::sleep_for(120ms);
        f.window.requestClose();
        CHECK(f.window.closed);
    }
    SECTION("quit closes without asking") {
        active = 1;
        f.pageSends(kDevOrigin);
        f.host.quit();
        CHECK(f.window.closed);
        CHECK(f.h.eventsOn("app:onCloseRequested").empty());
    }
}

TEST_CASE("host: the window placement is kept in state.json", "[app][host]") {
    HostFixture f;
    CHECK_FALSE(f.window.loadPlacement());
    f.store.setState("window", Json{{"x", 10}, {"y", 20}});
    CHECK_FALSE(f.window.loadPlacement());  // no size: not a placement

    f.window.onPlacementChanged({11, 22, 900, 700, true}, true);
    CHECK(f.store.getState("window") == Json{{"x", 11}, {"y", 22}, {"w", 900}, {"h", 700}, {"maximized", true}});
    auto p = f.window.loadPlacement();
    REQUIRE(p);
    CHECK((p->x == 11 && p->y == 22 && p->width == 900 && p->height == 700 && p->maximized));

    // While resizing, changes are written once they settle; the final placement wins over a pending one.
    f.host.placementDelay = 30ms;
    f.window.onPlacementChanged({1, 2, 800, 600, false}, false);
    f.window.onPlacementChanged({3, 4, 801, 601, false}, false);
    CHECK(f.store.getState("window")["w"] == 900);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (f.store.getState("window")["w"] != 801 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(10ms);
    CHECK(f.store.getState("window") == Json{{"x", 3}, {"y", 4}, {"w", 801}, {"h", 601}, {"maximized", false}});

    f.window.onPlacementChanged({5, 6, 802, 602, false}, false);
    f.window.onPlacementChanged({7, 8, 803, 603, true}, true);
    std::this_thread::sleep_for(150ms);
    CHECK(f.store.getState("window")["x"] == 7);
}

TEST_CASE("host: the bundled UI is served under its origin", "[app][host]") {
    HostFixture f("");
    REQUIRE(f.host.appOrigin() == ui::bundledUiOrigin());
    auto html = f.window.serveUi(f.host.appOrigin() + "/");
    REQUIRE(html);
    CHECK(html->body == "<!doctype html><title>embedded</title>");
    CHECK(html->contentType == "text/html; charset=utf-8");
    CHECK(f.window.serveUi(f.host.appOrigin() + "/assets/app.js"));
    CHECK_FALSE(f.window.serveUi(f.host.appOrigin() + "/missing.js"));
    CHECK_FALSE(f.window.serveUi("https://elsewhere.example/index.html"));
}

TEST_CASE("host: menu bar commands and the title bar insets reach the page", "[app][host]") {
    HostFixture f;
    f.window.onMenuCommand("tab.new");
    CHECK(f.h.eventsOn("window:onMenuCommand").at(0)["args"] == Json::array({"tab.new"}));

    // Insets reported before the page talks are handed over once it does, and again after a reload.
    f.window.onChromeInsetsChanged(ui::ChromeInsets{86, 0});
    f.pageSends(kDevOrigin);
    auto insets = f.h.eventsOn("window:onChromeInsets");
    REQUIRE(insets.size() == 2);
    CHECK(insets[1]["args"] == Json::array({Json{{"left", 86.0}, {"right", 0.0}}}));
    CHECK(f.window.onNavigationStarting(std::string(kDevOrigin) + "/"));
    f.pageSends(kDevOrigin);
    CHECK(f.h.eventsOn("window:onChromeInsets").size() == 3);
}

TEST_CASE("host: the controller releases the window's hooks", "[app][host]") {
    TempDir data;
    Store store(data.path());
    store.load();
    BridgeHarness h;
    FakeWindow window;
    {
        HostController host(window, h.bridge, store, kDevUrl);
        CHECK(window.onCloseRequested);
        CHECK(window.onWebMessage);
    }
    CHECK_FALSE(window.onCloseRequested);
    CHECK_FALSE(window.onWebMessage);
    CHECK_FALSE(window.serveUi);
    CHECK_FALSE(window.onMenuCommand);
    CHECK_FALSE(window.onChromeInsetsChanged);
    window.requestClose();
    CHECK(window.closed);
}
