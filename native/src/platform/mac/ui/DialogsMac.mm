// File dialogs on macOS: NSOpenPanel / NSSavePanel as sheets on the main window. Called off the UI thread (the
// bridge's async handlers), the caller waits while the sheet is up; on the UI thread the panel runs modally.
#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "platform/ui/Dialogs.h"

#include "core/Threading.h"

#include <pthread.h>

#include <future>
#include <memory>

namespace st::ui::dialogs {

namespace {

NSWindow* g_owner = nil;
Dispatcher* g_dispatcher = nullptr;

NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

void configure(NSSavePanel* panel, const FileDialogOptions& o, bool save) {
    if (!o.title.empty()) {
        panel.title = ns(o.title);
        panel.message = ns(o.title);  // sheets show the message, not the title
    }
    panel.canCreateDirectories = YES;
    // One list of types for all filters (the panel has no filter menu); any "*" allows every file.
    NSMutableArray<UTType*>* types = [NSMutableArray array];
    bool any = o.filters.empty();
    for (const auto& f : o.filters) {
        for (const auto& ext : f.extensions) {
            if (ext.empty() || ext == "*") any = true;
            else if (UTType* t = [UTType typeWithFilenameExtension:ns(ext)]) [types addObject:t];
        }
    }
    if (!any && types.count) panel.allowedContentTypes = types;
    if (!o.defaultPath.empty()) {
        NSString* p = ns(o.defaultPath).stringByExpandingTildeInPath;
        BOOL isDir = NO;
        if ([NSFileManager.defaultManager fileExistsAtPath:p isDirectory:&isDir] && isDir) {
            panel.directoryURL = [NSURL fileURLWithPath:p isDirectory:YES];
        } else {
            NSString* dir = p.stringByDeletingLastPathComponent;
            if (dir.length && [NSFileManager.defaultManager fileExistsAtPath:dir])
                panel.directoryURL = [NSURL fileURLWithPath:dir isDirectory:YES];
            if (save || !p.lastPathComponent.length) panel.nameFieldStringValue = p.lastPathComponent;
        }
    }
}

std::vector<std::string> paths(NSArray<NSURL*>* urls) {
    std::vector<std::string> out;
    for (NSURL* u in urls)
        if (u.fileURL) out.emplace_back(u.fileSystemRepresentation);
    return out;
}

/** Shows the panel `make` creates - as a sheet on the main window when there is one - and returns collect(panel, ok).
 *  From any thread: off the UI thread the call waits for the answer. */
template <class R>
R present(NSSavePanel* (^make)(void), std::function<R(NSSavePanel*, bool)> collect) {
    if (pthread_main_np()) {
        NSSavePanel* panel = make();
        const bool ok = [panel runModal] == NSModalResponseOK;
        return collect(panel, ok);
    }
    auto promise = std::make_shared<std::promise<R>>();
    std::future<R> answer = promise->get_future();
    void (^show)(void) = ^{
        NSSavePanel* panel = make();
        if (g_owner && g_owner.visible) {
            [panel beginSheetModalForWindow:g_owner completionHandler:^(NSModalResponse r) {
                promise->set_value(collect(panel, r == NSModalResponseOK));
            }];
        } else {
            const bool ok = [panel runModal] == NSModalResponseOK;
            promise->set_value(collect(panel, ok));
        }
    };
    if (g_dispatcher) {
        g_dispatcher->post([show] { show(); });
    } else {
        CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, show);
        CFRunLoopWakeUp(CFRunLoopGetMain());
    }
    return answer.get();
}

std::optional<std::string> single(NSSavePanel* panel, bool ok) {
    if (!ok || !panel.URL) return std::nullopt;
    return std::string(panel.URL.fileSystemRepresentation);
}

}  // namespace

void init(void* ownerWindow, Dispatcher* dispatcher) {
    g_owner = (__bridge NSWindow*)ownerWindow;
    g_dispatcher = dispatcher;
}

std::optional<std::string> openFile(const FileDialogOptions& opts) {
    return present<std::optional<std::string>>(^NSSavePanel* {
        NSOpenPanel* p = [NSOpenPanel openPanel];
        configure(p, opts, false);
        return p;
    }, single);
}

std::vector<std::string> openFiles(const FileDialogOptions& opts) {
    return present<std::vector<std::string>>(^NSSavePanel* {
        NSOpenPanel* p = [NSOpenPanel openPanel];
        p.allowsMultipleSelection = YES;
        configure(p, opts, false);
        return p;
    }, [](NSSavePanel* panel, bool ok) {
        return ok ? paths(static_cast<NSOpenPanel*>(panel).URLs) : std::vector<std::string>();
    });
}

std::optional<std::string> openDirectory(const FileDialogOptions& opts) {
    return present<std::optional<std::string>>(^NSSavePanel* {
        NSOpenPanel* p = [NSOpenPanel openPanel];
        p.canChooseFiles = NO;
        p.canChooseDirectories = YES;
        configure(p, opts, false);
        return p;
    }, single);
}

std::optional<std::string> saveFile(const FileDialogOptions& opts) {
    return present<std::optional<std::string>>(^NSSavePanel* {
        NSSavePanel* p = [NSSavePanel savePanel];
        configure(p, opts, true);
        return p;
    }, single);
}

}  // namespace st::ui::dialogs
