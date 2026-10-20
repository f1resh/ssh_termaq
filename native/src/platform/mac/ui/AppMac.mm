// The app shell on macOS: NSApplication on the main thread with its menus, Quit and Dock clicks handed to the main
// window (AppMac.h), URLs from Launch Services, one instance per data folder - a lock file and a Unix socket in the
// per-user temporary folder, over which a second instance hands over its command line - and alerts.
//
// Launch Services itself starts an app bundle only once; a second process comes from running the executable
// directly (a terminal, a script), and hands its arguments to the first one.
#import <AppKit/AppKit.h>

#include "platform/ui/App.h"

#include "core/Log.h"
#include "platform/mac/ui/AppMac.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace {

using st::ui::mac::AppEvents;

AppEvents* g_events = nullptr;
std::vector<std::pair<std::vector<std::string>, std::filesystem::path>> g_pendingOpens;  // before the window exists
int g_exitCode = 0;
std::string g_primarySocket;  // the primary instance's socket, for forwardToPrimary()

NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

/** Runs `block` on the main thread in the common modes: also while a menu is tracked or a modal panel runs, when the
 *  GCD main queue would wait. */
void runOnMain(void (^block)(void)) {
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, block);
    CFRunLoopWakeUp(CFRunLoopGetMain());
}

std::filesystem::path homeDir() { return std::filesystem::path(NSHomeDirectory().fileSystemRepresentation); }

void deliverOpen(std::vector<std::string> args, std::filesystem::path cwd) {
    if (g_events && g_events->openRequest) g_events->openRequest(args, cwd);
    else g_pendingOpens.emplace_back(std::move(args), std::move(cwd));
}

/** This process's arguments without argv[0] and Launch Services' -psn_ argument. */
std::vector<std::string> ownArguments() {
    std::vector<std::string> args;
    NSArray<NSString*>* all = [[NSProcessInfo processInfo] arguments];
    for (NSUInteger i = 1; i < all.count; ++i)
        if (![all[i] hasPrefix:@"-psn_"]) args.emplace_back(all[i].UTF8String);
    return args;
}

std::string tempPath(const std::string& name) {
    std::string dir = NSTemporaryDirectory().fileSystemRepresentation;  // per user
    if (!dir.empty() && dir.back() != '/') dir += '/';
    return dir + name;
}

bool socketAddress(const std::string& path, sockaddr_un& addr) {
    addr = {};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) return false;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return true;
}

/** The primary instance's lock and listening socket. */
struct Instance {
    int lockFd = -1;
    int listenFd = -1;
    std::string socketPath;
    std::atomic<bool> stop{false};
    std::thread listener;

    ~Instance() {
        stop = true;
        if (listener.joinable()) listener.join();
        if (listenFd >= 0) {
            ::close(listenFd);
            ::unlink(socketPath.c_str());
        }
        if (lockFd >= 0) ::close(lockFd);  // releases the lock
    }

    /** One connection: "<cwd>\0<arg>\0<arg>\0..." until EOF, answered with one byte once taken. */
    static void serve(int fd) {
        std::string msg;
        char buf[4096];
        for (;;) {
            pollfd p{fd, POLLIN, 0};
            if (::poll(&p, 1, 3000) <= 0) return;
            const ssize_t n = ::read(fd, buf, sizeof buf);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            msg.append(buf, static_cast<size_t>(n));
            if (msg.size() > (1u << 20)) return;
        }
        std::vector<std::string> parts;
        size_t pos = 0;
        while (pos < msg.size()) {
            const size_t end = msg.find('\0', pos);
            parts.push_back(msg.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
            if (end == std::string::npos) break;
            pos = end + 1;
        }
        if (parts.empty()) return;
        std::filesystem::path cwd = parts[0];
        std::vector<std::string> args(parts.begin() + 1, parts.end());
        runOnMain(^{
            deliverOpen(args, cwd);
        });
        const char ack = 1;
        (void)!::write(fd, &ack, 1);
    }

    void listen() {
        listener = std::thread([this] {
            while (!stop) {
                pollfd p{listenFd, POLLIN, 0};
                if (::poll(&p, 1, 250) <= 0) continue;
                const int c = ::accept(listenFd, nullptr, nullptr);
                if (c < 0) continue;
                const int one = 1;
                ::setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
                serve(c);
                ::close(c);
            }
        });
    }
};

}  // namespace

@interface STAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation STAppDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)note {
    if ([NSApp respondsToSelector:@selector(activate)]) [NSApp activate];
    else [NSApp activateIgnoringOtherApps:YES];
}

/** Quit closes the main window the way its close button does (which may ask the page first); the event loop ends
 *  when it is gone, so the app shuts down in order instead of exiting from here. */
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    if (g_events && g_events->quitRequested) g_events->quitRequested();
    else st::ui::mac::stopEventLoop(0);
    return NSTerminateCancel;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender { return NO; }

- (BOOL)applicationShouldHandleReopen:(NSApplication*)sender hasVisibleWindows:(BOOL)visible {
    deliverOpen({}, homeDir());  // a Dock click: the window comes to the front
    return NO;
}

- (void)application:(NSApplication*)application openURLs:(NSArray<NSURL*>*)urls {
    for (NSURL* url in urls) deliverOpen({url.absoluteString.UTF8String}, homeDir());
}

- (BOOL)applicationSupportsSecureRestorableState:(NSApplication*)app { return YES; }

@end

namespace st::ui {

namespace {

STAppDelegate* g_delegate = nil;

NSMenuItem* item(NSString* title, SEL action, NSString* key, NSEventModifierFlags modifiers = NSEventModifierFlagCommand) {
    NSMenuItem* i = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
    i.keyEquivalentModifierMask = modifiers;
    return i;
}

std::function<void(const std::string&)> g_runCommand;  // the page's commands, while it has menus

/** A page chord ("Meta+Shift+T", "Ctrl+Alt+ArrowLeft", "F11") as a key equivalent; false when it has no equivalent. */
bool keyEquivalent(const std::string& chord, NSString** key, NSEventModifierFlags* modifiers) {
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= chord.size()) {
        const size_t plus = chord.find('+', pos);
        parts.push_back(chord.substr(pos, plus == std::string::npos ? std::string::npos : plus - pos));
        if (plus == std::string::npos) break;
        pos = plus + 1;
    }
    if (parts.empty() || parts.back().empty()) return false;
    NSEventModifierFlags mods = 0;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        if (parts[i] == "Ctrl") mods |= NSEventModifierFlagControl;
        else if (parts[i] == "Alt") mods |= NSEventModifierFlagOption;
        else if (parts[i] == "Shift") mods |= NSEventModifierFlagShift;
        else if (parts[i] == "Meta") mods |= NSEventModifierFlagCommand;
        else return false;
    }
    const std::string& k = parts.back();
    static const std::pair<const char*, unichar> kNamed[] = {
        {"ArrowUp", NSUpArrowFunctionKey}, {"ArrowDown", NSDownArrowFunctionKey}, {"ArrowLeft", NSLeftArrowFunctionKey},
        {"ArrowRight", NSRightArrowFunctionKey}, {"Home", NSHomeFunctionKey}, {"End", NSEndFunctionKey},
        {"PageUp", NSPageUpFunctionKey}, {"PageDown", NSPageDownFunctionKey}, {"Delete", NSDeleteFunctionKey},
        {"Insert", NSInsertFunctionKey}, {"Tab", '\t'}, {"Enter", '\r'}, {"Escape", 0x1b}, {"Space", ' '},
        {"Backspace", 0x08},
    };
    unichar c = 0;
    if (k.size() == 1) {
        c = static_cast<unichar>(std::tolower(static_cast<unsigned char>(k[0])));
    } else if (k.size() >= 2 && k[0] == 'F' && std::isdigit(static_cast<unsigned char>(k[1]))) {
        const int n = std::atoi(k.c_str() + 1);
        if (n < 1 || n > 24) return false;
        c = static_cast<unichar>(NSF1FunctionKey + n - 1);
    } else {
        for (const auto& [name, code] : kNamed)
            if (k == name) c = code;
        if (!c) return false;
    }
    *key = [NSString stringWithCharacters:&c length:1];
    *modifiers = mods;
    return true;
}

}  // namespace

}  // namespace st::ui

/** Target of the page's menu items: hands the chosen command to the page. */
@interface STMenuTarget : NSObject
- (void)runCommand:(NSMenuItem*)sender;
@end

@implementation STMenuTarget
- (void)runCommand:(NSMenuItem*)sender {
    NSString* command = sender.representedObject;
    if (st::ui::g_runCommand && command) st::ui::g_runCommand(command.UTF8String);
}
@end

namespace st::ui {

namespace {

STMenuTarget* g_menuTarget = nil;

/** A menu of the page's commands; its items keep the enabled state the page gave them. */
NSMenu* pageMenu(NSString* title, const std::vector<MenuBarItem>& items) {
    NSMenu* menu = [[NSMenu alloc] initWithTitle:title];
    menu.autoenablesItems = NO;
    for (const auto& i : items) {
        if (i.separator) {
            [menu addItem:[NSMenuItem separatorItem]];
            continue;
        }
        NSString* key = @"";
        NSEventModifierFlags mods = 0;
        if (!keyEquivalent(i.shortcut, &key, &mods)) {
            key = @"";
            mods = 0;
        }
        NSMenuItem* it = item(ns(i.label), @selector(runCommand:), key, mods);
        it.target = g_menuTarget;
        it.representedObject = ns(i.command);
        it.state = i.checked ? NSControlStateValueOn : NSControlStateValueOff;
        it.enabled = i.enabled;
        [menu addItem:it];
    }
    return menu;
}

/** App, File (the page's), Edit (Cmd+C / Cmd+V in text fields need it), View... (the page's) and Window menus. */
void buildMainMenu(const std::vector<MenuBarMenu>& pageMenus) {
    if (!g_menuTarget) g_menuTarget = [STMenuTarget new];
    NSString* name = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleName"] ?: @"SSHTermaq";
    NSMenu* bar = [NSMenu new];

    NSMenu* app = [[NSMenu alloc] initWithTitle:name];
    app.autoenablesItems = NO;
    const MenuBarMenu* appItems = nullptr;
    for (const auto& m : pageMenus)
        if (m.title == "app") appItems = &m;
    if (appItems) {
        for (NSMenuItem* it in pageMenu(name, appItems->items).itemArray.copy) {
            [it.menu removeItem:it];
            [app addItem:it];
        }
    } else {
        [app addItem:item([@"About " stringByAppendingString:name], @selector(orderFrontStandardAboutPanel:), @"")];
    }
    [app addItem:[NSMenuItem separatorItem]];
    NSMenu* services = [NSMenu new];
    NSMenuItem* servicesItem = item(@"Services", nil, @"");
    servicesItem.submenu = services;
    NSApp.servicesMenu = services;
    [app addItem:servicesItem];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItem:item([@"Hide " stringByAppendingString:name], @selector(hide:), @"h")];
    [app addItem:item(@"Hide Others", @selector(hideOtherApplications:), @"h", NSEventModifierFlagCommand | NSEventModifierFlagOption)];
    [app addItem:item(@"Show All", @selector(unhideAllApplications:), @"")];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItem:item([@"Quit " stringByAppendingString:name], @selector(terminate:), @"q")];

    NSMenu* edit = [[NSMenu alloc] initWithTitle:@"Edit"];
    [edit addItem:item(@"Undo", @selector(undo:), @"z")];
    [edit addItem:item(@"Redo", @selector(redo:), @"z", NSEventModifierFlagCommand | NSEventModifierFlagShift)];
    [edit addItem:[NSMenuItem separatorItem]];
    [edit addItem:item(@"Cut", @selector(cut:), @"x")];
    [edit addItem:item(@"Copy", @selector(copy:), @"c")];
    [edit addItem:item(@"Paste", @selector(paste:), @"v")];
    [edit addItem:item(@"Select All", @selector(selectAll:), @"a")];

    NSMenu* window = [[NSMenu alloc] initWithTitle:@"Window"];
    [window addItem:item(@"Minimize", @selector(performMiniaturize:), @"m")];
    [window addItem:item(@"Zoom", @selector(performZoom:), @"")];
    [window addItem:item(@"Enter Full Screen", @selector(toggleFullScreen:), @"f", NSEventModifierFlagCommand | NSEventModifierFlagControl)];
    [window addItem:[NSMenuItem separatorItem]];
    [window addItem:item(@"Bring All to Front", @selector(arrangeInFront:), @"")];
    NSApp.windowsMenu = window;

    std::vector<NSMenu*> menus{app};
    for (const auto& m : pageMenus)
        if (m.title == "File") menus.push_back(pageMenu(@"File", m.items));
    menus.push_back(edit);
    for (const auto& m : pageMenus)
        if (m.title != "app" && m.title != "File") menus.push_back(pageMenu(ns(m.title), m.items));
    menus.push_back(window);
    for (NSMenu* m : menus) {
        NSMenuItem* top = [NSMenuItem new];
        top.submenu = m;
        [bar addItem:top];
    }
    NSApp.mainMenu = bar;
}

void logException(NSException* e) {
    applog::error(std::string("Objective-C exception ") + e.name.UTF8String + ": " + (e.reason ? e.reason.UTF8String : ""));
}

/** NSApplication exists and may show windows (also before the UI thread is set up, for showMessage). */
void ensureApplication() {
    [NSApplication sharedApplication];
    if (NSApp.activationPolicy != NSApplicationActivationPolicyRegular)
        NSApp.activationPolicy = NSApplicationActivationPolicyRegular;
}

}  // namespace

void installCrashLogging() { NSSetUncaughtExceptionHandler(&logException); }

UiThread::UiThread() {
    ensureApplication();
    g_delegate = [STAppDelegate new];
    NSApp.delegate = g_delegate;
    buildMainMenu({});
    initialized_ = true;
}

UiThread::~UiThread() {
    NSApp.delegate = nil;
    g_delegate = nil;
}

int runEventLoop() {
    g_exitCode = 0;
    [NSApp run];
    return g_exitCode;
}

InstanceLock::InstanceLock(const std::string& key) {
    auto inst = std::make_unique<Instance>();
    const std::string lockPath = tempPath("sshtermaq-" + key + ".lock");
    inst->socketPath = tempPath("sshtermaq-" + key + ".sock");
    g_primarySocket = inst->socketPath;
    inst->lockFd = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (inst->lockFd < 0 || ::flock(inst->lockFd, LOCK_EX | LOCK_NB) != 0) {
        // Another process has it (or the folder is unusable: then run without the single-instance guard).
        primary_ = inst->lockFd < 0;
        if (inst->lockFd < 0) applog::warn(std::string("Cannot create the instance lock: ") + std::strerror(errno));
        return;
    }
    primary_ = true;
    sockaddr_un addr;
    if (socketAddress(inst->socketPath, addr)) {
        ::unlink(inst->socketPath.c_str());  // left over from a crashed instance: we hold the lock now
        inst->listenFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (inst->listenFd >= 0) {
            ::fcntl(inst->listenFd, F_SETFD, FD_CLOEXEC);
            if (::bind(inst->listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0 && ::listen(inst->listenFd, 8) == 0) {
                inst->listen();
            } else {
                applog::warn(std::string("Cannot listen for other instances: ") + std::strerror(errno));
                ::close(inst->listenFd);
                inst->listenFd = -1;
            }
        }
    }
    handle_ = inst.release();
}

InstanceLock::~InstanceLock() { delete static_cast<Instance*>(handle_); }

bool InstanceLock::forwardToPrimary() {
    sockaddr_un addr;
    if (g_primarySocket.empty() || !socketAddress(g_primarySocket, addr)) return false;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    bool ok = false;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) {
        std::error_code ec;
        std::string msg = std::filesystem::current_path(ec).string();
        msg.push_back('\0');
        for (const auto& a : ownArguments()) {
            msg += a;
            msg.push_back('\0');
        }
        size_t sent = 0;
        while (sent < msg.size()) {
            const ssize_t n = ::write(fd, msg.data() + sent, msg.size() - sent);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            sent += static_cast<size_t>(n);
        }
        ::shutdown(fd, SHUT_WR);
        char ack = 0;
        pollfd p{fd, POLLIN, 0};
        ok = sent == msg.size() && ::poll(&p, 1, 3000) > 0 && ::read(fd, &ack, 1) == 1;
    }
    ::close(fd);
    return ok;
}

void showMessage(const std::string& text, MessageKind kind) {
    @autoreleasepool {
        ensureApplication();
        NSAlert* alert = [NSAlert new];
        alert.messageText = ns(text);
        alert.alertStyle = kind == MessageKind::Error ? NSAlertStyleCritical : NSAlertStyleInformational;
        if ([NSApp respondsToSelector:@selector(activate)]) [NSApp activate];
        else [NSApp activateIgnoringOtherApps:YES];
        [alert runModal];
    }
}

namespace mac {

void setAppEvents(AppEvents* events) {
    g_events = events;
    if (!events) return;
    auto pending = std::move(g_pendingOpens);
    g_pendingOpens.clear();
    for (auto& [args, cwd] : pending) deliverOpen(args, cwd);
}

void setMenuBar(const std::vector<MenuBarMenu>& menus, std::function<void(const std::string& command)> run) {
    g_runCommand = std::move(run);
    buildMainMenu(menus);
}

void stopEventLoop(int exitCode) {
    g_exitCode = exitCode;
    [NSApp stop:nil];
    // stop: takes effect after the next event: post one.
    NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0
                                   windowNumber:0 context:nil subtype:0 data1:0 data2:0];
    [NSApp postEvent:wake atStart:YES];
}

}  // namespace mac

}  // namespace st::ui
