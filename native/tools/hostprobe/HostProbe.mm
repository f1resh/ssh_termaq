// Host probe for the macOS port's step B3 (not part of the app): an AppKit window with a WKWebView that serves pages
// through the sshtermaq:// scheme and measures what the macOS host window has to decide - how host -> page messages
// travel (evaluateJavaScript, callAsyncJavaScript, a page-driven pull, a streamed fetch), how the page copes with a
// flood of terminal output, whether messages flow while a sheet or a modal panel is open, and what the page sees
// (secure context, WebGL, page zoom, window chrome).
//
// Build: native/tools/hostprobe/build.sh. Run:
//   HostProbe.app/Contents/MacOS/HostProbe [--root DIR] [--page PAGE] [--out FILE] [--snapshot FILE.png]
//                                          [--quit-after SECONDS] [--keep]
//   --root  folder served at sshtermaq://app/ (default: this tool's folder); /node_modules/ maps to ui/node_modules
//   --page  path and query loaded first (default: probe.html)
// The page drives the measurements (probe.html) and posts a report, printed as JSON (and written to --out).
#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::string root;
    std::string nodeModules;
    std::string page = "probe.html";
    std::string out;
    std::string snapshot;
    double quitAfter = 0;
    bool keep = false;
};
Options g_opt;

double nowMs() { return [[NSDate date] timeIntervalSince1970] * 1000.0; }

/* ---- realistic terminal output, as JSON messages of the bridge ---- */

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    out += b;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

/** About `size` bytes of `ls -l`-like output with colours and some non-ASCII names, cut at a line end. */
std::string terminalText(size_t size, int seed) {
    std::string text;
    for (int i = 0; text.size() < size; ++i) {
        char line[256];
        const int n = seed * 100000 + i;
        const bool dir = n % 5 == 0;
        std::snprintf(line, sizeof line, "%s  %2d user  staff  %8d Oct 18 12:%02d %s%s_%06d%s%s\r\n",
                      dir ? "drwxr-xr-x" : "-rw-r--r--", 1 + n % 9, (n * 7919) % 99999999, n % 60,
                      dir ? "\x1b[01;34m" : "", n % 11 == 0 ? "\xc3\xbc" "ber_\xe2\x9c\x93" : "file", n,
                      dir ? "\x1b[0m" : "", n % 13 == 0 ? "  \xe2\x86\x92 link" : "");
        text += line;
    }
    return text;
}

/* ---- host -> page queue ---- */

enum class Mode { Eval, Call, Pull, Stream };
enum class Dispatch { Source, Gcd };

struct Outbox {
    std::mutex m;
    std::deque<std::string> q;
    size_t bytes = 0;

    void push(std::string msg) {
        std::lock_guard lock(m);
        bytes += msg.size();
        q.push_back(std::move(msg));
    }
    /** Up to `maxBytes` (at least one message) joined with `sep`; "" when empty. */
    std::string take(size_t maxBytes, const char* open, const char* sep, const char* close, size_t* count = nullptr) {
        std::lock_guard lock(m);
        if (q.empty()) return {};
        std::string out = open;
        size_t n = 0;
        while (!q.empty() && (n == 0 || out.size() + q.front().size() <= maxBytes)) {
            if (n) out += sep;
            out += q.front();
            bytes -= q.front().size();
            q.pop_front();
            ++n;
        }
        out += close;
        if (count) *count = n;
        return out;
    }
    bool empty() {
        std::lock_guard lock(m);
        return q.empty();
    }
};

NSString* ns(const std::string& s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding];
}

}  // namespace

@interface Probe : NSObject <WKURLSchemeHandler, WKScriptMessageHandler, WKScriptMessageHandlerWithReply,
                             WKNavigationDelegate, NSApplicationDelegate>
@end

@implementation Probe {
    NSWindow* _window;
    WKWebView* _web;
    Outbox _outbox;
    Mode _mode;
    Dispatch _dispatch;
    size_t _batchMax;
    int _maxInFlight;
    int _inFlight;
    std::atomic<bool> _gcdScheduled;
    CFRunLoopSourceRef _source;
    void (^_pendingReply)(id, NSString*);
    id<WKURLSchemeTask> _eventsTask;
    std::thread _producer;
    std::vector<std::string> _messages;  // precomputed conn:onData messages
    NSMutableArray* _sheetTimes;
    NSMutableArray* _errors;
    NSMutableArray* _runs;  // results the page posted so far (kept if a later run hangs)
}

static void performSource(void* info) { [(__bridge Probe*)info flush]; }

- (instancetype)init {
    if ((self = [super init])) {
        _mode = Mode::Call;
        _dispatch = Dispatch::Source;
        _batchMax = 1 << 20;
        _maxInFlight = 4;
        _sheetTimes = [NSMutableArray array];
        _errors = [NSMutableArray array];
        _runs = [NSMutableArray array];
        CFRunLoopSourceContext ctx{};
        ctx.info = (__bridge void*)self;
        ctx.perform = performSource;
        _source = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &ctx);
        // Common modes: the source also runs while AppKit tracks the mouse or runs a modal panel.
        CFRunLoopAddSource(CFRunLoopGetMain(), _source, kCFRunLoopCommonModes);
    }
    return self;
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
    [self buildMenu];
    [self buildWindow];
    [NSApp activateIgnoringOtherApps:YES];
    if (g_opt.quitAfter > 0) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(g_opt.quitAfter * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            [self finishWithReport:@{@"error" : @"timed out"}];
        });
    }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app { return YES; }

- (void)buildMenu {
    NSMenu* bar = [NSMenu new];
    NSMenuItem* appItem = [NSMenuItem new];
    [bar addItem:appItem];
    NSMenu* appMenu = [NSMenu new];
    [appMenu addItemWithTitle:@"Quit HostProbe" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    NSMenuItem* editItem = [NSMenuItem new];
    [bar addItem:editItem];
    NSMenu* edit = [[NSMenu alloc] initWithTitle:@"Edit"];
    [edit addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [edit addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [edit addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
    editItem.submenu = edit;
    NSApp.mainMenu = bar;
}

- (void)buildWindow {
    const NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                                    NSWindowStyleMaskResizable | NSWindowStyleMaskFullSizeContentView;
    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1100, 720) styleMask:style backing:NSBackingStoreBuffered defer:NO];
    _window.titlebarAppearsTransparent = YES;
    _window.titleVisibility = NSWindowTitleHidden;
    _window.tabbingMode = NSWindowTabbingModeDisallowed;
    _window.releasedWhenClosed = NO;
    [_window center];

    WKWebViewConfiguration* cfg = [WKWebViewConfiguration new];
    cfg.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];
    [cfg setURLSchemeHandler:self forURLScheme:@"sshtermaq"];
    WKUserContentController* ucc = cfg.userContentController;
    [ucc addScriptMessageHandler:self name:@"st"];
    [ucc addScriptMessageHandlerWithReply:self contentWorld:WKContentWorld.pageWorld name:@"stpull"];
    // Page errors and console.error reach the report (the real UI has no other way to tell us).
    NSString* hook =
        @"(()=>{const post=(k,m)=>{try{window.webkit.messageHandlers.st.postMessage(JSON.stringify({cmd:'log',kind:k,text:String(m)}))}catch(e){}};"
         "addEventListener('error',e=>post('error',(e.message||'')+' @'+(e.filename||'')+':'+(e.lineno||'')));"
         "addEventListener('unhandledrejection',e=>post('rejection',e.reason&&e.reason.stack||e.reason));"
         "const ce=console.error.bind(console);console.error=(...a)=>{post('console',a.map(String).join(' '));ce(...a)};})()";
    [ucc addUserScript:[[WKUserScript alloc] initWithSource:hook injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES]];

    _web = [[WKWebView alloc] initWithFrame:_window.contentView.bounds configuration:cfg];
    _web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _web.navigationDelegate = self;
    if ([_web respondsToSelector:@selector(setInspectable:)]) [_web setValue:@YES forKey:@"inspectable"];
    [_window.contentView addSubview:_web];
    [_window makeKeyAndOrderFront:nil];

    NSString* url = [@"sshtermaq://app/" stringByAppendingString:ns(g_opt.page)];
    [_web loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:url]]];
}

/* ---- sshtermaq:// ---- */

static NSString* mimeType(NSString* ext) {
    NSDictionary* types = @{@"html" : @"text/html", @"js" : @"text/javascript", @"mjs" : @"text/javascript", @"css" : @"text/css",
                            @"json" : @"application/json", @"svg" : @"image/svg+xml", @"png" : @"image/png", @"ico" : @"image/x-icon",
                            @"woff2" : @"font/woff2", @"woff" : @"font/woff", @"ttf" : @"font/ttf", @"map" : @"application/json",
                            @"wasm" : @"application/wasm"};
    return types[ext.lowercaseString] ?: @"application/octet-stream";
}

- (void)webView:(WKWebView*)webView startURLSchemeTask:(id<WKURLSchemeTask>)task {
    NSURL* url = task.request.URL;
    NSString* path = url.path.length ? url.path : @"/";
    if ([path isEqualToString:@"/__events"]) {
        NSHTTPURLResponse* r = [[NSHTTPURLResponse alloc] initWithURL:url statusCode:200 HTTPVersion:@"HTTP/1.1"
                                                         headerFields:@{@"Content-Type" : @"application/x-ndjson", @"Cache-Control" : @"no-store"}];
        [task didReceiveResponse:r];
        _eventsTask = task;
        [self flush];
        return;
    }
    if ([path hasSuffix:@"/"]) path = [path stringByAppendingString:@"index.html"];
    NSString* file;
    if ([path hasPrefix:@"/node_modules/"] && !g_opt.nodeModules.empty())
        file = [ns(g_opt.nodeModules) stringByAppendingPathComponent:[path substringFromIndex:14]];
    else
        file = [ns(g_opt.root) stringByAppendingPathComponent:path];
    NSData* data = [path containsString:@".."] ? nil : [NSData dataWithContentsOfFile:file];
    if (!data) {
        NSHTTPURLResponse* r = [[NSHTTPURLResponse alloc] initWithURL:url statusCode:404 HTTPVersion:@"HTTP/1.1" headerFields:@{}];
        [task didReceiveResponse:r];
        [task didFinish];
        [_errors addObject:[@"404 " stringByAppendingString:path]];
        return;
    }
    NSHTTPURLResponse* r = [[NSHTTPURLResponse alloc] initWithURL:url statusCode:200 HTTPVersion:@"HTTP/1.1"
                                                     headerFields:@{@"Content-Type" : mimeType(path.pathExtension),
                                                                    @"Content-Length" : [@(data.length) stringValue]}];
    [task didReceiveResponse:r];
    [task didReceiveData:data];
    [task didFinish];
}

- (void)webView:(WKWebView*)webView stopURLSchemeTask:(id<WKURLSchemeTask>)task {
    if (task == _eventsTask) _eventsTask = nil;
}

/* ---- page -> host ---- */

- (void)userContentController:(WKUserContentController*)ucc didReceiveScriptMessage:(WKScriptMessage*)message {
    NSDictionary* cmd = [NSJSONSerialization JSONObjectWithData:[message.body dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
    NSString* name = cmd[@"cmd"];
    if ([name isEqualToString:@"log"]) {
        [_errors addObject:[NSString stringWithFormat:@"%@: %@", cmd[@"kind"], cmd[@"text"]]];
    } else if ([name isEqualToString:@"ping"]) {
        // Behind whatever output is queued, as a real reply would be.
        _outbox.push("{\"t\":\"res\",\"id\":" + std::to_string([cmd[@"id"] longLongValue]) + ",\"ok\":true,\"result\":null}");
        [self signal];
    } else if ([name isEqualToString:@"run"]) {
        [_runs addObject:cmd[@"result"]];
        fprintf(stderr, "run %s\n", [[NSString stringWithFormat:@"%@", cmd[@"summary"]] UTF8String]);
    } else if ([name isEqualToString:@"flood"]) {
        [self startFlood:cmd];
    } else if ([name isEqualToString:@"zoom"]) {
        _web.pageZoom = [cmd[@"pageZoom"] doubleValue];
        if (cmd[@"magnification"]) [_web setMagnification:[cmd[@"magnification"] doubleValue] centeredAtPoint:NSZeroPoint];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            [self->_web evaluateJavaScript:@"window.__zoomApplied && __zoomApplied()" completionHandler:nil];
        });
    } else if ([name isEqualToString:@"panel"]) {
        [self openPanel:cmd];
    } else if ([name isEqualToString:@"chrome"]) {
        [self reportChrome];
    } else if ([name isEqualToString:@"done"]) {
        [self finishWithReport:cmd[@"report"]];
    }
}

- (void)userContentController:(WKUserContentController*)ucc
      didReceiveScriptMessage:(WKScriptMessage*)message
                 replyHandler:(void (^)(id, NSString*))replyHandler {
    if (_pendingReply) _pendingReply(@"[]", nil);  // every request gets an answer, or WebKit rejects it
    _pendingReply = replyHandler;  // answered by flush() once something is queued
    [self flush];
}

/* ---- the flood ---- */

- (void)startFlood:(NSDictionary*)cmd {
    NSString* mode = cmd[@"mode"];
    _mode = [mode isEqualToString:@"eval"] ? Mode::Eval : [mode isEqualToString:@"pull"] ? Mode::Pull
                                         : [mode isEqualToString:@"stream"]          ? Mode::Stream
                                                                                     : Mode::Call;
    if (_pendingReply && _mode != Mode::Pull) {  // a pull loop's last request, from an earlier run
        auto reply = _pendingReply;
        _pendingReply = nil;
        reply(@"[]", nil);
    }
    _dispatch = [cmd[@"dispatch"] isEqualToString:@"gcd"] ? Dispatch::Gcd : Dispatch::Source;
    _batchMax = cmd[@"batchMax"] ? [cmd[@"batchMax"] unsignedLongValue] : (1u << 20);
    _maxInFlight = cmd[@"inFlight"] ? [cmd[@"inFlight"] intValue] : 4;
    const size_t total = [cmd[@"bytes"] unsignedLongValue];
    const size_t chunk = [cmd[@"chunk"] unsignedLongValue];
    const double rateMBs = [cmd[@"rateMBs"] doubleValue];  // 0: as fast as possible
    if (_messages.empty() || _messages[0].size() < chunk) {
        _messages.clear();
        for (int i = 0; i < 16; ++i)
            _messages.push_back("{\"t\":\"evt\",\"ch\":\"conn:onData\",\"args\":[\"pane#1\",\"" + jsonEscape(terminalText(chunk, i)) + "\"]}");
    }
    if (_producer.joinable()) _producer.join();
    const size_t count = (total + chunk - 1) / chunk;
    _producer = std::thread([self, count, rateMBs, chunk] {
        const auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < count; ++i) {
            if (rateMBs > 0) {
                const auto due = start + std::chrono::duration<double>(static_cast<double>(i * chunk) / (rateMBs * 1e6));
                std::this_thread::sleep_until(due);
            }
            self->_outbox.push(self->_messages[i % self->_messages.size()]);
            [self signal];
        }
    });
}

/** Asks the main thread to flush, the way the app's dispatcher would. */
- (void)signal {
    if (_dispatch == Dispatch::Source) {
        CFRunLoopSourceSignal(_source);
        CFRunLoopWakeUp(CFRunLoopGetMain());
    } else if (!_gcdScheduled.exchange(true)) {
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_gcdScheduled = false;
            [self flush];
        });
    }
}

- (void)flush {
    switch (_mode) {
        case Mode::Eval:
        case Mode::Call:
            while (_inFlight < _maxInFlight) {
                std::string batch = _outbox.take(_batchMax, "[", ",", "]");
                if (batch.empty()) break;
                ++_inFlight;
                void (^done)(id, NSError*) = ^(id result, NSError* error) {
                    if (error) [self->_errors addObject:error.localizedDescription];
                    --self->_inFlight;
                    [self flush];
                };
                if (_mode == Mode::Eval)
                    [_web evaluateJavaScript:ns("__stDeliver(" + batch + ")") completionHandler:done];
                else
                    [_web callAsyncJavaScript:@"__stDeliverText(b)" arguments:@{@"b" : ns(batch)} inFrame:nil
                               inContentWorld:WKContentWorld.pageWorld completionHandler:done];
            }
            break;
        case Mode::Pull:
            if (_pendingReply && !_outbox.empty()) {
                auto reply = _pendingReply;
                _pendingReply = nil;
                reply(ns(_outbox.take(_batchMax, "[", ",", "]")), nil);
            }
            break;
        case Mode::Stream:
            if (_eventsTask) {
                std::string lines = _outbox.take(_batchMax, "", "\n", "\n");
                if (!lines.empty()) [_eventsTask didReceiveData:[NSData dataWithBytes:lines.data() length:lines.size()]];
            }
            break;
    }
}

/* ---- panels ---- */

- (void)openPanel:(NSDictionary*)cmd {
    const double seconds = [cmd[@"seconds"] doubleValue];
    NSString* kind = cmd[@"kind"];
    // Outside the message callback: a modal loop must not run inside WebKit's IPC dispatch.
    dispatch_async(dispatch_get_main_queue(), ^{
        [self->_sheetTimes addObject:@{@"kind" : kind, @"event" : @"open", @"t" : @(nowMs())}];
        if ([kind isEqualToString:@"modal"]) {
            NSAlert* alert = [NSAlert new];
            alert.messageText = @"HostProbe: modal panel";
            NSTimer* t = [NSTimer timerWithTimeInterval:seconds repeats:NO block:^(NSTimer*) { [NSApp abortModal]; }];
            [[NSRunLoop currentRunLoop] addTimer:t forMode:NSModalPanelRunLoopMode];
            [alert runModal];
            [self->_sheetTimes addObject:@{@"kind" : kind, @"event" : @"close", @"t" : @(nowMs())}];
        } else {
            NSOpenPanel* panel = [NSOpenPanel openPanel];
            [panel beginSheetModalForWindow:self->_window completionHandler:^(NSModalResponse) {
                [self->_sheetTimes addObject:@{@"kind" : kind, @"event" : @"close", @"t" : @(nowMs())}];
            }];
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                [panel cancel:nil];
            });
        }
    });
}

/** Where the traffic lights are, in the web view's CSS pixels (the custom title bar leaves room for them). */
- (void)reportChrome {
    NSButton* close = [_window standardWindowButton:NSWindowCloseButton];
    NSButton* zoom = [_window standardWindowButton:NSWindowZoomButton];
    const NSRect c = [close convertRect:close.bounds toView:nil], z = [zoom convertRect:zoom.bounds toView:nil];
    const CGFloat h = _window.contentView.frame.size.height;
    NSString* js = [NSString stringWithFormat:@"window.__chrome && __chrome({left:%.1f,right:%.1f,top:%.1f,bottom:%.1f,titlebar:%.1f,backing:%.1f})",
                    c.origin.x, z.origin.x + z.size.width, h - (c.origin.y + c.size.height), h - c.origin.y,
                    h - _window.contentLayoutRect.size.height, _window.backingScaleFactor];
    [_web evaluateJavaScript:js completionHandler:nil];
}

/* ---- the end ---- */

- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
    if (g_opt.page.rfind("probe.html", 0) == 0) return;  // the probe page reports itself
    // Another page (the real UI): give it time to start, then report what it logged.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        [self->_web evaluateJavaScript:@"JSON.stringify({title:document.title,secure:isSecureContext,body:document.body.innerText.slice(0,300)})"
                     completionHandler:^(id result, NSError* error) {
                         [self finishWithReport:@{@"page" : result ?: [NSNull null]}];
                     }];
    });
}

- (void)webView:(WKWebView*)webView didFailProvisionalNavigation:(WKNavigation*)navigation withError:(NSError*)error {
    [_errors addObject:error.localizedDescription];
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView*)webView {
    [_errors addObject:@"web content process terminated"];
}

- (void)finishWithReport:(id)report {
    static bool finished = false;
    if (finished) return;
    finished = true;
    void (^write)(void) = ^{
        NSMutableDictionary* all = [NSMutableDictionary dictionaryWithDictionary:@{@"report" : report ?: [NSNull null],
                                                                                   @"panels" : self->_sheetTimes,
                                                                                   @"runs" : self->_runs,
                                                                                   @"errors" : self->_errors}];
        all[@"macOS"] = [[NSProcessInfo processInfo] operatingSystemVersionString];
        NSData* json = [NSJSONSerialization dataWithJSONObject:all options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:nil];
        fwrite(json.bytes, 1, json.length, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        if (!g_opt.out.empty()) [json writeToFile:ns(g_opt.out) atomically:YES];
        if (!g_opt.keep) [NSApp terminate:nil];
    };
    if (g_opt.snapshot.empty()) return write();
    [_web takeSnapshotWithConfiguration:nil completionHandler:^(NSImage* image, NSError* error) {
        if (image) {
            NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:[image CGImageForProposedRect:nil context:nil hints:nil]];
            [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:ns(g_opt.snapshot) atomically:YES];
        }
        write();
    }];
}

@end

int main(int argc, char** argv) {
    @autoreleasepool {
        NSString* exe = [[NSBundle mainBundle] executablePath];
        // HostProbe.app/Contents/MacOS/HostProbe -> the tool's folder is recorded at build time.
        g_opt.root = HOSTPROBE_DIR;
        g_opt.nodeModules = std::string(HOSTPROBE_DIR) + "/../../../ui/node_modules";
        (void)exe;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
            if (a == "--root") g_opt.root = next();
            else if (a == "--page") g_opt.page = next();
            else if (a == "--out") g_opt.out = next();
            else if (a == "--snapshot") g_opt.snapshot = next();
            else if (a == "--quit-after") g_opt.quitAfter = std::stod(next());
            else if (a == "--keep") g_opt.keep = true;
        }
        NSApplication* app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        Probe* probe = [Probe new];
        app.delegate = probe;
        [app run];
    }
    return 0;
}
