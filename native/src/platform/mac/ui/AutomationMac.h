#pragma once
// Private to platform/mac/ui, Objective-C++ (include from .mm files): the Debug builds' automation socket, over which
// ui/scripts/app.mjs drives the window as Playwright drives WebView2 over CDP on Windows.
//
// Protocol: one client at a time; one JSON object per line each way.
//   -> {"id": n, "cmd": "eval", "fn": "<function source>", "arg": "<JSON>"}   runs fn(arg) in the page (may be async)
//   -> {"id": n, "cmd": "screenshot", "path": "<file.png>"}                    the page as a PNG
//   -> {"id": n, "cmd": "type", "text": "..."}                                  real key presses, one per character
//   -> {"id": n, "cmd": "press", "key": "Meta+T"}                               one key with modifiers (Playwright names)
//   -> {"id": n, "cmd": "click", "x": 10, "y": 20}                              a real click at CSS px of the page
//   -> {"id": n, "cmd": "sheet", "check": "Apply to All", "press": "Keep Both"}  the window's sheet: {texts, buttons}
//                                                                               or null; check / press are optional
//   <- {"id": n, "ok": true, "result": <JSON>} | {"id": n, "ok": false, "error": "..."}
//   <- {"event": "console", "type": "error", "text": "..."}                     the page's console and errors
// Key presses and clicks are AppKit events sent to the window, so the page sees trusted input and Command keys go
// through the menu bar as they do for the user.
#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

#include <memory>
#include <string>

namespace st::ui::mac {

class Automation {
public:
    /** Listens on `socketPath` (replaced; owner-only). nullptr with `error` when it cannot. UI thread. */
    static std::unique_ptr<Automation> start(const std::string& socketPath, NSWindow* window, WKWebView* web,
                                             std::string& error);
    ~Automation();
    Automation(const Automation&) = delete;
    Automation& operator=(const Automation&) = delete;

    /** A console message or page error for the client (UI thread). */
    void console(const std::string& type, const std::string& text);
    /** The user script that reports the page's console to console() through the "stauto" message handler. */
    static NSString* consoleHookScript();

    struct Impl;

private:
    explicit Automation(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

}  // namespace st::ui::mac
