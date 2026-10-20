// The main window on macOS: an NSWindow whose content is a WKWebView. The page draws the title bar (full-size content,
// transparent title bar; the traffic lights stay over the page). The bundled UI is served at sshtermaq://app/ by a URL
// scheme handler. Bridge messages (docs/ARCHITECTURE.md, "Bridge"):
//   page -> host: window.webkit.messageHandlers.st.postMessage(json), from the main frame only;
//   host -> page: everything queued in one run-loop turn goes as one JSON array string through callAsyncJavaScript to
//                 window.__stReceive, at most ~1 MB per call and 4 calls in flight (the rest waits here).
// The Dispatcher is a run-loop source in the common modes, so posted work also runs while a menu is tracked or a modal
// panel is up (the GCD main queue would wait).
// The page's title bar: the page reports its height and drag areas (setTitleBarLayout); the traffic lights are centred
// in it and their room is reported back (onChromeInsetsChanged); a press in a drag area moves the window, a double
// click there does what System Settings say (zoom or minimize). The page's commands fill the menu bar (AppMac).
// Debug builds with --automation: ui/scripts/app.mjs drives the window over a Unix socket (AutomationMac.h).
#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

#include "platform/ui/HostWindow.h"

#include "core/Log.h"
#include "platform/mac/ui/AppMac.h"
#include "platform/mac/ui/AutomationMac.h"
#include "platform/mac/ui/DragDropMac.h"

#include <pthread.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace {

constexpr size_t kMaxBatchBytes = 1 << 20;
constexpr int kMaxCallsInFlight = 4;

NSString* ns(const std::string& s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @"";
}

std::string str(NSString* s) { return s ? std::string(s.UTF8String) : std::string(); }

}  // namespace

namespace st::ui {
class HostWindowMac;
}

/** Window, navigation, script message and URL scheme callbacks, forwarded to the C++ window while it is attached. */
@interface STHostDelegate : NSObject <NSWindowDelegate, WKNavigationDelegate, WKUIDelegate, WKScriptMessageHandler, WKURLSchemeHandler>
@property(nonatomic, assign) st::ui::HostWindowMac* owner;
@end

/** The web view: the page's title bar drag areas, browser navigation left out of its context menu, and its side of
 *  drag & drop (DragDropMac.h). */
@interface STWebView : WKWebView
- (void)setDragAreas:(std::vector<st::ui::PageRect>)drag except:(std::vector<st::ui::PageRect>)noDrag;
@end

@implementation STWebView {
    std::vector<st::ui::PageRect> _drag;    // CSS px
    std::vector<st::ui::PageRect> _noDrag;
}

- (void)setDragAreas:(std::vector<st::ui::PageRect>)drag except:(std::vector<st::ui::PageRect>)noDrag {
    _drag = std::move(drag);
    _noDrag = std::move(noDrag);
}

- (BOOL)isInDragArea:(NSEvent*)event {
    if (_drag.empty()) return NO;
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    const double zoom = self.pageZoom > 0 ? self.pageZoom : 1.0;
    const double x = p.x / zoom, y = (self.isFlipped ? p.y : self.bounds.size.height - p.y) / zoom;
    auto inside = [x, y](const st::ui::PageRect& r) { return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height; };
    return std::any_of(_drag.begin(), _drag.end(), inside) && std::none_of(_noDrag.begin(), _noDrag.end(), inside);
}

- (void)mouseDown:(NSEvent*)event {
    if (![self isInDragArea:event]) {
        st::ui::dnd::mac::noteMouseEvent(self, event);
        return [super mouseDown:event];
    }
    if (event.clickCount == 2) {
        NSString* action = [NSUserDefaults.standardUserDefaults stringForKey:@"AppleActionOnDoubleClick"];
        if ([action isEqualToString:@"Minimize"]) [self.window performMiniaturize:nil];
        else if (![action isEqualToString:@"None"]) [self.window performZoom:nil];
        return;
    }
    [self.window performWindowDragWithEvent:event];
}

- (void)mouseDragged:(NSEvent*)event {
    st::ui::dnd::mac::noteMouseEvent(self, event);
    [super mouseDragged:event];
}

- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender {
    if (st::ui::dnd::mac::takeOwnDrop(sender, self)) {
        // Our own drag: the page hears of it as "internal" from the drag's result, not as a drop.
        if ([WKWebView instancesRespondToSelector:@selector(draggingExited:)]) [super draggingExited:sender];
        return NO;
    }
    return [WKWebView instancesRespondToSelector:@selector(prepareForDragOperation:)] ? [super prepareForDragOperation:sender] : YES;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    st::ui::dnd::mac::noteDroppedFiles(sender.draggingPasteboard);  // for files:pathsForFiles, which the drop leads to
    return [super performDragOperation:sender];
}

- (void)willOpenMenu:(NSMenu*)menu withEvent:(NSEvent*)event {
    static NSArray<NSString*>* const drop = @[ @"Reload", @"GoBack", @"GoForward", @"InNewWindow", @"Download" ];
    for (NSMenuItem* item in [menu.itemArray copy]) {
        for (NSString* part in drop)
            if ([item.identifier containsString:part]) [menu removeItem:item];
    }
    while (menu.numberOfItems && [menu itemAtIndex:0].isSeparatorItem) [menu removeItemAtIndex:0];
    while (menu.numberOfItems && [menu itemAtIndex:menu.numberOfItems - 1].isSeparatorItem) [menu removeItemAtIndex:menu.numberOfItems - 1];
    [super willOpenMenu:menu withEvent:event];
}

@end

namespace st::ui {

class HostWindowMac final : public HostWindow {
public:
    explicit HostWindowMac(HostWindowOptions options);
    ~HostWindowMac() override;

    void post(std::function<void()> fn) override;
    bool isUiThread() const override { return pthread_main_np() != 0; }

    bool create() override;
    void* nativeHandle() const override { return (__bridge void*)window_; }
    std::string browserVersion() const override;
    void postWebMessage(const std::string& json) override;

    void minimize() override { [window_ miniaturize:nil]; }
    void toggleMaximize() override { [window_ zoom:nil]; }
    bool isMaximized() const override { return window_ && window_.isZoomed; }
    void toggleFullscreen() override { [window_ toggleFullScreen:nil]; }
    bool isFullscreen() const override { return window_ && (window_.styleMask & NSWindowStyleMaskFullScreen); }
    void requestClose() override { [window_ performClose:nil]; }
    void setTitle(const std::string& title) override {
        title_ = title;
        window_.title = ns(title);
    }
    void flash() override {
        if (!NSApp.isActive) [NSApp requestUserAttention:NSInformationalRequest];
    }
    void setThemeSource(const std::string& source) override;
    // Windows 11 backdrops; macOS keeps the page's own background.
    void setBackgroundMaterial(const std::string&) override {}
    void setUiScale(double factor) override {
        uiScale_ = std::clamp(factor, 0.25, 5.0);
        web_.pageZoom = uiScale_;
        layoutTitleBar();
    }
    void setTitleBarLayout(const TitleBarLayout& layout) override;
    void setMenuBar(const std::vector<MenuBarMenu>& menus) override;
    /** Centres the traffic lights in the page's title bar and reports the room they take. */
    void layoutTitleBar();

    /* ---- from STHostDelegate and the run-loop source (UI thread) ---- */
    void runPosted();
    bool shouldClose() { return !onCloseRequested || onCloseRequested(); }
    void willClose();
    void frameChanged();
    void focusChanged(bool focused) {
        layoutTitleBar();  // AppKit lays the title bar out again on activation changes
        if (onFocusChanged) onFocusChanged(focused);
    }
    void fullscreenChanged(bool fullscreen) {
        layoutTitleBar();
        if (onFullscreenChanged) onFullscreenChanged(fullscreen);
    }
    bool allowNavigation(const std::string& url) { return !onNavigationStarting || onNavigationStarting(url); }
    void newWindowRequested(const std::string& url) {
        if (onNewWindowRequested) onNewWindowRequested(url);
    }
    void navigationStarted();
    void navigationFinished();
    void webMessage(WKScriptMessage* message);
    void automationConsole(WKScriptMessage* message);
    void serve(id<WKURLSchemeTask> task);
    void contentProcessGone();
    void systemThemeChanged() {
        if (onSystemThemeChanged) onSystemThemeChanged(systemPrefersDark());
    }

private:
    void signal();
    void flushOutbox();
    NSRect initialFrame();
    WindowPlacement placement() const;

    HostWindowOptions options_;
    NSWindow* window_ = nil;
    STWebView* web_ = nil;
    STHostDelegate* delegate_ = nil;
    id themeObserver_ = nil;
    CFRunLoopSourceRef source_ = nullptr;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);  // blocks outliving the window check it

    std::mutex postMutex_;
    std::deque<std::function<void()>> posted_;

    std::deque<std::string> outbox_;  // bridge messages for the page
    bool pageReady_ = false;          // the current document has loaded
    unsigned generation_ = 0;         // of the current document: answers for an older one are ignored
    int callsInFlight_ = 0;

    std::string title_ = "SSHTermaq";
    std::string themeSource_ = "system";
    double uiScale_ = 1.0;
    double titleBarHeight_ = 0;          // CSS px, from the page (0: AppKit's own layout)
    std::optional<ChromeInsets> insets_;  // as last reported
    bool maximized_ = false;
    NSRect normalFrame_ = NSZeroRect;  // last frame neither zoomed nor full screen
    mac::AppEvents appEvents_;
    std::unique_ptr<mac::Automation> automation_;  // Debug builds with --automation
};

}  // namespace st::ui

@implementation STHostDelegate

/* ---- NSWindowDelegate ---- */

- (BOOL)windowShouldClose:(NSWindow*)sender { return !_owner || _owner->shouldClose(); }
- (void)windowWillClose:(NSNotification*)note {
    if (_owner) _owner->willClose();
}
- (void)windowDidResize:(NSNotification*)note {
    if (_owner) _owner->frameChanged();
}
- (void)windowDidMove:(NSNotification*)note {
    if (_owner) _owner->frameChanged();
}
- (void)windowDidBecomeKey:(NSNotification*)note {
    if (_owner) _owner->focusChanged(true);
}
- (void)windowDidResignKey:(NSNotification*)note {
    if (_owner) _owner->focusChanged(false);
}
- (void)windowDidEnterFullScreen:(NSNotification*)note {
    if (_owner) _owner->fullscreenChanged(true);
}
- (void)windowDidExitFullScreen:(NSNotification*)note {
    if (_owner) _owner->fullscreenChanged(false);
}

/* ---- WKNavigationDelegate / WKUIDelegate ---- */

- (void)webView:(WKWebView*)webView
    decidePolicyForNavigationAction:(WKNavigationAction*)action
                    decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
    const std::string url = str(action.request.URL.absoluteString);
    if (!action.targetFrame) {  // a new window (target=_blank)
        if (_owner) _owner->newWindowRequested(url);
        return decisionHandler(WKNavigationActionPolicyCancel);
    }
    if (action.targetFrame.isMainFrame && _owner && !_owner->allowNavigation(url))
        return decisionHandler(WKNavigationActionPolicyCancel);
    decisionHandler(WKNavigationActionPolicyAllow);
}

- (void)webView:(WKWebView*)webView didStartProvisionalNavigation:(WKNavigation*)navigation {
    if (_owner) _owner->navigationStarted();
}

- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
    if (_owner) _owner->navigationFinished();
}

- (void)webView:(WKWebView*)webView didFailProvisionalNavigation:(WKNavigation*)navigation withError:(NSError*)error {
    if (error.code != NSURLErrorCancelled) st::applog::warn("Page failed to load: " + str(error.localizedDescription));
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView*)webView {
    if (_owner) _owner->contentProcessGone();
}

- (WKWebView*)webView:(WKWebView*)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration*)configuration
               forNavigationAction:(WKNavigationAction*)action
                    windowFeatures:(WKWindowFeatures*)features {
    if (_owner) _owner->newWindowRequested(str(action.request.URL.absoluteString));
    return nil;
}

- (void)webView:(WKWebView*)webView
    runOpenPanelWithParameters:(WKOpenPanelParameters*)parameters
              initiatedByFrame:(WKFrameInfo*)frame
             completionHandler:(void (^)(NSArray<NSURL*>*))completionHandler {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.allowsMultipleSelection = parameters.allowsMultipleSelection;
    panel.canChooseDirectories = parameters.allowsDirectories;
    [panel beginSheetModalForWindow:webView.window completionHandler:^(NSModalResponse result) {
        completionHandler(result == NSModalResponseOK ? panel.URLs : nil);
    }];
}

/* ---- WKScriptMessageHandler / WKURLSchemeHandler ---- */

- (void)userContentController:(WKUserContentController*)controller didReceiveScriptMessage:(WKScriptMessage*)message {
    if (!_owner) return;
    if ([message.name isEqualToString:@"stauto"]) _owner->automationConsole(message);
    else _owner->webMessage(message);
}

- (void)webView:(WKWebView*)webView startURLSchemeTask:(id<WKURLSchemeTask>)task {
    if (_owner) {
        _owner->serve(task);
        return;
    }
    [task didFailWithError:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorCancelled userInfo:nil]];
}

- (void)webView:(WKWebView*)webView stopURLSchemeTask:(id<WKURLSchemeTask>)task {
    // Answers are complete when serve() returns: nothing to stop.
}

@end

namespace st::ui {

namespace {

void performPosted(void* info) { static_cast<HostWindowMac*>(info)->runPosted(); }

}  // namespace

HostWindowMac::HostWindowMac(HostWindowOptions options) : options_(std::move(options)) {
    delegate_ = [STHostDelegate new];
    delegate_.owner = this;
    CFRunLoopSourceContext ctx{};
    ctx.info = this;
    ctx.perform = performPosted;
    source_ = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &ctx);
    CFRunLoopAddSource(CFRunLoopGetMain(), source_, kCFRunLoopCommonModes);
}

HostWindowMac::~HostWindowMac() {
    *alive_ = false;
    automation_.reset();
    mac::setAppEvents(nullptr);
    mac::setMenuBar({}, nullptr);
    if (themeObserver_) [[NSDistributedNotificationCenter defaultCenter] removeObserver:themeObserver_];
    delegate_.owner = nullptr;
    if (web_) {
        [web_.configuration.userContentController removeScriptMessageHandlerForName:@"st"];
        if (!options_.automationSocket.empty())
            [web_.configuration.userContentController removeScriptMessageHandlerForName:@"stauto"];
        web_.navigationDelegate = nil;
        web_.UIDelegate = nil;
    }
    if (window_) {
        window_.delegate = nil;
        [window_ close];
    }
    CFRunLoopSourceInvalidate(source_);
    CFRelease(source_);
}

void HostWindowMac::post(std::function<void()> fn) {
    {
        std::lock_guard lock(postMutex_);
        posted_.push_back(std::move(fn));
    }
    signal();
}

void HostWindowMac::signal() {
    CFRunLoopSourceSignal(source_);
    CFRunLoopWakeUp(CFRunLoopGetMain());
}

void HostWindowMac::runPosted() {
    std::deque<std::function<void()>> tasks;
    {
        std::lock_guard lock(postMutex_);
        tasks.swap(posted_);
    }
    for (auto& task : tasks) task();
    flushOutbox();
}

/* ---- the page ---- */

void HostWindowMac::postWebMessage(const std::string& json) {
    outbox_.push_back(json);
    signal();  // flushed once this run-loop turn is over, with whatever else it queues
}

void HostWindowMac::flushOutbox() {
    if (!pageReady_ || !web_) return;
    while (callsInFlight_ < kMaxCallsInFlight && !outbox_.empty()) {
        std::string batch = "[";
        size_t count = 0;
        while (!outbox_.empty() && (count == 0 || batch.size() + outbox_.front().size() < kMaxBatchBytes)) {
            if (count++) batch += ',';
            batch += outbox_.front();
            outbox_.pop_front();
        }
        batch += ']';
        ++callsInFlight_;
        const unsigned generation = generation_;
        auto alive = alive_;
        [web_ callAsyncJavaScript:@"window.__stReceive && window.__stReceive(b)"
                        arguments:@{@"b" : ns(batch)}
                          inFrame:nil
                   inContentWorld:WKContentWorld.pageWorld
                completionHandler:^(id result, NSError* error) {
                    if (!*alive || generation != generation_) return;
                    if (error) applog::warn("Bridge delivery failed: " + str(error.localizedDescription));
                    --callsInFlight_;
                    flushOutbox();
                }];
    }
}

void HostWindowMac::navigationStarted() {
    // Messages for the document that goes away are not handed to the next one.
    pageReady_ = false;
    ++generation_;
    callsInFlight_ = 0;
    outbox_.clear();
}

void HostWindowMac::navigationFinished() {
    pageReady_ = true;
    flushOutbox();
}

void HostWindowMac::contentProcessGone() {
    applog::warn("The web content process ended; reloading the page");
    [web_ reload];
}

void HostWindowMac::webMessage(WKScriptMessage* message) {
    if (!message.frameInfo.isMainFrame || !onWebMessage) return;
    WKSecurityOrigin* o = message.frameInfo.securityOrigin;
    std::string source = str(o.protocol) + "://" + str(o.host);
    if (o.port) source += ":" + std::to_string(o.port);
    std::string json;
    if ([message.body isKindOfClass:[NSString class]]) {
        json = str(message.body);
    } else if ([NSJSONSerialization isValidJSONObject:message.body]) {
        NSData* data = [NSJSONSerialization dataWithJSONObject:message.body options:0 error:nil];
        json.assign(static_cast<const char*>(data.bytes), data.length);
    } else {
        return;
    }
    // Files dropped on the page: their paths, by the names the page sends (a File has no path in WebKit).
    std::vector<std::string> files;
    if (json.find("\"files:pathsForFiles\"") != std::string::npos) {
        NSDictionary* m = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:json.data() length:json.size()] options:0 error:nil];
        NSArray* args = [m isKindOfClass:[NSDictionary class]] ? m[@"args"] : nil;
        NSArray* names = [args isKindOfClass:[NSArray class]] && args.count ? args[0] : nil;
        std::vector<std::string> wanted;
        if ([names isKindOfClass:[NSArray class]])
            for (id n in names)
                if ([n isKindOfClass:[NSString class]]) wanted.emplace_back([n UTF8String]);
        files = dnd::mac::droppedPaths(wanted);
    }
    onWebMessage(source, json, files);
}

void HostWindowMac::automationConsole(WKScriptMessage* message) {
    if (!automation_ || ![message.body isKindOfClass:[NSString class]]) return;
    NSDictionary* m = [NSJSONSerialization JSONObjectWithData:[message.body dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
    if ([m isKindOfClass:[NSDictionary class]]) automation_->console(str(m[@"type"]), str(m[@"text"]));
}

void HostWindowMac::serve(id<WKURLSchemeTask> task) {
    NSURL* url = task.request.URL;
    const std::optional<UiFile> file = serveUi ? serveUi(str(url.absoluteString)) : std::nullopt;
    if (!file) {
        [task didReceiveResponse:[[NSHTTPURLResponse alloc] initWithURL:url statusCode:404 HTTPVersion:@"HTTP/1.1" headerFields:@{}]];
        [task didFinish];
        return;
    }
    NSDictionary* headers = @{@"Content-Type" : ns(file->contentType),
                              @"Content-Length" : [@(file->body.size()) stringValue],
                              @"Cache-Control" : @"no-cache"};
    [task didReceiveResponse:[[NSHTTPURLResponse alloc] initWithURL:url statusCode:200 HTTPVersion:@"HTTP/1.1" headerFields:headers]];
    // The bytes live in the executable for the process lifetime: no copy.
    if (!file->body.empty())
        [task didReceiveData:[NSData dataWithBytesNoCopy:const_cast<char*>(file->body.data()) length:file->body.size() freeWhenDone:NO]];
    [task didFinish];
}

/* ---- the window ---- */

NSRect HostWindowMac::initialFrame() {
    NSScreen* main = NSScreen.mainScreen ?: NSScreen.screens.firstObject;
    const CGFloat top = NSScreen.screens.firstObject.frame.size.height;  // placements count y down from here
    if (loadPlacement) {
        if (const auto p = loadPlacement(); p && p->width >= 400 && p->height >= 300) {
            const NSRect r = NSMakeRect(p->x, top - p->y - p->height, p->width, p->height);
            maximized_ = p->maximized;
            for (NSScreen* s in NSScreen.screens) {
                const NSRect overlap = NSIntersectionRect(r, s.visibleFrame);
                if (overlap.size.width >= 100 && overlap.size.height >= 50) return r;
            }
        }
    }
    const NSRect visible = main.visibleFrame;
    const CGFloat w = std::min<CGFloat>(1200, visible.size.width * 0.9), h = std::min<CGFloat>(780, visible.size.height * 0.9);
    return NSMakeRect(NSMidX(visible) - w / 2, NSMidY(visible) - h / 2, w, h);
}

WindowPlacement HostWindowMac::placement() const {
    const CGFloat top = NSScreen.screens.firstObject.frame.size.height;
    WindowPlacement p;
    p.x = static_cast<int>(normalFrame_.origin.x);
    p.y = static_cast<int>(top - normalFrame_.origin.y - normalFrame_.size.height);
    p.width = static_cast<int>(normalFrame_.size.width);
    p.height = static_cast<int>(normalFrame_.size.height);
    p.maximized = maximized_;
    return p;
}

void HostWindowMac::frameChanged() {
    layoutTitleBar();
    if (isFullscreen()) return;
    const bool zoomed = window_.isZoomed;
    if (!zoomed) normalFrame_ = window_.frame;
    if (zoomed != maximized_) {
        maximized_ = zoomed;
        if (onMaximizedChanged) onMaximizedChanged(zoomed);
    }
    if (onPlacementChanged) onPlacementChanged(placement(), false);
}

void HostWindowMac::willClose() {
    if (onPlacementChanged) onPlacementChanged(placement(), true);
    mac::setAppEvents(nullptr);
    mac::stopEventLoop(0);
}

void HostWindowMac::setTitleBarLayout(const TitleBarLayout& layout) {
    [web_ setDragAreas:layout.drag except:layout.noDrag];
    titleBarHeight_ = layout.height;
    layoutTitleBar();
}

void HostWindowMac::setMenuBar(const std::vector<MenuBarMenu>& menus) {
    mac::setMenuBar(menus, [this](const std::string& command) {
        if (onMenuCommand) onMenuCommand(command);
    });
}

void HostWindowMac::layoutTitleBar() {
    NSButton* close = [window_ standardWindowButton:NSWindowCloseButton];
    NSButton* mini = [window_ standardWindowButton:NSWindowMiniaturizeButton];
    NSButton* zoom = [window_ standardWindowButton:NSWindowZoomButton];
    if (!close || !mini || !zoom || !web_) return;
    const double pageZoom = web_.pageZoom > 0 ? web_.pageZoom : 1.0;
    ChromeInsets insets;
    if (!isFullscreen()) {  // in full screen the lights only show with the menu bar, over the page
        if (titleBarHeight_ > 0) {
            // The buttons live in the title bar container at the top of the window: make it as tall as the page's
            // bar and centre them in it, with the same margin to the left edge as above and below.
            const CGFloat height = titleBarHeight_ * pageZoom;
            NSView* container = close.superview.superview;
            NSRect frame = container.frame;
            frame.size.height = height;
            frame.origin.y = window_.frame.size.height - height;
            container.frame = frame;
            const CGFloat margin = std::max<CGFloat>(0, (height - close.frame.size.height) / 2);
            const CGFloat step = mini.frame.origin.x - close.frame.origin.x;
            NSArray<NSButton*>* buttons = @[ close, mini, zoom ];
            for (NSUInteger i = 0; i < buttons.count; ++i)
                [buttons[i] setFrameOrigin:NSMakePoint(margin + i * step, (height - buttons[i].frame.size.height) / 2)];
        }
        const NSRect c = [close convertRect:close.bounds toView:nil], z = [zoom convertRect:zoom.bounds toView:nil];
        insets.left = (NSMaxX(z) + c.origin.x) / pageZoom;  // the lights and the margin before them again after them
    }
    if (insets_ && *insets_ == insets) return;
    insets_ = insets;
    if (onChromeInsetsChanged) onChromeInsetsChanged(insets);
}

void HostWindowMac::setThemeSource(const std::string& source) {
    themeSource_ = source;
    if (!window_) return;
    if (source == "light") window_.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
    else if (source == "dark") window_.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    else window_.appearance = nil;  // the system's
}

std::string HostWindowMac::browserVersion() const {
    NSString* v = [[NSBundle bundleForClass:[WKWebView class]] objectForInfoDictionaryKey:@"CFBundleVersion"];
    return v ? "WebKit " + str(v) : std::string();
}

bool HostWindowMac::create() {
    const NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                                    NSWindowStyleMaskResizable | NSWindowStyleMaskFullSizeContentView;
    const NSRect frame = initialFrame();
    window_ = [[NSWindow alloc] initWithContentRect:frame styleMask:style backing:NSBackingStoreBuffered defer:NO];
    [window_ setFrame:frame display:NO];
    normalFrame_ = frame;
    window_.releasedWhenClosed = NO;
    window_.titlebarAppearsTransparent = YES;
    window_.titleVisibility = NSWindowTitleHidden;
    window_.tabbingMode = NSWindowTabbingModeDisallowed;
    window_.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;
    window_.contentMinSize = NSMakeSize(640, 400);
    window_.title = ns(title_);
    window_.delegate = delegate_;
    setThemeSource(themeSource_);

    WKWebViewConfiguration* config = [WKWebViewConfiguration new];
    // The UI keeps its state through the bridge; nothing of the web view's own storage needs to outlive a run.
    config.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];
    [config setURLSchemeHandler:delegate_ forURLScheme:@"sshtermaq"];
    [config.userContentController addScriptMessageHandler:delegate_ name:@"st"];
    config.preferences.javaScriptCanOpenWindowsAutomatically = NO;

    web_ = [[STWebView alloc] initWithFrame:window_.contentView.bounds configuration:config];
    web_.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    web_.navigationDelegate = delegate_;
    web_.UIDelegate = delegate_;
    web_.allowsMagnification = NO;
    web_.allowsBackForwardNavigationGestures = NO;
    web_.pageZoom = uiScale_;
    bool devtools = options_.devtools;
#ifndef NDEBUG
    devtools = true;
#endif
    if (devtools) {
        if ([web_ respondsToSelector:@selector(setInspectable:)]) [web_ setValue:@YES forKey:@"inspectable"];
        else [config.preferences setValue:@YES forKey:@"developerExtrasEnabled"];  // macOS 12 and 13.0-13.2
    }
    @try {
        [web_ setValue:@NO forKey:@"drawsBackground"];  // the window's background shows until the page paints
    } @catch (NSException*) {
    }
    window_.contentView = web_;

    if (!options_.automationSocket.empty()) {
#ifdef NDEBUG
        applog::warn("--automation is ignored: it is only available in Debug builds");
#else
        std::string error;
        automation_ = mac::Automation::start(options_.automationSocket, window_, web_, error);
        if (automation_) {
            [config.userContentController addScriptMessageHandler:delegate_ name:@"stauto"];
            [config.userContentController addUserScript:[[WKUserScript alloc] initWithSource:mac::Automation::consoleHookScript()
                                                                                injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                                                             forMainFrameOnly:YES]];
            applog::info("Automation socket: " + options_.automationSocket);
        } else {
            applog::warn("Automation socket " + options_.automationSocket + " failed: " + error);
        }
#endif
    }

    appEvents_.openRequest = [this](const std::vector<std::string>& args, const std::filesystem::path& cwd) {
        if (window_.isMiniaturized) [window_ deminiaturize:nil];
        [window_ makeKeyAndOrderFront:nil];
        if ([NSApp respondsToSelector:@selector(activate)]) [NSApp activate];
        else [NSApp activateIgnoringOtherApps:YES];
        if (onSecondInstance) onSecondInstance(args, cwd);
    };
    appEvents_.quitRequested = [this] { requestClose(); };
    mac::setAppEvents(&appEvents_);

    auto alive = alive_;
    themeObserver_ = [[NSDistributedNotificationCenter defaultCenter] addObserverForName:@"AppleInterfaceThemeChangedNotification"
                                                                                  object:nil
                                                                                   queue:[NSOperationQueue mainQueue]
                                                                              usingBlock:^(NSNotification*) {
                                                                                  if (*alive) systemThemeChanged();
                                                                              }];

    [window_ makeKeyAndOrderFront:nil];
    if (maximized_) [window_ zoom:nil];
    layoutTitleBar();

    const std::string start = options_.devUrl.empty() ? bundledUiOrigin() + "/" : options_.devUrl;
    NSURL* url = [NSURL URLWithString:ns(start)];
    if (!url) {
        applog::error("Invalid start page: " + start);
        return false;
    }
    [web_ loadRequest:[NSURLRequest requestWithURL:url]];
    return true;
}

std::unique_ptr<HostWindow> createHostWindow(HostWindowOptions options) {
    return std::make_unique<HostWindowMac>(std::move(options));
}

std::string bundledUiOrigin() { return "sshtermaq://app"; }

bool systemPrefersDark() {
    NSAppearance* appearance = NSApp ? NSApp.effectiveAppearance : [NSAppearance currentDrawingAppearance];
    return [[appearance bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]]
        isEqualToString:NSAppearanceNameDarkAqua];
}

}  // namespace st::ui
