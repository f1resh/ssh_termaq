// Drag & drop on macOS: local files go out as file URLs, starting from the web view's latest mouse event while the
// button is still down. Our own drags dropped back on the window are refused there and reported "internal"; files
// another application (Finder) drops on the window are kept for files:pathsForFiles.
#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

#include "platform/ui/DragDrop.h"

#include "core/Threading.h"
#include "platform/mac/ui/DragDropMac.h"
#include "platform/mac/ui/DragDropMacSupport.h"

#include <algorithm>
#include <cmath>

using namespace st;
using namespace st::ui::dnd;

namespace {

NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

NSWindow* g_window = nil;                     // init(): drops on it are "internal"
__weak NSView* g_view = nil;                  // the web view
NSEvent* g_mouse = nil;                       // its latest press or drag
std::vector<std::string> g_dropped;           // the files of the last drop from another application

/** A point of the window in the web view's CSS px. */
NSPoint cssPoint(NSView* view, NSPoint inWindow) {
    const NSPoint p = [view convertPoint:inWindow fromView:nil];
    const double zoom = [view isKindOfClass:[WKWebView class]] && ((WKWebView*)view).pageZoom > 0 ? ((WKWebView*)view).pageZoom : 1.0;
    return NSMakePoint(p.x / zoom, (view.isFlipped ? p.y : view.bounds.size.height - p.y) / zoom);
}

/** Where the n-th dragged item's icon goes: a small pile under the pointer. */
NSRect iconFrame(NSPoint at, NSUInteger n) {
    const CGFloat shift = 4.0 * std::min<NSUInteger>(n, 4);
    return NSMakeRect(at.x - 16 + shift, at.y - 16 + shift, 32, 32);
}

bool moveModifiers() { return (NSEvent.modifierFlags & (NSEventModifierFlagShift | NSEventModifierFlagCommand)) != 0; }

}  // namespace

/* ------------------------------------------------------------------------ */
/* Drag source                                                               */
/* ------------------------------------------------------------------------ */

@interface STDragSource : NSObject <NSDraggingSource>
@property(nonatomic) NSDragOperation outside;
@property(nonatomic, weak) NSDraggingSession* session;
@end

namespace {
STDragSource* g_active = nil;  // the drag in progress
}

@implementation STDragSource {
@public
    std::function<void(const DragResult&)> done;
    bool internal;
    NSPoint at;  // CSS px
    bool move;
}

- (NSDragOperation)draggingSession:(NSDraggingSession*)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
    return context == NSDraggingContextOutsideApplication ? self.outside : NSDragOperationCopy | NSDragOperationMove;
}

/** Dropped where nothing took it: on our own window (a part of the page that does not take files) it is internal. */
- (void)checkWindowAt:(NSPoint)screenPoint {
    NSView* view = g_view;
    if (!g_window || !view || [NSWindow windowNumberAtPoint:screenPoint belowWindowWithWindowNumber:0] != g_window.windowNumber)
        return;
    const NSPoint inView = [view convertPoint:[g_window convertPointFromScreen:screenPoint] fromView:nil];
    if (!NSPointInRect(inView, view.bounds)) return;
    internal = true;
    at = cssPoint(view, [g_window convertPointFromScreen:screenPoint]);
    move = moveModifiers();
}

- (void)draggingSession:(NSDraggingSession*)session endedAtPoint:(NSPoint)screenPoint operation:(NSDragOperation)operation {
    if (!internal && operation == NSDragOperationNone) [self checkWindowAt:screenPoint];
    DragResult r;
    if (internal) {
        r.result = "internal";
        r.x = static_cast<int>(std::lround(at.x));
        r.y = static_cast<int>(std::lround(at.y));
        r.move = move;
    } else if (operation != NSDragOperationNone) {
        r.result = "external";
    }
    // The web view saw the press that started the drag but not its release: give it one.
    if (NSView* view = g_view; view && g_window) {
        [view mouseUp:[NSEvent mouseEventWithType:NSEventTypeLeftMouseUp
                                         location:[g_window convertPointFromScreen:screenPoint]
                                    modifierFlags:0
                                        timestamp:NSProcessInfo.processInfo.systemUptime
                                     windowNumber:g_window.windowNumber
                                          context:nil
                                      eventNumber:0
                                       clickCount:1
                                         pressure:0]];
    }
    if (g_active == self) g_active = nil;
    if (done) done(r);
}

@end

namespace {

/** Starts a drag of the items makeItems() gives for the pointer's place (UI thread); cancelled at once when the
 *  button is up already or another drag runs. */
void beginDrag(NSArray<NSDraggingItem*>* (^makeItems)(NSPoint at), NSDragOperation outside,
               std::function<void(const DragResult&)> done) {
    NSView* view = g_view;
    NSEvent* event = g_mouse;
    if (!view || !view.window || !event || !(NSEvent.pressedMouseButtons & 1) || g_active) {
        if (done) done(DragResult{});
        return;
    }
    NSArray<NSDraggingItem*>* items = makeItems([view convertPoint:event.locationInWindow fromView:nil]);
    if (!items.count) {
        if (done) done(DragResult{});
        return;
    }
    STDragSource* source = [[STDragSource alloc] init];
    source->done = std::move(done);
    source.outside = outside;
    g_active = source;
    NSDraggingSession* session = [view beginDraggingSessionWithItems:items event:event source:source];
    session.animatesToStartingPositionsOnCancelOrFail = YES;
    session.draggingFormation = NSDraggingFormationPile;
    source.session = session;
}

}  // namespace

/* ------------------------------------------------------------------------ */
/* The web view's side                                                       */
/* ------------------------------------------------------------------------ */

namespace st::ui::dnd::mac {

void noteMouseEvent(NSView* webView, NSEvent* event) {
    g_view = webView;
    g_mouse = event;
}

bool takeOwnDrop(id<NSDraggingInfo> info, NSView* webView) {
    STDragSource* source = g_active;
    if (!source || info.draggingSource != source) return false;
    source->internal = true;
    source->at = cssPoint(webView, info.draggingLocation);
    source->move = moveModifiers();
    source.session.animatesToStartingPositionsOnCancelOrFail = NO;  // the page takes it from here
    return true;
}

void noteDroppedFiles(NSPasteboard* pasteboard) {
    g_dropped.clear();
    NSArray<NSURL*>* urls = [pasteboard readObjectsForClasses:@[ [NSURL class] ]
                                                      options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    for (NSURL* url in urls) g_dropped.emplace_back(url.fileSystemRepresentation);
}

std::vector<std::string> droppedPaths(const std::vector<std::string>& names) {
    // File names reach the page precomposed; the file system may keep them decomposed.
    auto nfc = [](const std::string& s) {
        return std::string(ns(s).precomposedStringWithCanonicalMapping.UTF8String ?: "");
    };
    std::vector<std::string> dropped, wanted;
    for (const auto& p : g_dropped) dropped.push_back(nfc(p));
    for (const auto& n : names) wanted.push_back(nfc(n));
    std::vector<std::string> out;
    for (const auto& match : matchDropped(dropped, wanted)) {
        // Back to the path as the file system spells it.
        const auto it = std::find(dropped.begin(), dropped.end(), match);
        out.push_back(g_dropped[static_cast<size_t>(it - dropped.begin())]);
    }
    return out;
}

}  // namespace st::ui::dnd::mac

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

namespace st::ui::dnd {

void init(void* mainWindow) { g_window = (__bridge NSWindow*)mainWindow; }

void startFileDrag(Dispatcher& dispatcher, std::vector<std::string> paths, std::function<void(const DragResult&)> done) {
    dispatcher.post([paths = std::move(paths), done = std::move(done)]() mutable {
        beginDrag(
            ^NSArray<NSDraggingItem*>*(NSPoint at) {
                NSMutableArray<NSDraggingItem*>* items = [NSMutableArray array];
                for (const auto& p : paths) {
                    NSDraggingItem* item = [[NSDraggingItem alloc] initWithPasteboardWriter:[NSURL fileURLWithPath:ns(p)]];
                    [item setDraggingFrame:iconFrame(at, items.count) contents:[NSWorkspace.sharedWorkspace iconForFile:ns(p)]];
                    [items addObject:item];
                }
                return items;
            },
            // As with the files themselves: Finder moves within a volume and copies across, Option / Command change it.
            NSDragOperationCopy | NSDragOperationMove | NSDragOperationLink | NSDragOperationGeneric, std::move(done));
    });
}

}  // namespace st::ui::dnd
