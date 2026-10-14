// SessionManager (owner: session module).
//
// Per connection (class Connection, also the TransportListener + Prompter handed to the transport):
//   - a "starter" thread runs the pre-connect work that may block (touch the saved session, open the session
//     log - which can prompt "file exists" - then Transport::start()). It never runs on the UI thread.
//   - output: Transport thread -> Decoder -> SessionLog -> pending batch. The batch is emitted as "conn:onData"
//     immediately when the previous emission is >= 8 ms old, otherwise the flusher thread emits it when the 8 ms
//     window ends (or at once when it exceeds 64 KiB). Every emission for a connection happens under its
//     outMutex and status events flush the batch first, so the UI sees data and status in causal order.
//     Nothing is emitted from the UI thread (Bridge::emit delivers synchronously there, which could overtake
//     queued events), so UI-thread producers (local echo) always defer to the flusher.
//   - input: UI thread -> LineDiscipline -> encodeFromUtf8 -> Transport::write (serialised by stateMutex;
//     writes made before start() are queued and replayed after it).
//   - close: onClose (exactly once) withdraws pending prompts, flushes data, emits the final status, closes the
//     session log and hands the connection to the worker thread, which destroys the Transport (joining its
//     I/O thread) outside any transport callback. The Connection object stays in the map (without transport)
//     so the event log remains readable; at most kMaxClosedRetained closed connections are kept.
//
// Lock order: ManagerCore::mutex -> Connection::stateMutex;  Connection::inMutex -> stateMutex -> outMutex;
//             ManagerCore::mutex -> PendingPrompt::m;  flushMutex and workMutex are leaf locks.
// Shutdown: closeAll() waits (bounded) until every connection has been reaped, i.e. no transport or starter
// thread can call back into the manager any more.
#include "session/SessionManager.h"

#include "bridge/Bridge.h"
#include "core/Log.h"
#include "core/Strings.h"
#include "core/Uuid.h"
#include "session/Charset.h"
#include "session/LineDiscipline.h"
#include "session/Prompter.h"
#include "session/SessionLog.h"
#include "store/Store.h"
#include "transport/Transport.h"
#include "transport/TransportFactory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace st {

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kMaxEventLog = 2000;
constexpr size_t kMaxClosedRetained = 64;
constexpr size_t kMaxBatchBytes = 64 * 1024;
constexpr auto kFlushInterval = std::chrono::milliseconds(8);
constexpr auto kPromptTimeout = std::chrono::minutes(10);
constexpr auto kCloseAllWait = std::chrono::seconds(8);

struct PendingPrompt {
    std::string id;
    std::string connId;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    bool withdrawn = false;
    PromptResponse response;
};

const char* logModeLabel(LogMode m) {
    switch (m) {
        case LogMode::Printable: return "printable output";
        case LogMode::All: return "all session output";
        case LogMode::SshDebug: return "SSH debug";
        case LogMode::Off: break;
    }
    return "off";
}

}  // namespace

class Connection;

/** Manager state shared with Connection (SessionManager::Impl is private, so it derives from this). */
struct ManagerCore {
    ManagerCore(Store& s, Bridge& b);
    ~ManagerCore();
    ManagerCore(const ManagerCore&) = delete;
    ManagerCore& operator=(const ManagerCore&) = delete;

    Store& store;
    Bridge& bridge;

    mutable std::mutex mutex;  // conns, closedOrder, prompts, factory, unreaped
    std::condition_variable reapedCv;
    size_t unreaped = 0;  // launched connections whose transport has not been torn down yet
    std::unordered_map<std::string, std::shared_ptr<Connection>> conns;
    std::deque<std::string> closedOrder;
    std::unordered_map<std::string, std::shared_ptr<PendingPrompt>> prompts;
    SessionManager::TransportFactoryFn factory;

    // Flusher: deferred "conn:onData" emissions.
    std::mutex flushMutex;
    std::condition_variable flushCv;
    std::vector<std::pair<Clock::time_point, std::weak_ptr<Connection>>> flushQueue;
    bool flushStop = false;
    std::thread flusher;

    // Worker: transport teardown (reaping) and small blocking jobs (Store::touchSession).
    std::mutex workMutex;
    std::condition_variable workCv;
    std::deque<std::function<void()>> work;
    bool workStop = false;
    std::thread worker;

    void scheduleFlush(const std::shared_ptr<Connection>& c, Clock::time_point due);
    void post(std::function<void()> job);
    void flusherLoop();
    void workerLoop();
    void connectionClosed(const std::shared_ptr<Connection>& c);
    void cancelPrompts(const std::string& connId);
    std::shared_ptr<Connection> find(const std::string& connId) const;
    std::shared_ptr<Connection> findLive(const std::string& connId) const;
};

/* ------------------------------------------------------------------------------------------------------------ */
/* Connection                                                                                                     */
/* ------------------------------------------------------------------------------------------------------------ */

class Connection final : public TransportListener, public Prompter, public std::enable_shared_from_this<Connection> {
public:
    Connection(ManagerCore& mgr, const ConnOpenRequest& req)
        : mgr_(mgr), connId_(req.connId), session_(req.session), cols_(req.cols), rows_(req.rows),
          widthPx_(req.widthPx), heightPx_(req.heightPx), decoder_(req.session.translation.charset),
          log_(req.session, mgr.store.logDirectory()) {}

    ~Connection() override {
        if (starter_.joinable()) {
            if (starter_.get_id() == std::this_thread::get_id())
                starter_.detach();
            else
                starter_.join();
        }
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    const std::string& id() const { return connId_; }
    const SessionConfig& session() const { return session_; }

    /** Second construction phase (UI thread): adopt the transport, then launch the starter thread. */
    void launch(std::unique_ptr<Transport> transport) {
        transport_ = std::shared_ptr<Transport>(std::move(transport));
        ldisc_ = std::make_unique<LineDiscipline>(
            session_.terminal.localEcho, session_.terminal.localLineEditing, transport_->defaultLdisc(),
            LineDiscipline::Sinks{[this](const std::string& s) { toRemote(s); },
                                  [this](const std::string& s) { toTerminal(s); }});
        if (const std::string& cs = session_.translation.charset; !trim(cs).empty() && codePageForCharset(cs) == 0)
            onLog(LogLevel::Warn, "Unknown character set \"" + cs + "\"; using UTF-8");
        starter_ = std::thread([self = shared_from_this()] { self->runStarter(); });
    }

    /* ---- state queries ---- */

    bool closed() const {
        std::lock_guard lock(stateMutex_);
        return closed_;
    }

    std::shared_ptr<Transport> liveTransport() const {
        std::lock_guard lock(stateMutex_);
        return closed_ ? nullptr : transport_;
    }

    std::vector<EventLogEntry> eventLog() const {
        std::lock_guard lock(stateMutex_);
        return {events_.begin(), events_.end()};
    }

    std::vector<SpecialCommandDef> specials() const {
        std::shared_ptr<Transport> t = liveTransport();
        return t ? t->specials() : std::vector<SpecialCommandDef>{};
    }

    /* ---- UI -> remote ---- */

    void write(const std::string& utf8, bool binary) {
        if (detached_ || closing_) return;
        if (binary) {
            transportWrite(binaryStringToBytes(utf8));
            return;
        }
        std::lock_guard lock(inMutex_);
        if (ldisc_) ldisc_->input(utf8);
    }

    void resize(int cols, int rows, int widthPx, int heightPx) {
        std::lock_guard lock(stateMutex_);
        cols_ = cols;
        rows_ = rows;
        widthPx_ = widthPx;
        heightPx_ = heightPx;
        if (closed_ || closeRequested_) return;
        if (started_)
            transport_->resize(cols, rows, widthPx, heightPx);
        else
            resizePending_ = true;
    }

    void special(const std::string& id) {
        std::lock_guard lock(stateMutex_);
        if (started_ && !closed_ && !closeRequested_) transport_->sendSpecial(id);
    }

    /** User/app requested close. Idempotent; onClose follows (from the transport or the starter thread). */
    void requestClose() {
        closing_ = true;
        mgr_.cancelPrompts(connId_);
        std::lock_guard lock(stateMutex_);
        if (closed_ || closeRequested_) return;
        closeRequested_ = true;
        if (started_) transport_->close();
    }

    /** Worker thread: join the starter thread (it may still be returning from start()), then destroy the
     *  transport (joins its I/O threads). Afterwards nothing of this connection runs any more. */
    void reap() {
        if (starter_.joinable() && starter_.get_id() != std::this_thread::get_id()) starter_.join();
        std::shared_ptr<Transport> t;
        {
            std::lock_guard lock(stateMutex_);
            t = std::move(transport_);
        }
        t.reset();
        {
            std::lock_guard lock(inMutex_);
            ldisc_.reset();
        }
        {
            std::lock_guard lock(mgr_.mutex);
            reaped_ = true;
            --mgr_.unreaped;
        }
        mgr_.reapedCv.notify_all();
    }

    /** Caller holds ManagerCore::mutex. */
    bool reapedLocked() const { return reaped_; }

    /** Shutdown fallback for a transport that never reported onClose: make all callbacks no-ops. */
    void detach() { detached_ = true; }

    /** Flusher thread: emit the pending batch whose 8 ms window ended. */
    void flushScheduled() {
        std::lock_guard lock(outMutex_);
        flushScheduled_ = false;
        emitDataLocked();
    }

    /* ---- TransportListener ---- */

    void onData(std::string_view bytes) override {
        if (detached_ || bytes.empty()) return;
        std::string text = decoder_.decode(bytes);
        log_.output(bytes, text);
        appendOutput(std::move(text));
    }

    void onStatus(ConnStatus status, const std::string& message) override {
        if (detached_) return;
        {
            std::lock_guard lock(stateMutex_);
            if (closed_) return;
            status_ = status;
        }
        emitStatus(status, message, nullptr);
    }

    void onLog(LogLevel level, const std::string& message) override {
        if (detached_) return;
        if (level == LogLevel::Debug && session_.logging.mode != LogMode::SshDebug) return;
        EventLogEntry e{nowMs(), level, message};
        {
            std::lock_guard lock(stateMutex_);
            events_.push_back(e);
            while (events_.size() > kMaxEventLog) events_.pop_front();
            mgr_.bridge.emit("conn:onEventLog", Json::array({connId_, e}));
        }
        log_.event(message);
    }

    void onClose(const CloseInfo& info) override {
        if (detached_) return;
        {
            std::lock_guard lock(stateMutex_);
            if (closed_) return;  // contract says exactly once; be defensive anyway
            closed_ = true;
            status_ = info.error ? ConnStatus::Error : ConnStatus::Disconnected;
        }
        closing_ = true;
        mgr_.cancelPrompts(connId_);
        appendTail(decoder_.flush());
        emitStatus(info.error ? ConnStatus::Error : ConnStatus::Disconnected, info.reason, &info);
        log_.close();
        mgr_.connectionClosed(shared_from_this());
    }

    void onLdiscChanged(LdiscModes modes) override {
        if (detached_) return;
        std::lock_guard lock(inMutex_);
        if (ldisc_) ldisc_->setTransportModes(modes);
    }

    void onServerInfo(const std::string& info) override {
        if (detached_) return;
        ConnStatus status;
        {
            std::lock_guard lock(stateMutex_);
            if (closed_) return;
            serverInfo_ = info;
            status = status_;
        }
        // Make the information visible right away when we are already connected.
        if (status == ConnStatus::Connected) emitStatus(status, {}, nullptr);
    }

    void onLocalOutput(const std::string& utf8) override {
        if (detached_ || utf8.empty()) return;
        log_.localOutput(utf8);
        appendOutput(utf8);
    }

    /* ---- Prompter ---- */

    PromptResponse prompt(const PromptInput& input) override {
        PromptResponse cancelled;
        cancelled.cancelled = true;
        if (detached_) return cancelled;
        if (mgr_.bridge.dispatcher().isUiThread()) {
            applog::error("Prompter::prompt called on the UI thread; refusing to block it");
            return cancelled;
        }
        auto pp = std::make_shared<PendingPrompt>();
        pp->id = newUuid();
        pp->connId = connId_;
        {
            std::lock_guard lock(mgr_.mutex);
            if (closing_) return cancelled;
            mgr_.prompts.emplace(pp->id, pp);
        }
        Json req = input.json.is_object() ? input.json : Json::object();
        req["id"] = pp->id;
        req["connId"] = connId_;
        {
            // Flush output first so text that precedes the prompt (banner, "Access denied") is shown before it.
            std::lock_guard lock(outMutex_);
            emitDataLocked();
            mgr_.bridge.emit("prompts:onPrompt", Json::array({std::move(req)}));
        }

        PromptResponse result = cancelled;
        bool answered = false;
        {
            std::unique_lock lock(pp->m);
            answered = pp->cv.wait_for(lock, kPromptTimeout, [&] { return pp->done || pp->withdrawn; }) && pp->done &&
                       !pp->withdrawn;
            if (answered) result = pp->response;
        }
        {
            std::lock_guard lock(mgr_.mutex);
            mgr_.prompts.erase(pp->id);
        }
        if (!answered) {
            mgr_.bridge.emit("prompts:onCancel", Json::array({pp->id}));
            result = cancelled;
        }
        return result;
    }

private:
    void runStarter() {
        const SessionConfig& s = session_;
        if (!s.id.empty()) {
            Store& store = mgr_.store;
            const std::string id = s.id;
            try {
                if (store.getSession(id)) store.touchSession(id);
            } catch (const std::exception& e) {
                applog::warn(std::string("touchSession failed: ") + e.what());
            }
        }

        if (s.logging.mode != LogMode::Off && !closing_) {
            std::string error;
            if (!log_.open(this, error))
                onLog(LogLevel::Warn, "Failed to open session log: " + error);
            else if (log_.isOpen())
                onLog(LogLevel::Info, std::string("Writing session log (") + logModeLabel(s.logging.mode) +
                                          " mode) to file: " + narrow(log_.path().wstring()));
        }

        {
            std::lock_guard lock(stateMutex_);
            if (!closeRequested_) starting_ = true;
        }
        if (!starting_) {
            CloseInfo ci;
            ci.clean = true;
            ci.reason = "Connection closed";
            onClose(ci);
            return;
        }

        try {
            transport_->start();
        } catch (const std::exception& e) {
            CloseInfo ci;
            ci.error = true;
            ci.reason = e.what();
            onClose(ci);
            return;
        }

        std::lock_guard lock(stateMutex_);
        started_ = true;
        if (closed_) return;
        if (!preStart_.empty()) {
            transport_->write(std::move(preStart_));
            preStart_.clear();
        }
        if (resizePending_) {
            resizePending_ = false;
            transport_->resize(cols_, rows_, widthPx_, heightPx_);
        }
        if (closeRequested_) transport_->close();
    }

    void toRemote(const std::string& utf8) { transportWrite(encodeFromUtf8(session_.translation.charset, utf8)); }

    void toTerminal(const std::string& utf8) {
        log_.localOutput(utf8);
        appendOutput(utf8);
    }

    void transportWrite(std::string bytes) {
        if (bytes.empty()) return;
        std::lock_guard lock(stateMutex_);
        if (closed_ || closeRequested_) return;
        if (started_)
            transport_->write(std::move(bytes));
        else
            preStart_ += bytes;
    }

    /** Appends decoder leftovers at close time (emitted with the final status). */
    void appendTail(const std::string& text) {
        if (text.empty()) return;
        std::lock_guard lock(outMutex_);
        pending_ += text;
    }

    void appendOutput(std::string text) {
        if (text.empty()) return;
        std::lock_guard lock(outMutex_);
        if (finalEmitted_) return;
        pending_ += text;
        const bool uiThread = mgr_.bridge.dispatcher().isUiThread();
        const auto now = Clock::now();
        if (!uiThread && (pending_.size() >= kMaxBatchBytes || (!flushScheduled_ && now - lastFlush_ >= kFlushInterval))) {
            emitDataLocked();
            return;
        }
        if (!flushScheduled_) {
            flushScheduled_ = true;
            mgr_.scheduleFlush(shared_from_this(), uiThread ? now : std::max(now, lastFlush_ + kFlushInterval));
        }
    }

    void emitDataLocked() {
        if (pending_.empty()) return;
        std::string data;
        data.swap(pending_);
        mgr_.bridge.emit("conn:onData", Json::array({connId_, std::move(data)}));
        lastFlush_ = Clock::now();
    }

    void emitStatus(ConnStatus status, const std::string& message, const CloseInfo* close) {
        Json ev = {{"connId", connId_}, {"status", status}};
        if (!message.empty()) ev["message"] = message;
        if (close) {
            ev["clean"] = close->clean && !close->error;
            if (close->hasExitCode) ev["exitCode"] = close->exitCode;
            if (!close->exitSignal.empty()) ev["exitSignal"] = close->exitSignal;
        }
        {
            std::lock_guard lock(stateMutex_);
            if (!serverInfo_.empty()) ev["serverInfo"] = serverInfo_;
        }
        std::lock_guard lock(outMutex_);
        if (finalEmitted_) return;
        emitDataLocked();
        mgr_.bridge.emit("conn:onStatus", Json::array({std::move(ev)}));
        if (close) finalEmitted_ = true;
    }

    ManagerCore& mgr_;
    const std::string connId_;
    const SessionConfig session_;
    std::atomic<bool> detached_{false};
    std::atomic<bool> closing_{false};  // close requested or closed: no new prompts, input dropped
    bool reaped_ = false;               // guarded by ManagerCore::mutex

    // stateMutex_: lifecycle flags, transport calls, size, event log, server info.
    mutable std::mutex stateMutex_;
    std::shared_ptr<Transport> transport_;
    bool starting_ = false;
    bool started_ = false;
    bool closeRequested_ = false;
    bool closed_ = false;
    bool resizePending_ = false;
    std::string preStart_;
    int cols_, rows_, widthPx_, heightPx_;
    ConnStatus status_ = ConnStatus::Connecting;
    std::string serverInfo_;
    std::deque<EventLogEntry> events_;

    // outMutex_: output batching and all emissions of data/status for this connection.
    std::mutex outMutex_;
    std::string pending_;
    bool flushScheduled_ = false;
    bool finalEmitted_ = false;
    Clock::time_point lastFlush_{};

    // inMutex_: line discipline.
    std::mutex inMutex_;
    std::unique_ptr<LineDiscipline> ldisc_;

    Decoder decoder_;  // transport thread only
    SessionLog log_;   // internally synchronised
    std::thread starter_;
};

/* ------------------------------------------------------------------------------------------------------------ */
/* ManagerCore                                                                                                    */
/* ------------------------------------------------------------------------------------------------------------ */

ManagerCore::ManagerCore(Store& s, Bridge& b) : store(s), bridge(b) {
    flusher = std::thread([this] { flusherLoop(); });
    worker = std::thread([this] { workerLoop(); });
}

ManagerCore::~ManagerCore() {
    {
        std::lock_guard lock(flushMutex);
        flushStop = true;
    }
    flushCv.notify_all();
    if (flusher.joinable()) flusher.join();
    {
        std::lock_guard lock(workMutex);
        workStop = true;
    }
    workCv.notify_all();
    if (worker.joinable()) worker.join();
}

void ManagerCore::scheduleFlush(const std::shared_ptr<Connection>& c, Clock::time_point due) {
    {
        std::lock_guard lock(flushMutex);
        flushQueue.emplace_back(due, c);
    }
    flushCv.notify_one();
}

void ManagerCore::flusherLoop() {
    std::unique_lock lock(flushMutex);
    while (!flushStop) {
        if (flushQueue.empty()) {
            flushCv.wait(lock);
            continue;
        }
        const auto earliest =
            std::min_element(flushQueue.begin(), flushQueue.end(), [](const auto& a, const auto& b) { return a.first < b.first; })
                ->first;
        if (Clock::now() < earliest) {
            flushCv.wait_until(lock, earliest);
            continue;
        }
        const auto now = Clock::now();
        std::vector<std::shared_ptr<Connection>> due;
        auto it = std::partition(flushQueue.begin(), flushQueue.end(), [&](const auto& e) { return e.first > now; });
        for (auto d = it; d != flushQueue.end(); ++d)
            if (auto c = d->second.lock()) due.push_back(std::move(c));
        flushQueue.erase(it, flushQueue.end());
        lock.unlock();
        for (auto& c : due) c->flushScheduled();
        due.clear();
        lock.lock();
    }
}

void ManagerCore::post(std::function<void()> job) {
    {
        std::lock_guard lock(workMutex);
        work.push_back(std::move(job));
    }
    workCv.notify_one();
}

void ManagerCore::workerLoop() {
    std::unique_lock lock(workMutex);
    for (;;) {
        workCv.wait(lock, [this] { return workStop || !work.empty(); });
        if (work.empty()) {
            if (workStop) return;
            continue;
        }
        auto job = std::move(work.front());
        work.pop_front();
        lock.unlock();
        try {
            job();
        } catch (const std::exception& e) {
            applog::error(std::string("SessionManager worker job threw: ") + e.what());
        } catch (...) {
            applog::error("SessionManager worker job threw");
        }
        job = nullptr;
        lock.lock();
    }
}

void ManagerCore::connectionClosed(const std::shared_ptr<Connection>& c) {
    {
        std::lock_guard lock(mutex);
        auto it = conns.find(c->id());
        if (it != conns.end() && it->second == c) {
            closedOrder.push_back(c->id());
            // Forget the oldest closed connections (their event logs). One whose teardown is still pending is
            // kept until a later close so that closeAll() can still see it.
            while (closedOrder.size() > kMaxClosedRetained) {
                auto old = conns.find(closedOrder.front());
                if (old != conns.end()) {
                    if (!old->second->closed() || !old->second->reapedLocked()) break;
                    conns.erase(old);
                }
                closedOrder.pop_front();
            }
        }
    }
    post([c] { c->reap(); });
}

void ManagerCore::cancelPrompts(const std::string& connId) {
    std::lock_guard lock(mutex);
    for (auto& [id, pp] : prompts) {
        if (pp->connId != connId) continue;
        {
            std::lock_guard pl(pp->m);
            pp->withdrawn = true;
        }
        pp->cv.notify_all();
    }
}

std::shared_ptr<Connection> ManagerCore::find(const std::string& connId) const {
    std::lock_guard lock(mutex);
    auto it = conns.find(connId);
    return it == conns.end() ? nullptr : it->second;
}

std::shared_ptr<Connection> ManagerCore::findLive(const std::string& connId) const {
    auto c = find(connId);
    return (c && !c->closed()) ? c : nullptr;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* SessionManager                                                                                                 */
/* ------------------------------------------------------------------------------------------------------------ */

struct SessionManager::Impl : ManagerCore {
    using ManagerCore::ManagerCore;
};

SessionManager::SessionManager(Store& store, Bridge& bridge) : impl_(std::make_unique<Impl>(store, bridge)) {}

SessionManager::~SessionManager() {
    closeAll();
    // Anything still alive never reported onClose within the bounded wait: detach it so late callbacks are no-ops,
    // and leak it intentionally (destroying it could join a hung I/O thread or free memory still in use).
    std::vector<std::shared_ptr<Connection>> stuck;
    {
        std::lock_guard lock(impl_->mutex);
        for (auto& [id, c] : impl_->conns)
            if (!c->closed()) stuck.push_back(c);
        impl_->conns.clear();
    }
    if (!stuck.empty()) {
        applog::warn("SessionManager: " + std::to_string(stuck.size()) + " connection(s) did not close in time");
        auto* leaked = new std::vector<std::shared_ptr<Connection>>(std::move(stuck));
        for (auto& c : *leaked) c->detach();
    }
    impl_.reset();
}

void SessionManager::setTransportFactory(TransportFactoryFn factory) {
    std::lock_guard lock(impl_->mutex);
    impl_->factory = std::move(factory);
}

void SessionManager::open(const ConnOpenRequest& reqIn) {
    ConnOpenRequest req = reqIn;
    SessionConfig& s = req.session;
    if (trim(req.connId).empty()) throw std::invalid_argument("Connection id is required");
    if (s.protocol == Protocol::Serial) {
        if (trim(s.serial.line).empty()) throw std::invalid_argument("Serial line is required");
    } else {
        s.host = trim(s.host);
        if (s.host.empty()) throw std::invalid_argument("Host name is required");
        if (s.port == 0) s.port = defaultPort(s.protocol);
        if (s.port <= 0 || s.port > 65535) throw std::invalid_argument("Invalid port number");
    }
    if (req.cols <= 0) req.cols = 80;
    if (req.rows <= 0) req.rows = 24;

    TransportFactoryFn factory;
    {
        std::lock_guard lock(impl_->mutex);
        auto it = impl_->conns.find(req.connId);
        if (it != impl_->conns.end() && !it->second->closed())
            throw std::invalid_argument("Duplicate connection id: " + req.connId);
        factory = impl_->factory;
    }

    auto conn = std::make_shared<Connection>(*impl_, req);

    TransportContext ctx;
    ctx.connId = req.connId;
    ctx.session = s;
    ctx.cols = req.cols;
    ctx.rows = req.rows;
    ctx.widthPx = req.widthPx;
    ctx.heightPx = req.heightPx;
    ctx.oneTimePassword = req.password;
    ctx.listener = conn.get();
    ctx.prompter = conn.get();
    ctx.knownHosts = &impl_->store.knownHosts();
    ctx.credentials = &impl_->store.credentials();
    Store* store = &impl_->store;
    ctx.resolveSession = [store](const std::string& id) { return store->getSession(id); };

    std::unique_ptr<Transport> transport = factory ? factory(std::move(ctx)) : createTransport(std::move(ctx));
    if (!transport) throw std::invalid_argument("Unsupported connection type");

    {
        std::lock_guard lock(impl_->mutex);
        auto it = impl_->conns.find(req.connId);
        if (it != impl_->conns.end()) {
            if (!it->second->closed()) throw std::invalid_argument("Duplicate connection id: " + req.connId);
            impl_->closedOrder.erase(std::remove(impl_->closedOrder.begin(), impl_->closedOrder.end(), req.connId),
                                     impl_->closedOrder.end());
        }
        impl_->conns[req.connId] = conn;
        ++impl_->unreaped;
    }
    try {
        conn->launch(std::move(transport));
    } catch (...) {
        std::lock_guard lock(impl_->mutex);
        auto it = impl_->conns.find(req.connId);
        if (it != impl_->conns.end() && it->second == conn) impl_->conns.erase(it);
        --impl_->unreaped;
        throw;
    }
}

void SessionManager::write(const std::string& connId, const std::string& utf8, bool binary) {
    if (auto c = impl_->findLive(connId)) c->write(utf8, binary);
}

void SessionManager::resize(const std::string& connId, int cols, int rows, int widthPx, int heightPx) {
    if (cols <= 0 || rows <= 0) return;
    if (auto c = impl_->findLive(connId)) c->resize(cols, rows, std::max(0, widthPx), std::max(0, heightPx));
}

void SessionManager::close(const std::string& connId) {
    if (auto c = impl_->findLive(connId)) c->requestClose();
}

void SessionManager::special(const std::string& connId, const std::string& commandId) {
    if (auto c = impl_->findLive(connId)) c->special(commandId);
}

std::vector<SpecialCommandDef> SessionManager::specials(const std::string& connId) const {
    auto c = impl_->findLive(connId);
    return c ? c->specials() : std::vector<SpecialCommandDef>{};
}

std::vector<EventLogEntry> SessionManager::eventLog(const std::string& connId) const {
    auto c = impl_->find(connId);
    return c ? c->eventLog() : std::vector<EventLogEntry>{};
}

void SessionManager::respondPrompt(const std::string& promptId, const PromptResponse& response) {
    std::shared_ptr<PendingPrompt> pp;
    {
        std::lock_guard lock(impl_->mutex);
        auto it = impl_->prompts.find(promptId);
        if (it == impl_->prompts.end()) return;  // already answered / withdrawn
        pp = it->second;
    }
    {
        std::lock_guard lock(pp->m);
        if (pp->done || pp->withdrawn) return;
        pp->response = response;
        pp->done = true;
    }
    pp->cv.notify_all();
}

std::shared_ptr<Transport> SessionManager::transport(const std::string& connId) const {
    auto c = impl_->find(connId);
    return c ? c->liveTransport() : nullptr;
}

int SessionManager::activeCount() const {
    std::lock_guard lock(impl_->mutex);
    int n = 0;
    for (const auto& [id, c] : impl_->conns)
        if (!c->closed()) ++n;
    return n;
}

void SessionManager::closeAll() {
    std::vector<std::shared_ptr<Connection>> live;
    {
        std::lock_guard lock(impl_->mutex);
        for (const auto& [id, c] : impl_->conns)
            if (!c->closed()) live.push_back(c);
    }
    for (auto& c : live) c->requestClose();
    live.clear();
    // Wait (bounded) until every transport delivered onClose and was torn down by the worker.
    std::unique_lock lock(impl_->mutex);
    impl_->reapedCv.wait_for(lock, kCloseAllWait, [&] { return impl_->unreaped == 0; });
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Bridge handlers                                                                                                */
/* ------------------------------------------------------------------------------------------------------------ */

namespace {

ConnOpenRequest parseOpenRequest(const Json& j) {
    if (!j.is_object()) throw std::invalid_argument("conn:open expects a ConnOpenRequest object");
    ConnOpenRequest r;
    if (j.contains("connId") && j["connId"].is_string()) r.connId = j["connId"].get<std::string>();
    const Json session = j.contains("session") ? j["session"] : Json::object();
    if (!session.is_object()) throw std::invalid_argument("conn:open: session must be an object");
    r.session = sessionFromJson(Store::normalizeSessionJson(session, false));
    auto num = [&j](const char* k, int def) {
        return (j.contains(k) && j[k].is_number()) ? static_cast<int>(j[k].get<double>()) : def;
    };
    r.cols = num("cols", 80);
    r.rows = num("rows", 24);
    r.widthPx = num("widthPx", 0);
    r.heightPx = num("heightPx", 0);
    if (j.contains("password") && j["password"].is_string()) r.password = j["password"].get<std::string>();
    return r;
}

PromptResponse parsePromptResponse(const Json& j) {
    PromptResponse r;
    if (!j.is_object()) return r;  // cancelled
    if (j.contains("cancelled") && j["cancelled"].is_boolean()) r.cancelled = j["cancelled"].get<bool>();
    if (j.contains("values") && j["values"].is_array())
        for (const auto& v : j["values"]) r.values.push_back(v.is_string() ? v.get<std::string>() : std::string());
    if (j.contains("remember") && j["remember"].is_boolean()) r.remember = j["remember"].get<bool>();
    if (j.contains("choice") && j["choice"].is_string()) r.choice = j["choice"].get<std::string>();
    return r;
}

int argIntClamped(const Json& args, size_t i) {
    const int64_t v = argInt(args, i, 0);
    return static_cast<int>(std::clamp<int64_t>(v, 0, 100000));
}

}  // namespace

void registerConnHandlers(Bridge& bridge, SessionManager& sessions) {
    bridge.handle("conn:open", [&sessions](const Json& args, Bridge::Responder reply) {
        try {
            sessions.open(parseOpenRequest(jsonArg(args, 0)));
            reply.resolve();
        } catch (const std::exception& e) {
            reply.reject(e.what());
        }
    });
    bridge.on("conn:write", [&sessions](const Json& args) {
        sessions.write(argStr(args, 0), argStr(args, 1), argBool(args, 2, false));
    });
    bridge.on("conn:resize", [&sessions](const Json& args) {
        sessions.resize(argStr(args, 0), argIntClamped(args, 1), argIntClamped(args, 2), argIntClamped(args, 3),
                        argIntClamped(args, 4));
    });
    bridge.on("conn:close", [&sessions](const Json& args) { sessions.close(argStr(args, 0)); });
    bridge.on("conn:special", [&sessions](const Json& args) { sessions.special(argStr(args, 0), argStr(args, 1)); });
    bridge.handleSync("conn:specials", [&sessions](const Json& args) -> Json { return sessions.specials(argStr(args, 0)); });
    bridge.handleSync("conn:eventLog", [&sessions](const Json& args) -> Json { return sessions.eventLog(argStr(args, 0)); });
    bridge.on("prompts:respond", [&sessions](const Json& args) {
        sessions.respondPrompt(argStr(args, 0), parsePromptResponse(jsonArg(args, 1)));
    });
}

}  // namespace st
