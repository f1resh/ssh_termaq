#pragma once
// Native drag & drop of local files out of the file manager's local pane, started from the page (local:startDrag).
//
//  - The files themselves are offered (Windows: the shell's own data object, CF_HDROP etc.; macOS: file URLs), so
//    Explorer / Finder copy or move them as their own.
//  - Drops that land back on our own window are cancelled and reported as "internal" with the cursor position, so the
//    page decides what to do (copy between panes); Shift held at the drop means "move" (macOS: Shift or Command).
//    The position is in client pixels: Windows physical pixels, macOS CSS pixels.
//  - Server items are dragged inside the page only (between the panes), never out to Explorer / Finder.
#include "core/Types.h"

#include <functional>
#include <string>
#include <vector>

namespace st {
class Dispatcher;
}

namespace st::ui::dnd {

/**
 * Called once by the app (UI thread) with the main window's native handle (HWND, NSWindow); drops on it are
 * "internal". nullptr at shutdown.
 */
void init(void* mainWindow);

/** Starts a drag of local files / folders (all in one folder) on the UI thread. */
void startFileDrag(Dispatcher& dispatcher, std::vector<std::string> paths, std::function<void(const DragResult&)> done);

}  // namespace st::ui::dnd
