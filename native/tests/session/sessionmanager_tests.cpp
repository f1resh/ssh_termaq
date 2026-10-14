// SessionManager with a fake Transport (injected factory) and a capturing Dispatcher/Bridge sink.
// The test thread plays the UI thread; the fake transport runs callbacks on its own I/O thread like real ones.
#include "TestServers.h"
#include "bridge/Bridge.h"
#include "core/Strings.h"
#include "session/LineDiscipline.h"
#include "session/Prompter.h"
#include "session/SessionManager.h"
#include "store/Store.h"
#include "transport/Transport.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <future>
#include <map>
#include <sstream>
#include <thread>

using namespace st;
using namespace std::chrono_literals;
using st::test::TempDir;

namespace {

/* ---- capture ---------------------------------------------------------------------------------------------- */

/** UI thread = the thread that created it. post() runs the job inline (serialised), like an always-pumping UI. */
struct CaptureDispatcher : Dispatcher {
    std::thread::id ui = std::this_thread::get_id();
    std::mutex m;
    void post(std::function<void()> fn) override {
        std::lock_guard lock(m);
        fn();
    }
    bool isUiThread() const override { return std::this_thread::get_id() == ui; }
};

struct Captured {
    std::mutex m;
    std::condition_variable cv;
    std::vector<Json> msgs;

    void push(const std::string& text) {
        Json j = Json::parse(text);
        {
            std::lock_guard lock(m);
            msgs.push_back(std::move(j));
        }
        cv.notify_all();
    }
    std::vector<Json> all() {
        std::lock_guard lock(m);
        return msgs;
    }
    template <class Pred>
    bool waitFor(Pred pred, std::chrono::milliseconds timeout = 5000ms) {
        std::unique_lock lock(m);
        return cv.wait_for(lock, timeout, [&] { return pred(msgs); });
    }
};

/** Events of one channel for one connection (args[0] == connId, or args[0].connId for status). */
std::vector<Json> eventsOf(const std::vector<Json>& msgs, const std::string& ch, const std::string& connId) {
    std::vector<Json> out;
    for (const auto& m : msgs) {
        if (m.value("t", "") != "evt" || m.value("ch", "") != ch) continue;
        const Json& a0 = m["args"][0];
        const bool match = a0.is_string() ? a0 == connId : (a0.is_object() && a0.value("connId", "") == connId);
        if (match) out.push_back(m["args"]);
    }
    return out;
}

std::string dataOf(const std::vector<Json>& msgs, const std::string& connId) {
    std::string s;
    for (const auto& a : eventsOf(msgs, "conn:onData", connId)) s += a[1].get<std::string>();
    return s;
}

std::vector<std::string> statusesOf(const std::vector<Json>& msgs, const std::string& connId) {
    std::vector<std::string> s;
    for (const auto& a : eventsOf(msgs, "conn:onStatus", connId)) s.push_back(a[0]["status"].get<std::string>());
    return s;
}

bool hasFinalStatus(const std::vector<Json>& msgs, const std::string& connId) {
    for (const auto& s : statusesOf(msgs, connId))
        if (s == "disconnected" || s == "error") return true;
    return false;
}

std::vector<Json> promptsOf(const std::vector<Json>& msgs) {
    std::vector<Json> out;
    for (const auto& m : msgs)
        if (m.value("ch", "") == "prompts:onPrompt") out.push_back(m["args"][0]);
    return out;
}

bool cancelledPrompt(const std::vector<Json>& msgs, const std::string& id) {
    for (const auto& m : msgs)
        if (m.value("ch", "") == "prompts:onCancel" && m["args"][0] == id) return true;
    return false;
}

/* ---- fake transport --------------------------------------------------------------------------------------- */

struct FakeIo {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::function<void()>> jobs;
    bool stop = false;

    // Recorded calls.
    std::string written;
    std::vector<std::array<int, 4>> resizes;
    std::vector<std::string> specialsSent;
    int closeCalls = 0;
    bool started = false;
    bool destroyed = false;

    // Context handed to the factory.
    TransportListener* listener = nullptr;
    Prompter* prompter = nullptr;
    SessionConfig session;
    std::string oneTimePassword;
    int cols = 0, rows = 0;
    bool hasStoreServices = false;

    // Behaviour.
    LdiscModes ldisc;
    bool throwOnStart = false;
    bool closeDelivered = false;  // I/O thread only

    void post(std::function<void()> f) {
        {
            std::lock_guard lock(m);
            jobs.push_back(std::move(f));
        }
        cv.notify_all();
    }
    /** Runs f on the transport thread and waits for it. */
    bool run(std::function<void()> f) {
        auto done = std::make_shared<std::promise<void>>();
        auto fut = done->get_future();
        post([f = std::move(f), done] {
            f();
            done->set_value();
        });
        return fut.wait_for(5s) == std::future_status::ready;
    }
    template <class Pred>
    bool waitFor(Pred pred, std::chrono::milliseconds timeout = 5000ms) {
        std::unique_lock lock(m);
        return cv.wait_for(lock, timeout, pred);
    }
    /** I/O thread: deliver onClose once. */
    void closeWith(const CloseInfo& ci) {
        if (closeDelivered) return;
        closeDelivered = true;
        listener->onClose(ci);
    }
};

class FakeTransport final : public Transport {
public:
    FakeTransport(TransportContext ctx, std::shared_ptr<FakeIo> io) : ctx_(std::move(ctx)), io_(std::move(io)) {
        std::lock_guard lock(io_->m);
        io_->listener = ctx_.listener;
        io_->prompter = ctx_.prompter;
        io_->session = ctx_.session;
        io_->oneTimePassword = ctx_.oneTimePassword;
        io_->cols = ctx_.cols;
        io_->rows = ctx_.rows;
        io_->hasStoreServices = ctx_.knownHosts && ctx_.credentials && ctx_.resolveSession;
    }
    ~FakeTransport() override {
        {
            std::lock_guard lock(io_->m);
            io_->stop = true;
        }
        io_->cv.notify_all();
        if (thread_.joinable()) thread_.join();
        {
            std::lock_guard lock(io_->m);
            io_->destroyed = true;
        }
        io_->cv.notify_all();
    }
    Protocol protocol() const override { return ctx_.session.protocol; }
    void start() override {
        {
            std::lock_guard lock(io_->m);
            if (io_->throwOnStart) throw std::runtime_error("fake start failure");
            io_->started = true;
        }
        io_->cv.notify_all();
        thread_ = std::thread([this] { loop(); });
    }
    void write(std::string bytes) override {
        {
            std::lock_guard lock(io_->m);
            io_->written += bytes;
        }
        io_->cv.notify_all();
    }
    void resize(int cols, int rows, int widthPx, int heightPx) override {
        {
            std::lock_guard lock(io_->m);
            io_->resizes.push_back({cols, rows, widthPx, heightPx});
        }
        io_->cv.notify_all();
    }
    std::vector<SpecialCommandDef> specials() const override { return {{"break", "Break", ""}, {"eof", "EOF", ""}}; }
    void sendSpecial(const std::string& id) override {
        {
            std::lock_guard lock(io_->m);
            io_->specialsSent.push_back(id);
        }
        io_->cv.notify_all();
    }
    void close() override {
        {
            std::lock_guard lock(io_->m);
            ++io_->closeCalls;
        }
        io_->cv.notify_all();
        auto io = io_;
        io_->post([io] {
            CloseInfo ci;
            ci.clean = true;
            ci.reason = "Closed by user";
            io->closeWith(ci);
        });
    }
    LdiscModes defaultLdisc() const override { return io_->ldisc; }

private:
    void loop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(io_->m);
                io_->cv.wait(lock, [&] { return io_->stop || !io_->jobs.empty(); });
                if (io_->jobs.empty()) return;
                job = std::move(io_->jobs.front());
                io_->jobs.pop_front();
            }
            job();
        }
    }

    TransportContext ctx_;
    std::shared_ptr<FakeIo> io_;
    std::thread thread_;
};

/* ---- fixture ---------------------------------------------------------------------------------------------- */

struct Fixture {
    TempDir dir;
    Store store{dir.path()};
    CaptureDispatcher dispatcher;
    Bridge bridge{dispatcher};
    Captured cap;
    std::mutex fm;
    std::map<std::string, std::shared_ptr<FakeIo>> fakes;
    std::function<void(FakeIo&)> configure;  // applied to each new fake before the transport is built
    bool factoryThrows = false;
    std::unique_ptr<SessionManager> mgr;

    Fixture() {
        store.load();
        bridge.setSink([this](const std::string& s) { cap.push(s); });
        mgr = std::make_unique<SessionManager>(store, bridge);
        mgr->setTransportFactory([this](TransportContext ctx) -> std::unique_ptr<Transport> {
            if (factoryThrows) throw std::invalid_argument("unsupported in test");
            auto io = std::make_shared<FakeIo>();
            if (configure) configure(*io);
            {
                std::lock_guard lock(fm);
                fakes[ctx.connId] = io;
            }
            return std::make_unique<FakeTransport>(std::move(ctx), io);
        });
    }
    ~Fixture() { mgr.reset(); }

    std::shared_ptr<FakeIo> fake(const std::string& connId) {
        std::lock_guard lock(fm);
        auto it = fakes.find(connId);
        return it == fakes.end() ? nullptr : it->second;
    }

    static ConnOpenRequest request(const std::string& connId, Protocol p = Protocol::Raw) {
        ConnOpenRequest r;
        r.connId = connId;
        r.session.protocol = p;
        r.session.host = "127.0.0.1";
        r.session.port = 2323;
        r.session.name = "fake";
        r.cols = 100;
        r.rows = 30;
        return r;
    }

    /** Opens and waits until the fake transport was started. */
    std::shared_ptr<FakeIo> openStarted(const ConnOpenRequest& r) {
        mgr->open(r);
        auto io = fake(r.connId);
        REQUIRE(io);
        REQUIRE(io->waitFor([&] { return io->started; }));
        return io;
    }

    bool waitClosed(const std::string& connId) {
        return cap.waitFor([&](const std::vector<Json>& m) { return hasFinalStatus(m, connId); });
    }
};

bool ldiscEchoImplemented() {
    bool echoed = false;
    LineDiscipline l(TriState::On, TriState::Off, LdiscModes{},
                     LineDiscipline::Sinks{[](const std::string&) {}, [&](const std::string&) { echoed = true; }});
    l.input("x");
    return echoed;
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

/* ------------------------------------------------------------------------------------------------------------ */

TEST_CASE("session manager: open, status, data and remote close in order", "[manager]") {
    Fixture f;
    auto req = Fixture::request("p1#1");
    req.password = "one-time";
    auto io = f.openStarted(req);
    CHECK(io->cols == 100);
    CHECK(io->rows == 30);
    CHECK(io->oneTimePassword == "one-time");
    CHECK(io->hasStoreServices);
    CHECK(f.mgr->activeCount() == 1);
    CHECK(f.mgr->transport("p1#1") != nullptr);

    REQUIRE(io->run([&] {
        io->listener->onStatus(ConnStatus::Connecting, "Connecting to 127.0.0.1 port 2323");
        io->listener->onServerInfo("fake server");
        io->listener->onStatus(ConnStatus::Connected, "");
        io->listener->onData("hello ");
        io->listener->onData("w\xC3");  // UTF-8 sequence split across chunks
        io->listener->onData("\xA9rld");
    }));
    REQUIRE(f.cap.waitFor([](const std::vector<Json>& m) { return dataOf(m, "p1#1") == "hello w\xC3\xA9rld"; }));

    REQUIRE(io->run([&] {
        io->listener->onData("bye");
        CloseInfo ci;
        ci.clean = true;
        ci.hasExitCode = true;
        ci.exitCode = 3;
        ci.reason = "Remote side closed the connection";
        io->closeWith(ci);
    }));
    REQUIRE(f.waitClosed("p1#1"));

    const auto msgs = f.cap.all();
    CHECK(statusesOf(msgs, "p1#1") == std::vector<std::string>{"connecting", "connected", "disconnected"});
    const auto statuses = eventsOf(msgs, "conn:onStatus", "p1#1");
    CHECK(statuses[1][0]["serverInfo"] == "fake server");
    const Json& last = statuses.back()[0];
    CHECK(last["clean"] == true);
    CHECK(last["exitCode"] == 3);
    CHECK(last["message"] == "Remote side closed the connection");

    // All data precedes the final status, nothing follows it.
    bool afterFinal = false;
    std::string before;
    for (const auto& m : msgs) {
        if (m["ch"] == "conn:onStatus" && m["args"][0]["status"] == "disconnected") afterFinal = true;
        if (m["ch"] == "conn:onData") {
            CHECK_FALSE(afterFinal);
            before += m["args"][1].get<std::string>();
        }
    }
    CHECK(before == "hello w\xC3\xA9rldbye");

    CHECK(f.mgr->activeCount() == 0);
    CHECK(f.mgr->transport("p1#1") == nullptr);
    CHECK(io->waitFor([&] { return io->destroyed; }));  // reaped off the callback thread
}

TEST_CASE("session manager: output is batched and ordered", "[manager]") {
    Fixture f;
    auto io = f.openStarted(Fixture::request("b#1"));
    std::string expected;
    REQUIRE(io->run([&] {
        for (int i = 0; i < 300; ++i) {
            std::string chunk = "<" + std::to_string(i) + ">" + std::string(1000, static_cast<char>('a' + i % 26));
            expected += chunk;
            io->listener->onData(chunk);
        }
    }));
    REQUIRE(f.cap.waitFor([&](const std::vector<Json>& m) { return dataOf(m, "b#1").size() == expected.size(); }));
    const auto msgs = f.cap.all();
    CHECK(dataOf(msgs, "b#1") == expected);
    const auto events = eventsOf(msgs, "conn:onData", "b#1");
    CHECK(events.size() < 60);  // 300 chunks coalesced (64 KiB / 8 ms batches)
    for (const auto& e : events) CHECK(e[1].get<std::string>().size() <= 64 * 1024 + 1100);

    // A single huge chunk is delivered too.
    REQUIRE(io->run([&] { io->listener->onData(std::string(200000, 'z')); }));
    REQUIRE(f.cap.waitFor([&](const std::vector<Json>& m) { return dataOf(m, "b#1").size() == expected.size() + 200000; }));
}

TEST_CASE("session manager: input is charset-encoded, binary bypasses it, pre-start input is queued", "[manager]") {
    Fixture f;
    auto req = Fixture::request("in#1");
    req.session.translation.charset = "KOI8-R";
    auto io = f.openStarted(req);

    f.mgr->write("in#1", "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82", false);  // Привет
    CHECK(io->waitFor([&] { return io->written == "\xF0\xD2\xC9\xD7\xC5\xD4"; }));
    f.mgr->write("in#1", "\xC3\xBF\xC2\x80" "A\xC4\x80", true);  // binary string: U+00FF U+0080 'A' (U+0100 dropped)
    CHECK(io->waitFor([&] { return io->written == "\xF0\xD2\xC9\xD7\xC5\xD4\xFF\x80" "A"; }));

    // Remote output is decoded from KOI8-R.
    REQUIRE(io->run([&] { io->listener->onData("\xF0\xD2\xC9\xD7\xC5\xD4"); }));
    REQUIRE(f.cap.waitFor([](const std::vector<Json>& m) {
        return dataOf(m, "in#1") == "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";
    }));

    f.mgr->resize("in#1", 120, 40, 960, 640);
    f.mgr->special("in#1", "break");
    CHECK(io->waitFor([&] { return !io->resizes.empty() && !io->specialsSent.empty(); }));
    CHECK(io->resizes.back() == std::array<int, 4>{120, 40, 960, 640});
    CHECK(io->specialsSent.back() == "break");
    const auto specials = f.mgr->specials("in#1");
    REQUIRE(specials.size() == 2);
    CHECK(specials[0].id == "break");
    CHECK(f.mgr->specials("unknown").empty());
}

TEST_CASE("session manager: input and resize made before start are replayed after it", "[manager]") {
    Fixture f;
    // An existing log file with ifExists=ask makes the starter thread prompt before Transport::start().
    const auto logDir = f.store.logDirectory();
    std::filesystem::create_directories(logDir);
    { std::ofstream(logDir / "ask.log") << "old"; }
    auto req = Fixture::request("pre#1");
    req.session.logging.mode = LogMode::All;
    req.session.logging.path = "ask.log";
    req.session.logging.ifExists = LogIfExists::Ask;
    req.session.logging.header = false;
    f.mgr->open(req);
    REQUIRE(f.cap.waitFor([](const std::vector<Json>& m) { return !promptsOf(m).empty(); }));
    const Json prompt = promptsOf(f.cap.all()).front();
    CHECK(prompt["kind"] == "confirm");
    CHECK(prompt["connId"] == "pre#1");
    auto io = f.fake("pre#1");
    REQUIRE(io);
    CHECK_FALSE(io->started);

    f.mgr->write("pre#1", "typed early", false);
    f.mgr->resize("pre#1", 90, 20, 0, 0);
    PromptResponse r;
    r.cancelled = false;
    r.choice = "Append";
    f.mgr->respondPrompt(prompt["id"].get<std::string>(), r);

    REQUIRE(io->waitFor([&] { return io->started && io->written == "typed early" && !io->resizes.empty(); }));
    CHECK(io->resizes.back() == std::array<int, 4>{90, 20, 0, 0});
    REQUIRE(io->run([&] { io->listener->onData("new"); }));
    f.mgr->close("pre#1");
    REQUIRE(f.waitClosed("pre#1"));
    REQUIRE(io->waitFor([&] { return io->destroyed; }));
    CHECK(slurp(logDir / "ask.log") == "oldnew");
    CHECK_FALSE(cancelledPrompt(f.cap.all(), prompt["id"].get<std::string>()));
}

TEST_CASE("session manager: local echo through the line discipline", "[manager]") {
    Fixture f;
    f.configure = [](FakeIo& io) { io.ldisc = LdiscModes{true, false}; };  // transport asks for local echo
    auto io = f.openStarted(Fixture::request("echo#1"));
    f.mgr->write("echo#1", "ls", false);
    CHECK(io->waitFor([&] { return io->written == "ls"; }));
    if (ldiscEchoImplemented()) {
        // Echo produced on the UI thread is still delivered (deferred to the flusher thread).
        CHECK(f.cap.waitFor([](const std::vector<Json>& m) { return dataOf(m, "echo#1") == "ls"; }));
    } else {
        WARN("LineDiscipline is still a pass-through stub (net module); local echo not verified");
    }
}

TEST_CASE("session manager: prompt round trip answered from another thread", "[manager]") {
    Fixture f;
    auto io = f.openStarted(Fixture::request("pr#1"));
    std::promise<PromptResponse> result;
    io->post([&] { result.set_value(io->prompter->prompt(prompts::password("SSH password", "", "user", true))); });

    REQUIRE(f.cap.waitFor([](const std::vector<Json>& m) { return !promptsOf(m).empty(); }));
    const Json p = promptsOf(f.cap.all()).front();
    CHECK(p["kind"] == "password");
    CHECK(p["connId"] == "pr#1");
    const std::string id = p["id"].get<std::string>();
    REQUIRE_FALSE(id.empty());

    std::thread responder([&] {
        PromptResponse r;
        r.cancelled = false;
        r.values = {"secret"};
        r.remember = true;
        f.mgr->respondPrompt(id, r);
    });
    responder.join();
    auto fut = result.get_future();
    REQUIRE(fut.wait_for(5s) == std::future_status::ready);
    const PromptResponse got = fut.get();
    CHECK_FALSE(got.cancelled);
    REQUIRE(got.values.size() == 1);
    CHECK(got.values[0] == "secret");
    CHECK(got.remember);
    CHECK_FALSE(cancelledPrompt(f.cap.all(), id));
    f.mgr->respondPrompt(id, PromptResponse{});  // late/duplicate answers are ignored
    f.mgr->respondPrompt("no-such-prompt", PromptResponse{});
}

TEST_CASE("session manager: pending prompt is cancelled when the connection closes", "[manager]") {
    Fixture f;
    auto io = f.openStarted(Fixture::request("pc#1"));
    std::promise<PromptResponse> result;
    io->post([&] { result.set_value(io->prompter->prompt(prompts::username("Login"))); });
    REQUIRE(f.cap.waitFor([](const std::vector<Json>& m) { return !promptsOf(m).empty(); }));
    const std::string id = promptsOf(f.cap.all()).front()["id"].get<std::string>();

    f.mgr->close("pc#1");
    auto fut = result.get_future();
    REQUIRE(fut.wait_for(5s) == std::future_status::ready);
    CHECK(fut.get().cancelled);
    REQUIRE(f.waitClosed("pc#1"));
    CHECK(cancelledPrompt(f.cap.all(), id));

    // Prompts after close are refused immediately.
    std::promise<PromptResponse> late;
    // (the transport thread is gone after reaping; call from this non-UI helper thread instead)
    std::thread([&] { late.set_value(io->prompter->prompt(prompts::username("Login"))); }).join();
    CHECK(late.get_future().get().cancelled);
}

TEST_CASE("session manager: close is idempotent", "[manager]") {
    Fixture f;
    auto io = f.openStarted(Fixture::request("c#1"));
    f.mgr->close("c#1");
    f.mgr->close("c#1");
    REQUIRE(f.waitClosed("c#1"));
    f.mgr->close("c#1");
    f.mgr->close("never-opened");
    f.mgr->write("c#1", "ignored", false);
    REQUIRE(io->waitFor([&] { return io->destroyed; }));
    CHECK(io->closeCalls == 1);
    CHECK(io->written.empty());
    const auto statuses = statusesOf(f.cap.all(), "c#1");
    REQUIRE(statuses.size() == 1);
    CHECK(statuses[0] == "disconnected");
    CHECK(eventsOf(f.cap.all(), "conn:onStatus", "c#1")[0][0]["clean"] == true);
}

TEST_CASE("session manager: event log is kept after close, debug entries only in ssh-debug mode", "[manager]") {
    Fixture f;
    auto io = f.openStarted(Fixture::request("ev#1"));
    REQUIRE(io->run([&] {
        io->listener->onLog(LogLevel::Info, "Connected to fake");
        io->listener->onLog(LogLevel::Debug, "debug hidden");
        io->listener->onLog(LogLevel::Warn, "careful");
    }));
    REQUIRE(f.cap.waitFor([](const std::vector<Json>& m) { return eventsOf(m, "conn:onEventLog", "ev#1").size() == 2; }));
    CHECK(eventsOf(f.cap.all(), "conn:onEventLog", "ev#1")[0][1]["message"] == "Connected to fake");
    f.mgr->close("ev#1");
    REQUIRE(f.waitClosed("ev#1"));
    REQUIRE(io->waitFor([&] { return io->destroyed; }));
    const auto log = f.mgr->eventLog("ev#1");
    REQUIRE(log.size() == 2);
    CHECK(log[0].message == "Connected to fake");
    CHECK(log[0].level == LogLevel::Info);
    CHECK(log[0].time > 0);
    CHECK(log[1].level == LogLevel::Warn);
    CHECK(f.mgr->eventLog("unknown").empty());

    // ssh-debug: debug entries are kept and written to the session log with the raw output.
    auto req = Fixture::request("ev#2");
    req.session.logging.mode = LogMode::SshDebug;
    req.session.logging.path = "debug-&P.log";
    req.session.logging.header = false;
    auto io2 = f.openStarted(req);
    REQUIRE(io2->run([&] {
        io2->listener->onLog(LogLevel::Debug, "kex details");
        io2->listener->onData("raw");
    }));
    f.mgr->close("ev#2");
    REQUIRE(f.waitClosed("ev#2"));
    REQUIRE(io2->waitFor([&] { return io2->destroyed; }));
    bool sawDebug = false, sawLogInfo = false;
    for (const auto& e : f.mgr->eventLog("ev#2")) {
        sawDebug |= e.message == "kex details" && e.level == LogLevel::Debug;
        sawLogInfo |= startsWith(e.message, "Writing session log");
    }
    CHECK(sawDebug);
    CHECK(sawLogInfo);
    const std::string content = slurp(f.store.logDirectory() / "debug-2323.log");
    CHECK(content.find("Event Log: kex details\r\n") != std::string::npos);
    CHECK(content.find("raw") != std::string::npos);
}

TEST_CASE("session manager: event log ring buffer is bounded", "[manager]") {
    Fixture f;
    auto io = f.openStarted(Fixture::request("ring#1"));
    REQUIRE(io->run([&] {
        for (int i = 0; i < 2100; ++i) io->listener->onLog(LogLevel::Info, "line " + std::to_string(i));
    }));
    const auto log = f.mgr->eventLog("ring#1");
    REQUIRE(log.size() == 2000);
    CHECK(log.front().message == "line 100");
    CHECK(log.back().message == "line 2099");
}

TEST_CASE("session manager: validation and duplicate connection ids", "[manager]") {
    Fixture f;
    auto io = f.openStarted(Fixture::request("dup#1"));
    CHECK_THROWS_AS(f.mgr->open(Fixture::request("dup#1")), std::invalid_argument);

    auto noHost = Fixture::request("v#1");
    noHost.session.host = "  ";
    CHECK_THROWS_AS(f.mgr->open(noHost), std::invalid_argument);
    CHECK_THROWS_AS(f.mgr->open(Fixture::request("")), std::invalid_argument);
    auto rawNoPort = Fixture::request("v#2");
    rawNoPort.session.port = 0;
    CHECK_THROWS_AS(f.mgr->open(rawNoPort), std::invalid_argument);
    auto badPort = Fixture::request("v#3", Protocol::Ssh);
    badPort.session.port = 70000;
    CHECK_THROWS_AS(f.mgr->open(badPort), std::invalid_argument);
    auto serial = Fixture::request("v#4", Protocol::Serial);
    serial.session.host.clear();
    serial.session.serial.line.clear();
    CHECK_THROWS_AS(f.mgr->open(serial), std::invalid_argument);
    f.factoryThrows = true;
    CHECK_THROWS_AS(f.mgr->open(Fixture::request("v#5")), std::invalid_argument);
    f.factoryThrows = false;
    CHECK(f.mgr->activeCount() == 1);

    // Serial needs no host; ssh port 0 means the default port.
    serial.session.serial.line = "COM9";
    auto sio = f.openStarted(serial);
    auto sshDefault = Fixture::request("v#6", Protocol::Ssh);
    sshDefault.session.port = 0;
    auto dio = f.openStarted(sshDefault);
    CHECK(dio->session.port == 22);

    // After close, the id may be reused (UI restarts bump the generation, but be lenient).
    f.mgr->close("dup#1");
    REQUIRE(f.waitClosed("dup#1"));
    auto io2 = f.openStarted(Fixture::request("dup#1"));
    CHECK(io2 != io);
    CHECK(f.mgr->activeCount() == 3);
}

TEST_CASE("session manager: start failure and saved session touch", "[manager]") {
    Fixture f;
    f.configure = [](FakeIo& io) { io.throwOnStart = true; };
    f.mgr->open(Fixture::request("sf#1"));
    REQUIRE(f.waitClosed("sf#1"));
    const auto last = eventsOf(f.cap.all(), "conn:onStatus", "sf#1").back()[0];
    CHECK(last["status"] == "error");
    CHECK(last["clean"] == false);
    CHECK(last["message"] == "fake start failure");
    CHECK(f.mgr->activeCount() == 0);

    f.configure = nullptr;
    SessionConfig saved = f.store.saveSession(Fixture::request("x").session);
    REQUIRE_FALSE(saved.id.empty());
    REQUIRE(f.store.getSession(saved.id)->lastUsedAt == 0);
    auto req = Fixture::request("t#1");
    req.session = saved;
    f.openStarted(req);
    const auto touched = f.store.getSession(saved.id);
    REQUIRE(touched);
    CHECK(touched->lastUsedAt > 0);
}

TEST_CASE("session manager: closeAll tears everything down", "[manager]") {
    Fixture f;
    std::vector<std::shared_ptr<FakeIo>> ios;
    for (int i = 0; i < 4; ++i) ios.push_back(f.openStarted(Fixture::request("all#" + std::to_string(i))));
    // One of them is blocked in a prompt: closeAll must withdraw it.
    std::promise<PromptResponse> blocked;
    ios[1]->post([&] { blocked.set_value(ios[1]->prompter->prompt(prompts::username("Login"))); });
    REQUIRE(f.cap.waitFor([](const std::vector<Json>& m) { return !promptsOf(m).empty(); }));
    CHECK(f.mgr->activeCount() == 4);
    f.mgr->closeAll();
    CHECK(f.mgr->activeCount() == 0);
    for (auto& io : ios) {
        std::lock_guard lock(io->m);
        CHECK(io->destroyed);
    }
    CHECK(blocked.get_future().get().cancelled);
}

TEST_CASE("session manager: bridge handlers", "[manager]") {
    Fixture f;
    registerConnHandlers(f.bridge, *f.mgr);
    for (const char* ch : {"conn:open", "conn:write", "conn:resize", "conn:close", "conn:special", "conn:specials",
                           "conn:eventLog", "prompts:respond"})
        CHECK(f.bridge.hasHandler(ch));

    auto response = [&](int id) -> Json {
        Json found;
        f.cap.waitFor([&](const std::vector<Json>& m) {
            for (const auto& j : m)
                if (j.value("t", "") == "res" && j.value("id", -1) == id) {
                    found = j;
                    return true;
                }
            return false;
        });
        return found;
    };

    Json req = {{"connId", "br#1"},
                {"session", {{"protocol", "raw"}, {"host", "127.0.0.1"}, {"port", 2323}, {"name", "bridge"}}},
                {"cols", 81},
                {"rows", 25},
                {"widthPx", 0},
                {"heightPx", 0}};
    f.bridge.onWebMessage(Json({{"t", "req"}, {"id", 1}, {"ch", "conn:open"}, {"args", Json::array({req})}}).dump());
    CHECK(response(1)["ok"] == true);
    auto io = f.fake("br#1");
    REQUIRE(io);
    REQUIRE(io->waitFor([&] { return io->started; }));
    CHECK(io->cols == 81);

    // Duplicate -> rejected with a message.
    f.bridge.onWebMessage(Json({{"t", "req"}, {"id", 2}, {"ch", "conn:open"}, {"args", Json::array({req})}}).dump());
    const Json dup = response(2);
    CHECK(dup["ok"] == false);
    CHECK(dup["error"].get<std::string>().find("Duplicate") != std::string::npos);

    f.bridge.onWebMessage(R"({"t":"send","ch":"conn:write","args":["br#1","hi"]})");
    f.bridge.onWebMessage(R"({"t":"send","ch":"conn:write","args":["br#1","ÿ",true]})");
    f.bridge.onWebMessage(R"({"t":"send","ch":"conn:resize","args":["br#1",132,43,1056,688]})");
    f.bridge.onWebMessage(R"({"t":"send","ch":"conn:special","args":["br#1","eof"]})");
    CHECK(io->waitFor([&] { return io->written == "hi\xFF" && !io->resizes.empty() && !io->specialsSent.empty(); }));
    CHECK(io->resizes.back() == std::array<int, 4>{132, 43, 1056, 688});
    CHECK(io->specialsSent.back() == "eof");

    f.bridge.onWebMessage(R"({"t":"req","id":3,"ch":"conn:specials","args":["br#1"]})");
    const Json sp = response(3);
    REQUIRE(sp["ok"] == true);
    CHECK(sp["result"].size() == 2);
    CHECK(sp["result"][0]["id"] == "break");

    // Prompt answered through prompts:respond.
    std::promise<PromptResponse> result;
    io->post([&] { result.set_value(io->prompter->prompt(prompts::username("Login"))); });
    REQUIRE(f.cap.waitFor([](const std::vector<Json>& m) { return !promptsOf(m).empty(); }));
    const std::string pid = promptsOf(f.cap.all()).front()["id"].get<std::string>();
    f.bridge.onWebMessage(Json({{"t", "send"},
                                {"ch", "prompts:respond"},
                                {"args", Json::array({pid, {{"cancelled", false}, {"values", {"alice"}}}})}})
                              .dump());
    auto fut = result.get_future();
    REQUIRE(fut.wait_for(5s) == std::future_status::ready);
    const auto got = fut.get();
    CHECK_FALSE(got.cancelled);
    REQUIRE(got.values.size() == 1);
    CHECK(got.values[0] == "alice");

    REQUIRE(io->run([&] { io->listener->onLog(LogLevel::Info, "entry"); }));
    f.bridge.onWebMessage(R"({"t":"send","ch":"conn:close","args":["br#1"]})");
    REQUIRE(f.waitClosed("br#1"));
    f.bridge.onWebMessage(R"({"t":"req","id":4,"ch":"conn:eventLog","args":["br#1"]})");
    const Json el = response(4);
    REQUIRE(el["ok"] == true);
    REQUIRE(el["result"].size() == 1);
    CHECK(el["result"][0]["message"] == "entry");
    CHECK(el["result"][0]["level"] == "info");

    // Invalid request -> rejected.
    f.bridge.onWebMessage(R"({"t":"req","id":5,"ch":"conn:open","args":[{"connId":"bad#1","session":{"protocol":"ssh","host":""}}]})");
    CHECK(response(5)["ok"] == false);
}
