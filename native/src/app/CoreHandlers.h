#pragma once
// Bridge handlers owned by the host: sessions:*, credentials:*, settings:*, knownHosts list/remove, serial:*,
// dialog:*, app:* and window:*. (conn/prompts, importer/keys and sftp register themselves.)
#include "app/Paths.h"
#include "core/Types.h"

#include <functional>
#include <mutex>
#include <vector>

namespace st {

class Bridge;
class MainWindow;
class Store;

/** Open requests (CLI / second instance) waiting for the UI. Thread-safe. */
class OpenRequestQueue {
public:
    explicit OpenRequestQueue(Bridge& bridge) : bridge_(bridge) {}
    /** Delivers immediately (app:onOpenRequest) when the UI has drained the queue before, else queues. */
    void push(const OpenRequest& req);
    /** app:takeOpenRequests — returns queued requests and switches to immediate delivery. */
    std::vector<OpenRequest> take();
    /** The UI is (re)loading: queue again until it asks. */
    void reset();

private:
    Bridge& bridge_;
    std::mutex mutex_;
    std::vector<OpenRequest> pending_;
    bool uiReady_ = false;
};

struct CoreHandlerContext {
    Bridge& bridge;
    Store& store;
    MainWindow& window;
    AppPaths paths;
    OpenRequestQueue& openRequests;
};

void registerCoreHandlers(const CoreHandlerContext& ctx);

/** Clipboard helpers (UI thread). */
std::string readClipboardText(void* ownerHwnd);
void writeClipboardText(void* ownerHwnd, const std::string& utf8);

}  // namespace st
