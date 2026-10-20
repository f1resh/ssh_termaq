#pragma once
// Private to platform/mac/ui, Objective-C++ (include from .mm files): the web view's side of drag & drop
// (HostWindowMac.mm's STWebView) - the mouse events a drag starts from, our own drags dropped back on the window, and
// the files another application (Finder) drops on it.
#import <AppKit/AppKit.h>

#include <string>
#include <vector>

namespace st::ui::dnd::mac {

/** A press or a drag in the web view (mouseDown:, mouseDragged:): a drag the page asks for starts from the latest. */
void noteMouseEvent(NSView* webView, NSEvent* event);
/** prepareForDragOperation: true for one of our own drags, which the web view then refuses: it ends as "internal"
 *  at this point. */
bool takeOwnDrop(id<NSDraggingInfo> info, NSView* webView);
/** performDragOperation: of another application's drag: its files, kept for droppedPaths(). */
void noteDroppedFiles(NSPasteboard* pasteboard);
/** files:pathsForFiles - the paths of the last drop's files, for the page's File names, in their order. */
std::vector<std::string> droppedPaths(const std::vector<std::string>& names);

}  // namespace st::ui::dnd::mac
