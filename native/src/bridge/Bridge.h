#pragma once
// JSON message router between the WebView2 UI and native services.
// Wire format: see ui/src/shared/ipc.ts. Channels are "namespace:method".
//
// Threading:
//  - onWebMessage() is called on the UI thread (by MainWindow).
//  - handle()/handleSync() handlers run on the UI thread; handleAsync() handlers run on ThreadPool::shared().
//  - Responder::resolve/reject and emit() are thread-safe; they marshal to the UI thread via the Dispatcher.
#include "core/Json.h"
#include "core/Threading.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sc {

class Bridge {
public:
    /** One-shot reply to an invoke. Copyable; first resolve/reject wins; dropping all copies without replying rejects. */
    class Responder {
    public:
        void resolve(Json result = nullptr) const;
        void reject(const std::string& message) const;
        bool done() const;

    private:
        friend class Bridge;
        struct State;
        std::shared_ptr<State> state_;
    };

    using InvokeHandler = std::function<void(const Json& args, Responder reply)>;
    using SyncHandler = std::function<Json(const Json& args)>;
    using SendHandler = std::function<void(const Json& args)>;

    explicit Bridge(Dispatcher& dispatcher);
    ~Bridge();

    /** Where outgoing JSON text goes (MainWindow: ICoreWebView2::PostWebMessageAsJson). Called on the UI thread only. */
    void setSink(std::function<void(const std::string& json)> sink);

    /** Generic invoke handler (UI thread); reply whenever (any thread). */
    void handle(const std::string& channel, InvokeHandler h);
    /** Runs on the UI thread; return value resolves; exceptions reject with e.what(). */
    void handleSync(const std::string& channel, SyncHandler h);
    /** Runs on the shared thread pool; return value resolves; exceptions reject. */
    void handleAsync(const std::string& channel, SyncHandler h);
    /** Fire-and-forget message handler (UI thread). Exceptions are logged. */
    void on(const std::string& channel, SendHandler h);

    /** Push an event to the UI. `args` must be a JSON array (callback arguments). Thread-safe. */
    void emit(const std::string& channel, Json args);

    /** Entry point for messages from the WebView (UI thread). `filePaths` = paths of additional File objects (UTF-8). */
    void onWebMessage(const std::string& json, const std::vector<std::string>& filePaths = {});

    Dispatcher& dispatcher() { return dispatcher_; }

    /** True once at least one handler exists for channel (diagnostics/tests). */
    bool hasHandler(const std::string& channel) const;

private:
    void post(std::string text);  // thread-safe, delivers to sink on UI thread
    void respond(int64_t id, bool ok, const Json& payload);

    Dispatcher& dispatcher_;
    std::function<void(const std::string&)> sink_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, InvokeHandler> invokes_;
    std::unordered_map<std::string, SendHandler> sends_;
    std::shared_ptr<std::atomic<bool>> alive_;
};

}  // namespace sc
