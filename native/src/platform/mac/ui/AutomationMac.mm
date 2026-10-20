// The Debug builds' automation socket (AutomationMac.h): a thread accepts one client at a time and reads its lines;
// every request runs on the main thread, which also writes the answers and console events.
#include "platform/mac/ui/AutomationMac.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <thread>

namespace st::ui::mac {

namespace {

NSString* ns(const std::string& s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @"";
}

/** US-layout virtual key codes (HIToolbox's kVK_ANSI_*), and the key a shifted character is typed with. */
struct KeyCode {
    unichar ch;
    unsigned short code;
};
constexpr KeyCode kKeyCodes[] = {
    {'a', 0},   {'s', 1},   {'d', 2},   {'f', 3},   {'h', 4},   {'g', 5},   {'z', 6},   {'x', 7},  {'c', 8},
    {'v', 9},   {'b', 11},  {'q', 12},  {'w', 13},  {'e', 14},  {'r', 15},  {'y', 16},  {'t', 17}, {'1', 18},
    {'2', 19},  {'3', 20},  {'4', 21},  {'6', 22},  {'5', 23},  {'=', 24},  {'9', 25},  {'7', 26}, {'-', 27},
    {'8', 28},  {'0', 29},  {']', 30},  {'o', 31},  {'u', 32},  {'[', 33},  {'i', 34},  {'p', 35}, {'l', 37},
    {'j', 38},  {'\'', 39}, {'k', 40},  {';', 41},  {'\\', 42}, {',', 43},  {'/', 44},  {'n', 45}, {'m', 46},
    {'.', 47},  {'`', 50},  {' ', 49},
};
constexpr std::pair<unichar, unichar> kShifted[] = {
    {'!', '1'}, {'@', '2'}, {'#', '3'}, {'$', '4'}, {'%', '5'}, {'^', '6'}, {'&', '7'}, {'*', '8'}, {'(', '9'},
    {')', '0'}, {'_', '-'}, {'+', '='}, {'{', '['}, {'}', ']'}, {'|', '\\'}, {':', ';'}, {'"', '\''}, {'<', ','},
    {'>', '.'}, {'?', '/'}, {'~', '`'},
};

/** Playwright key names: virtual key code and the character AppKit reports. */
struct NamedKey {
    const char* name;
    unsigned short code;
    unichar ch;
};
constexpr NamedKey kNamed[] = {
    {"Enter", 36, '\r'},
    {"Tab", 48, '\t'},
    {"Space", 49, ' '},
    {"Backspace", 51, 0x7f},
    {"Escape", 53, 0x1b},
    {"Delete", 117, NSDeleteFunctionKey},
    {"Home", 115, NSHomeFunctionKey},
    {"End", 119, NSEndFunctionKey},
    {"PageUp", 116, NSPageUpFunctionKey},
    {"PageDown", 121, NSPageDownFunctionKey},
    {"ArrowLeft", 123, NSLeftArrowFunctionKey},
    {"ArrowRight", 124, NSRightArrowFunctionKey},
    {"ArrowDown", 125, NSDownArrowFunctionKey},
    {"ArrowUp", 126, NSUpArrowFunctionKey},
    {"F1", 122, NSF1FunctionKey},
    {"F2", 120, NSF2FunctionKey},
    {"F3", 99, NSF3FunctionKey},
    {"F4", 118, NSF4FunctionKey},
    {"F5", 96, NSF5FunctionKey},
    {"F6", 97, NSF6FunctionKey},
    {"F7", 98, NSF7FunctionKey},
    {"F8", 100, NSF8FunctionKey},
    {"F9", 101, NSF9FunctionKey},
    {"F10", 109, NSF10FunctionKey},
    {"F11", 103, NSF11FunctionKey},
    {"F12", 111, NSF12FunctionKey},
};

/** The key (unshifted character, code) a character is typed with on a US layout; code 0xffff when there is none. */
std::pair<unichar, unsigned short> keyFor(unichar c, bool& shift) {
    shift = false;
    unichar base = c;
    if (c >= 'A' && c <= 'Z') {
        base = static_cast<unichar>(c - 'A' + 'a');
        shift = true;
    }
    for (const auto& [shifted, plain] : kShifted)
        if (c == shifted) {
            base = plain;
            shift = true;
        }
    for (const auto& k : kKeyCodes)
        if (k.ch == base) return {base, k.code};
    return {c, 0xffff};
}

}  // namespace

struct Automation::Impl : std::enable_shared_from_this<Impl> {
    NSWindow* window = nil;
    WKWebView* web = nil;
    std::string path;
    int listenFd = -1;
    std::mutex clientMutex;
    int clientFd = -1;
    std::atomic<bool> stop{false};
    std::thread thread;

    ~Impl() {
        stop = true;
        if (thread.joinable()) thread.join();
        if (clientFd >= 0) ::close(clientFd);
        if (listenFd >= 0) ::close(listenFd);
        ::unlink(path.c_str());
    }

    /* ---- the socket thread ---- */

    void run() {
        std::string pending;
        while (!stop) {
            int client;
            {
                std::lock_guard lock(clientMutex);
                client = clientFd;
            }
            pollfd fds[2] = {{listenFd, POLLIN, 0}, {client, POLLIN, 0}};
            if (::poll(fds, client >= 0 ? 2 : 1, 250) <= 0) continue;
            if (fds[0].revents & POLLIN) {
                const int c = ::accept(listenFd, nullptr, nullptr);
                if (c >= 0) {
                    ::fcntl(c, F_SETFD, FD_CLOEXEC);
                    const int one = 1;
                    ::setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
                    std::lock_guard lock(clientMutex);
                    if (clientFd >= 0) ::close(clientFd);
                    clientFd = c;
                    pending.clear();
                }
                continue;
            }
            if (client < 0 || !(fds[1].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            char buf[65536];
            const ssize_t n = ::read(client, buf, sizeof buf);
            if (n <= 0) {
                std::lock_guard lock(clientMutex);
                if (clientFd == client) {
                    ::close(clientFd);
                    clientFd = -1;
                }
                continue;
            }
            pending.append(buf, static_cast<size_t>(n));
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                std::weak_ptr<Impl> weak = weak_from_this();
                // Common modes: requests also run while a menu is tracked or a modal panel is up.
                CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
                    if (auto self = weak.lock()) self->handle(line);
                });
                CFRunLoopWakeUp(CFRunLoopGetMain());
            }
        }
    }

    /* ---- the main thread ---- */

    void send(NSDictionary* message) {
        NSData* json = [NSJSONSerialization dataWithJSONObject:message options:0 error:nil];
        if (!json) return;
        std::string line(static_cast<const char*>(json.bytes), json.length);
        line += '\n';
        std::lock_guard lock(clientMutex);
        size_t sent = 0;
        while (clientFd >= 0 && sent < line.size()) {
            const ssize_t n = ::write(clientFd, line.data() + sent, line.size() - sent);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            sent += static_cast<size_t>(n);
        }
    }

    void reply(id requestId, id result) { send(@{@"id" : requestId, @"ok" : @YES, @"result" : result ?: [NSNull null]}); }
    void fail(id requestId, NSString* error) { send(@{@"id" : requestId, @"ok" : @NO, @"error" : error ?: @"failed"}); }

    void handle(const std::string& line) {
        NSDictionary* req = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:line.data() length:line.size()]
                                                            options:0
                                                              error:nil];
        if (![req isKindOfClass:[NSDictionary class]]) return;
        id requestId = req[@"id"] ?: [NSNull null];
        NSString* cmd = req[@"cmd"];
        if ([cmd isEqualToString:@"eval"]) return evaluate(requestId, req[@"fn"], req[@"arg"]);
        if ([cmd isEqualToString:@"screenshot"]) return screenshot(requestId, req[@"path"]);
        if ([cmd isEqualToString:@"type"]) {
            typeText(req[@"text"]);
            return reply(requestId, nil);
        }
        if ([cmd isEqualToString:@"press"]) {
            if (!pressKey(req[@"key"])) return fail(requestId, [@"unknown key " stringByAppendingString:req[@"key"] ?: @""]);
            return reply(requestId, nil);
        }
        if ([cmd isEqualToString:@"click"]) {
            click([req[@"x"] doubleValue], [req[@"y"] doubleValue]);
            return reply(requestId, nil);
        }
        if ([cmd isEqualToString:@"sheet"]) return sheet(requestId, req[@"press"], req[@"check"]);
        fail(requestId, [@"unknown command " stringByAppendingString:cmd ?: @"(none)"]);
    }

    void evaluate(id requestId, NSString* fn, NSString* arg) {
        if (![fn isKindOfClass:[NSString class]]) return fail(requestId, @"eval needs fn");
        NSString* body = [NSString stringWithFormat:@"const f = (%@);\nconst r = await f(JSON.parse(arg));\n"
                                                    @"return JSON.stringify(r === undefined ? null : r);",
                                                    fn];
        std::weak_ptr<Impl> weak = weak_from_this();
        [web callAsyncJavaScript:body
                       arguments:@{@"arg" : [arg isKindOfClass:[NSString class]] ? arg : @"null"}
                         inFrame:nil
                  inContentWorld:WKContentWorld.pageWorld
               completionHandler:^(id result, NSError* error) {
                   auto self = weak.lock();
                   if (!self) return;
                   if (error) return self->fail(requestId, error.userInfo[@"WKJavaScriptExceptionMessage"] ?: error.localizedDescription);
                   id value = [NSNull null];
                   if ([result isKindOfClass:[NSString class]])
                       value = [NSJSONSerialization JSONObjectWithData:[result dataUsingEncoding:NSUTF8StringEncoding]
                                                               options:NSJSONReadingFragmentsAllowed
                                                                 error:nil]
                                   ?: [NSNull null];
                   self->reply(requestId, value);
               }];
    }

    void screenshot(id requestId, NSString* file) {
        if (![file isKindOfClass:[NSString class]]) return fail(requestId, @"screenshot needs path");
        std::weak_ptr<Impl> weak = weak_from_this();
        [web takeSnapshotWithConfiguration:nil completionHandler:^(NSImage* image, NSError* error) {
            auto self = weak.lock();
            if (!self) return;
            CGImageRef cg = image ? [image CGImageForProposedRect:nil context:nil hints:nil] : nil;
            NSData* png = cg ? [[[NSBitmapImageRep alloc] initWithCGImage:cg] representationUsingType:NSBitmapImageFileTypePNG properties:@{}] : nil;
            if (!png || ![png writeToFile:file atomically:YES])
                return self->fail(requestId, error ? error.localizedDescription : @"cannot write the screenshot");
            self->reply(requestId, file);
        }];
    }

    /** One key down and up, routed as AppKit routes the user's: Command keys first to the key equivalents. */
    void postKey(NSString* chars, NSString* plain, unsigned short code, NSEventModifierFlags mods) {
        const NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
        NSEvent* down = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:mods timestamp:now
                                     windowNumber:window.windowNumber context:nil characters:chars
                      charactersIgnoringModifiers:plain isARepeat:NO keyCode:code];
        NSEvent* up = [NSEvent keyEventWithType:NSEventTypeKeyUp location:NSZeroPoint modifierFlags:mods timestamp:now
                                   windowNumber:window.windowNumber context:nil characters:chars
                    charactersIgnoringModifiers:plain isARepeat:NO keyCode:code];
        const bool equivalent = (mods & NSEventModifierFlagCommand) &&
                                ([window performKeyEquivalent:down] || [NSApp.mainMenu performKeyEquivalent:down]);
        if (!equivalent) [window sendEvent:down];
        [window sendEvent:up];
    }

    void typeText(NSString* text) {
        if (![text isKindOfClass:[NSString class]]) return;
        [text enumerateSubstringsInRange:NSMakeRange(0, text.length)
                                 options:NSStringEnumerationByComposedCharacterSequences
                              usingBlock:^(NSString* piece, NSRange, NSRange, BOOL*) {
                                  bool shift = false;
                                  const auto [base, code] = piece.length == 1 ? keyFor([piece characterAtIndex:0], shift)
                                                                              : std::pair<unichar, unsigned short>{0, 0xffff};
                                  NSString* plain = code == 0xffff ? piece : [NSString stringWithCharacters:&base length:1];
                                  postKey(piece, plain, code == 0xffff ? 0 : code, shift ? NSEventModifierFlagShift : 0);
                              }];
    }

    /** "Meta+Shift+T", "Control+C", "Enter", "ArrowUp" (Playwright's names; Command / Cmd / Option work too). */
    bool pressKey(NSString* spec) {
        if (![spec isKindOfClass:[NSString class]] || !spec.length) return false;
        NSArray<NSString*>* parts = [spec componentsSeparatedByString:@"+"];
        NSString* key = parts.lastObject.length ? parts.lastObject : @"+";
        NSEventModifierFlags mods = 0;
        for (NSUInteger i = 0; i + 1 < parts.count; ++i) {
            NSString* m = parts[i].lowercaseString;
            if ([m isEqualToString:@"meta"] || [m isEqualToString:@"command"] || [m isEqualToString:@"cmd"]) mods |= NSEventModifierFlagCommand;
            else if ([m isEqualToString:@"control"] || [m isEqualToString:@"ctrl"]) mods |= NSEventModifierFlagControl;
            else if ([m isEqualToString:@"alt"] || [m isEqualToString:@"option"]) mods |= NSEventModifierFlagOption;
            else if ([m isEqualToString:@"shift"]) mods |= NSEventModifierFlagShift;
            else return false;
        }
        for (const auto& k : kNamed) {
            if ([key isEqualToString:@(k.name)]) {
                NSString* c = [NSString stringWithCharacters:&k.ch length:1];
                postKey(c, c, k.code, mods);
                return true;
            }
        }
        if (key.length != 1) return false;
        bool shift = false;
        const unichar ch = [key characterAtIndex:0];
        auto [base, code] = keyFor(ch, shift);
        // "A" types A, but in a shortcut a letter only names its key ("Meta+T" is Command+T): Shift when it is written
        // out, or when a punctuation character needs it ("Meta+?").
        const bool letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        if (shift && !(letter && mods)) mods |= NSEventModifierFlagShift;
        unichar typed = [key characterAtIndex:0];
        if ((mods & NSEventModifierFlagShift) && base >= 'a' && base <= 'z') typed = static_cast<unichar>(base - 'a' + 'A');
        if ((mods & NSEventModifierFlagControl) && base >= 'a' && base <= 'z') typed = static_cast<unichar>(base - 'a' + 1);
        postKey([NSString stringWithCharacters:&typed length:1], [NSString stringWithCharacters:&base length:1],
                code == 0xffff ? 0 : code, mods);
        return true;
    }

    static void collect(NSView* view, NSMutableArray<NSString*>* texts, NSMutableArray<NSButton*>* buttons) {
        for (NSView* sub in view.subviews) {
            if (sub.hidden) continue;
            if ([sub isKindOfClass:[NSButton class]]) [buttons addObject:(NSButton*)sub];
            else if ([sub isKindOfClass:[NSTextField class]] && ((NSTextField*)sub).stringValue.length)
                [texts addObject:((NSTextField*)sub).stringValue];
            collect(sub, texts, buttons);
        }
    }

    /** The sheet on the window (an alert, a file panel): its texts and buttons, null when there is none. `check` turns
     *  a checkbox on, then `press` presses a button (both by title). */
    void sheet(id requestId, NSString* press, NSString* check) {
        NSWindow* s = window.attachedSheet;
        if (!s) return reply(requestId, nil);
        if ([s isKindOfClass:[NSSavePanel class]]) {  // its controls live in another process: its own words only
            NSSavePanel* panel = (NSSavePanel*)s;
            NSString* ok = panel.prompt.length ? panel.prompt : @"OK";
            NSDictionary* result = @{
                @"texts" : @[ panel.message ?: @"", panel.title ?: @"" ],
                @"buttons" : @[ @{@"title" : ok, @"on" : @NO}, @{@"title" : @"Cancel", @"on" : @NO} ]
            };
            if ([press isEqual:@"Cancel"]) [panel cancel:nil];
            else if ([press isEqual:ok]) [panel ok:nil];
            else if ([press isKindOfClass:[NSString class]]) return fail(requestId, [@"no button " stringByAppendingString:press]);
            return reply(requestId, result);
        }
        NSMutableArray<NSString*>* texts = [NSMutableArray array];
        NSMutableArray<NSButton*>* all = [NSMutableArray array];
        collect(s.contentView, texts, all);
        auto find = [&](NSString* title) -> NSButton* {
            for (NSButton* b in all)
                if ([b.title isEqualToString:title]) return b;
            return nil;
        };
        NSButton* toCheck = [check isKindOfClass:[NSString class]] ? find(check) : nil;
        NSButton* toPress = [press isKindOfClass:[NSString class]] ? find(press) : nil;
        if ([check isKindOfClass:[NSString class]] && !toCheck) return fail(requestId, [@"no checkbox " stringByAppendingString:check]);
        if ([press isKindOfClass:[NSString class]] && !toPress) return fail(requestId, [@"no button " stringByAppendingString:press]);
        NSMutableArray* buttons = [NSMutableArray array];
        for (NSButton* b in all) [buttons addObject:@{@"title" : b.title ?: @"", @"on" : @(b.state == NSControlStateValueOn)}];
        toCheck.state = NSControlStateValueOn;
        [toPress performClick:nil];
        reply(requestId, @{@"texts" : texts, @"buttons" : buttons});
    }

    /** A left click at (x, y) CSS px of the page, given to the view under it as the key window would: the app may be
     *  in the background (the user working elsewhere), where the window would only take a first click to activate. */
    void click(double x, double y) {
        const double zoom = web.pageZoom > 0 ? web.pageZoom : 1.0;
        const NSPoint inView = web.isFlipped ? NSMakePoint(x * zoom, y * zoom) : NSMakePoint(x * zoom, web.bounds.size.height - y * zoom);
        const NSPoint p = [web convertPoint:inView toView:nil];
        NSView* frame = window.contentView.superview ?: window.contentView;
        NSView* target = [frame hitTest:[frame convertPoint:p fromView:nil]] ?: web;
        const NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
        auto event = [&](NSEventType type) {
            return [NSEvent mouseEventWithType:type location:p modifierFlags:0 timestamp:now windowNumber:window.windowNumber
                                       context:nil eventNumber:0 clickCount:1
                                      pressure:type == NSEventTypeLeftMouseDown ? 1.0f : 0.0f];
        };
        [target mouseDown:event(NSEventTypeLeftMouseDown)];
        [target mouseUp:event(NSEventTypeLeftMouseUp)];
    }
};

Automation::Automation(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

Automation::~Automation() = default;

std::unique_ptr<Automation> Automation::start(const std::string& socketPath, NSWindow* window, WKWebView* web,
                                              std::string& error) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socketPath.size() >= sizeof addr.sun_path) {
        error = "the socket path is too long";
        return nullptr;
    }
    std::memcpy(addr.sun_path, socketPath.c_str(), socketPath.size() + 1);
    auto impl = std::make_shared<Impl>();
    impl->window = window;
    impl->web = web;
    impl->path = socketPath;
    impl->listenFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (impl->listenFd < 0) {
        error = std::strerror(errno);
        return nullptr;
    }
    ::fcntl(impl->listenFd, F_SETFD, FD_CLOEXEC);
    ::unlink(socketPath.c_str());
    const mode_t old = ::umask(077);  // owner only: whoever connects runs code in the page
    const int bound = ::bind(impl->listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    ::umask(old);
    if (bound != 0 || ::listen(impl->listenFd, 1) != 0) {
        error = std::strerror(errno);
        return nullptr;
    }
    Impl* raw = impl.get();
    impl->thread = std::thread([raw] { raw->run(); });
    return std::unique_ptr<Automation>(new Automation(std::move(impl)));
}

void Automation::console(const std::string& type, const std::string& text) {
    impl_->send(@{@"event" : @"console", @"type" : ns(type), @"text" : ns(text)});
}

NSString* Automation::consoleHookScript() {
    return @"(() => {"
            "  const post = (type, args) => {"
            "    try {"
            "      const text = args.map((a) => { try { return typeof a === 'string' ? a : JSON.stringify(a); } catch { return String(a); } }).join(' ');"
            "      window.webkit.messageHandlers.stauto.postMessage(JSON.stringify({ type, text }));"
            "    } catch {}"
            "  };"
            "  for (const type of ['log', 'info', 'warn', 'error', 'debug']) {"
            "    const original = console[type].bind(console);"
            "    console[type] = (...args) => { post(type, args); original(...args); };"
            "  }"
            "  addEventListener('error', (e) => post('pageerror', [e.message]));"
            "  addEventListener('unhandledrejection', (e) => post('pageerror', [String((e.reason && e.reason.stack) || e.reason)]));"
            "})();";
}

}  // namespace st::ui::mac
