#pragma once
// Native (OLE) drag & drop out of the file manager's panes.
//
//  - Remote items are offered as *virtual files* (CFSTR_FILEDESCRIPTORW + CFSTR_FILECONTENTS streams) that Explorer
//    pulls on drop. The data object lives in the MTA: Explorer's calls (enumeration, IStream::Read over the network)
//    run on RPC threads and never block the UI thread. It supports IDataObjectAsyncCapability, so Explorer copies in
//    the background with its own progress UI.
//  - Local items use the shell's own data object (CF_HDROP etc.).
//  - The drag source cancels drops that land back on our own window and reports them as "internal" with the cursor
//    position, so the page decides what to do (copy between panes); Shift held at the drop means "move".
#include "core/Types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct IDataObject;

namespace sc {
class Dispatcher;
}

namespace sc::dnd {

/** One file or folder of a virtual drag. Folders come before their contents. */
struct VirtualItem {
    /** '/'-separated path inside the drop, e.g. "folder/sub/file.txt" (at most 259 characters). */
    std::string relPath;
    bool dir = false;
    uint64_t size = 0;
    int64_t mtime = 0;  // Unix s (0 = unknown)
};

/** Sequential reader of one virtual file; read() returns 0 at the end and throws on errors. */
class VirtualReader {
public:
    virtual ~VirtualReader() = default;
    virtual size_t read(void* buffer, size_t length) = 0;
};

/** Contents of a virtual drag. Called on COM / RPC threads (never the UI thread); calls may block. */
class VirtualSource {
public:
    virtual ~VirtualSource() = default;
    /** All items; called lazily, once, when the drop target first asks. */
    virtual std::vector<VirtualItem> enumerate() = 0;
    /** Opens item `index` of enumerate() (a file). */
    virtual std::unique_ptr<VirtualReader> open(size_t index) = 0;
};

/** Called once by the app (UI thread) with the main window; drops on it are "internal". Keeps the MTA alive. */
void init(void* mainWindow);

/** Starts a virtual-file drag on the UI thread (through the dispatcher); done() gets the outcome. */
void startVirtualDrag(Dispatcher& dispatcher, std::shared_ptr<VirtualSource> source,
                      std::function<void(const DragResult&)> done);
/** Starts a drag of local files / folders (all in one folder) on the UI thread. */
void startFileDrag(Dispatcher& dispatcher, std::vector<std::string> paths, std::function<void(const DragResult&)> done);

/* ---- building blocks (exposed for tests) --------------------------------------------------------- */

/** Virtual data object in the calling apartment (refcount 1). progressUi sets FD_PROGRESSUI on every descriptor. */
IDataObject* createVirtualDataObject(std::shared_ptr<VirtualSource> source, bool progressUi);
/** The same object created in the MTA; returns a proxy for the calling STA thread (refcount 1). */
IDataObject* createVirtualDataObjectInMta(std::shared_ptr<VirtualSource> source, bool progressUi);
/** Shell data object for local paths (refcount 1), nullptr when the paths cannot be resolved. */
IDataObject* createShellDataObject(const std::vector<std::string>& paths);

}  // namespace sc::dnd
