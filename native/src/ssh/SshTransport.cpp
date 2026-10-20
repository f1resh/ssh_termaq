// SSH transport on libssh: one I/O thread per connection running an ssh_event loop.
//
// Thread model: everything libssh-related happens on the I/O thread. Other threads queue writes, resizes,
// specials and posted tasks under `mutex_` and wake the loop through a socket pair registered in the event.
// Forwarded connections (local/dynamic/remote/X11) are Relay objects whose sockets are registered in the same
// event; channel data arrives through channel callbacks. Agent-forwarding channels answer through AgentForwarder.
#include "ssh/SshTransport.h"

#include "core/Crypto.h"
#include "core/Log.h"
#include "core/Strings.h"
#include "net/Proxy.h"
#include "net/Socket.h"
#include "ssh/SshAgent.h"
#include "ssh/SshClient.h"
#include "ssh/SshJump.h"
#include "ssh/SshUtil.h"
#include "ssh/X11Forward.h"
#include "platform/sys/Env.h"
#include "platform/sys/Fs.h"

#include <libssh/callbacks.h>
#include <libssh/server.h>  // ssh_send_keepalive

#include <algorithm>
#include <chrono>
#include <deque>
#include <list>
#include <mutex>
#include <thread>

namespace st {

using namespace ssh;

namespace {

using Clock = std::chrono::steady_clock;

const std::vector<SpecialCommandDef> kSpecials = {
    {"break", "Break", ""},
    {"eof", "EOF", ""},
    {"ignore", "IGNORE message", ""},
    {"signal:INT", "SIGINT (Interrupt)", "Signals"},
    {"signal:TERM", "SIGTERM (Terminate)", "Signals"},
    {"signal:KILL", "SIGKILL (Kill)", "Signals"},
    {"signal:HUP", "SIGHUP (Hangup)", "Signals"},
    {"signal:QUIT", "SIGQUIT (Quit)", "Signals"},
    {"signal:ABRT", "SIGABRT (Abort)", "Signals"},
    {"signal:ALRM", "SIGALRM (Alarm clock)", "Signals"},
    {"signal:PIPE", "SIGPIPE (Broken pipe)", "Signals"},
    {"signal:USR1", "SIGUSR1 (User 1)", "Signals"},
    {"signal:USR2", "SIGUSR2 (User 2)", "Signals"},
};

std::string forwardBind(const PortForward& f, bool acceptAll) {
    if (!f.bindAddress.empty()) return f.bindAddress;
    return acceptAll ? "*" : "";
}

std::string describe(const PortForward& f) {
    std::string bind = f.bindAddress.empty() ? "" : f.bindAddress + ":";
    switch (f.type) {
        case ForwardType::Local:
            return "local port " + bind + std::to_string(f.sourcePort) + " -> " + f.destHost + ":" + std::to_string(f.destPort);
        case ForwardType::Remote:
            return "remote port " + bind + std::to_string(f.sourcePort) + " -> " + f.destHost + ":" + std::to_string(f.destPort);
        case ForwardType::Dynamic: return "local port " + bind + std::to_string(f.sourcePort) + " (SOCKS dynamic forwarding)";
    }
    return {};
}

class SshTransportImpl;

/** Bytes a relay holds per direction; beyond it the source waits (the socket is not read, the SSH window stays shut). */
constexpr size_t kRelayBuffer = 512 * 1024;

/** One forwarded TCP connection: channel <-> socket. */
struct Relay {
    SshTransportImpl* owner = nullptr;
    ssh_channel channel = nullptr;
    sys::SocketHandle sock = sys::kInvalidSocket;
    ssh_channel_callbacks_struct cb{};
    std::string toSock;   // channel data waiting for the socket
    std::string toChan;   // socket data waiting for channel window
    std::string socksBuf; // dynamic forwarding negotiation buffer
    enum class State { Socks, Open } state = State::Open;
    bool sockEof = false;
    bool chanEof = false;
    bool chanClosed = false;  // the server closed the channel: deliver what is left, then end
    size_t chanPending = 0;   // channel data libssh still holds for us (not taken into toSock yet)
    bool dead = false;
    bool inEvent = false;
    short events = 0;    // what the socket is watched for in event_
    bool agent = false;  // auth-agent channel (no socket)
    std::unique_ptr<AgentForwarder> agentFwd;
    std::unique_ptr<X11AuthGate> x11Gate;  // X11: the client's setup not checked yet
    bool x11Refused = false;               // X11: the client was turned away
    std::string desc;
};

struct Listener {
    SshTransportImpl* owner = nullptr;
    PortForward fwd;
    sys::SocketHandle sock = sys::kInvalidSocket;
    bool inEvent = false;
};

class SshTransportImpl final : public SshTransport {
public:
    explicit SshTransportImpl(TransportContext ctx) : ctx_(std::move(ctx)) {}

    ~SshTransportImpl() override {
        close();
        if (thread_.joinable()) thread_.join();
    }

    Protocol protocol() const override { return Protocol::Ssh; }

    void start() override { thread_ = std::thread([this] { run(); }); }

    void write(std::string bytes) override {
        if (bytes.empty()) return;
        {
            std::lock_guard lock(mutex_);
            writeQueue_ += bytes;
        }
        wake_.signal();
    }

    void resize(int cols, int rows, int widthPx, int heightPx) override {
        {
            std::lock_guard lock(mutex_);
            pendingSize_ = {cols, rows, widthPx, heightPx};
            sizeDirty_ = true;
        }
        wake_.signal();
    }

    std::vector<SpecialCommandDef> specials() const override { return kSpecials; }

    void sendSpecial(const std::string& id) override {
        {
            std::lock_guard lock(mutex_);
            specialQueue_.push_back(id);
        }
        wake_.signal();
    }

    void close() override {
        closing_ = true;
        cancel_ = true;
        {
            std::lock_guard lock(sockMutex_);
            if (sock_ != sys::kInvalidSocket) sys::shutdownBoth(sock_);
        }
        wake_.signal();
    }

    LdiscModes defaultLdisc() const override { return {false, false}; }

    void post(std::function<void(ssh_session)> task) override {
        bool runNow = false;
        {
            std::lock_guard lock(mutex_);
            if (finished_)
                runNow = true;
            else
                tasks_.push_back(std::move(task));
        }
        if (runNow) {
            task(nullptr);
            return;
        }
        wake_.signal();
    }

    bool isConnected() const override { return connected_.load(); }

    /* ---- called from callbacks on the I/O thread ---- */
    void onMainData(const char* data, size_t len) {
        if (ctx_.listener) ctx_.listener->onData(std::string_view(data, len));
    }
    void onMainEof() { mainEof_ = true; }
    void onMainClose() { mainClosed_ = true; }
    void onExitStatus(int status) {
        exitKnown_ = true;
        exitCode_ = status;
        log(LogLevel::Info, "Server sent command exit status " + std::to_string(status));
    }
    void onExitSignal(const std::string& sig, const std::string& msg) {
        exitSignal_ = sig;
        exitKnown_ = true;
        log(LogLevel::Info, "Remote process terminated by signal " + sig + (msg.empty() ? "" : " (" + msg + ")"));
    }
    ssh_channel openForwardedTcpip(const char* destAddr, int destPort, const char* origAddr, int origPort);
    ssh_channel openX11(const char* origAddr, int origPort);
    sys::SocketHandle connectX11(std::string& error);
    std::string x11RealCookie();
    ssh_channel openAgent();
    size_t onRelayData(Relay* r, const char* data, size_t len);
    void onRelayEof(Relay* r);
    void onRelayClose(Relay* r);
    void onRelaySocket(Relay* r);
    void onListener(Listener* l);
    void drainWake() { wake_.drain(); }

private:
    void run();
    void log(LogLevel level, const std::string& msg) {
        if (ctx_.listener) ctx_.listener->onLog(level, msg);
    }
    void finish(CloseInfo info);
    bool openMainChannel(std::string& error);
    void setupForwards();
    void processQueues();
    void flushMainWrites();
    void pumpRelays();
    void keepalive();
    Relay* addRelay(ssh_channel ch, sys::SocketHandle sock, const std::string& desc);
    void registerRelaySocket(Relay* r);
    void updateRelayEvents(Relay* r);
    void destroyRelay(Relay* r);
    bool handleSocks(Relay* r);
    bool openDirect(Relay* r, const std::string& host, int port);
    void teardown();

    TransportContext ctx_;
    std::thread thread_;
    sys::PollWaker wake_;
    std::atomic<bool> closing_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<bool> connected_{false};

    std::mutex sockMutex_;
    sys::SocketHandle sock_ = sys::kInvalidSocket;  // for cancellation only (owned by libssh after connect)

    std::mutex mutex_;
    std::string writeQueue_;
    struct Size {
        int cols, rows, w, h;
    } pendingSize_{};
    bool sizeDirty_ = false;
    std::deque<std::string> specialQueue_;
    std::deque<std::function<void(ssh_session)>> tasks_;
    bool finished_ = false;

    // I/O-thread state
    SessionPtr session_;
    ssh_event event_ = nullptr;
    ssh_channel main_ = nullptr;
    ssh_channel_callbacks_struct mainCb_{};
    ssh_callbacks_struct sessionCb_{};
    std::string pendingWrite_;
    bool mainEof_ = false, mainClosed_ = false, exitKnown_ = false;
    int exitCode_ = 0;
    std::string exitSignal_;
    std::list<std::unique_ptr<Relay>> relays_;
    std::list<std::unique_ptr<Listener>> listeners_;
    std::vector<PortForward> remoteForwards_;
    X11Display x11_;
    bool x11Enabled_ = false;
    std::string x11Fake_;     // the cookie the server got (16 raw bytes)
    std::string x11Address_;  // a remote X server's address bytes (its Xauthority entries)
    bool x11CookieLogged_ = false;
    Clock::time_point lastActivity_ = Clock::now();
    bool closedOnce_ = false;
};

/* ---- C callbacks ---------------------------------------------------------------------------------- */

int cbMainData(ssh_session, ssh_channel, void* data, uint32_t len, int, void* ud) {
    static_cast<SshTransportImpl*>(ud)->onMainData(static_cast<const char*>(data), len);
    return static_cast<int>(len);
}
void cbMainEof(ssh_session, ssh_channel, void* ud) { static_cast<SshTransportImpl*>(ud)->onMainEof(); }
void cbMainClose(ssh_session, ssh_channel, void* ud) { static_cast<SshTransportImpl*>(ud)->onMainClose(); }
void cbExitStatus(ssh_session, ssh_channel, int status, void* ud) { static_cast<SshTransportImpl*>(ud)->onExitStatus(status); }
void cbExitSignal(ssh_session, ssh_channel, const char* sig, int, const char* msg, const char*, void* ud) {
    static_cast<SshTransportImpl*>(ud)->onExitSignal(sig ? sig : "", msg ? msg : "");
}
ssh_channel cbForwardedTcpip(ssh_session, const char* destAddr, int destPort, const char* origAddr, int origPort, void* ud) {
    return static_cast<SshTransportImpl*>(ud)->openForwardedTcpip(destAddr, destPort, origAddr, origPort);
}
ssh_channel cbX11(ssh_session, const char* origAddr, int origPort, void* ud) {
    return static_cast<SshTransportImpl*>(ud)->openX11(origAddr, origPort);
}
ssh_channel cbAgent(ssh_session, void* ud) { return static_cast<SshTransportImpl*>(ud)->openAgent(); }

int cbRelayData(ssh_session, ssh_channel, void* data, uint32_t len, int, void* ud) {
    auto* r = static_cast<Relay*>(ud);
    return static_cast<int>(r->owner->onRelayData(r, static_cast<const char*>(data), len));
}
void cbRelayEof(ssh_session, ssh_channel, void* ud) {
    auto* r = static_cast<Relay*>(ud);
    r->owner->onRelayEof(r);
}
void cbRelayClose(ssh_session, ssh_channel, void* ud) {
    auto* r = static_cast<Relay*>(ud);
    r->owner->onRelayClose(r);
}
int cbRelaySocket(socket_t, int, void* ud) {
    auto* r = static_cast<Relay*>(ud);
    r->owner->onRelaySocket(r);
    return 0;
}
int cbListener(socket_t, int, void* ud) {
    auto* l = static_cast<Listener*>(ud);
    l->owner->onListener(l);
    return 0;
}
int cbWake(socket_t, int, void* ud) {
    static_cast<SshTransportImpl*>(ud)->drainWake();
    return 0;
}

/* ---- main thread ------------------------------------------------------------------------------------ */

void SshTransportImpl::run() {
    ensureInit();
    const bool debug = ctx_.session.logging.mode == LogMode::SshDebug;
    ThreadLogScope logScope([this](LogLevel l, const std::string& m) { log(l, m); }, debug);
    const SessionConfig& cfg = ctx_.session;
    std::string error;
    if (!wake_.open(error)) {
        finish({false, false, 0, "", "Internal error: " + error, true});
        return;
    }
    if (ctx_.listener) ctx_.listener->onStatus(ConnStatus::Connecting, "Connecting to " + cfg.host);

    const int port = cfg.port > 0 ? cfg.port : 22;
    sys::SocketHandle sock = openConnection(ctx_, cfg.host, port, error, &cancel_);
    if (sock == sys::kInvalidSocket) {
        finish({closing_.load(), false, 0, "", closing_ ? "Connection closed by user" : error, !closing_});
        return;
    }
    {
        std::lock_guard lock(sockMutex_);
        sock_ = sock;
    }
    if (closing_) sys::shutdownBoth(sock);
    if (ctx_.listener) ctx_.listener->onStatus(ConnStatus::Authenticating, "");

    SshConnector connector(ctx_, cfg, "", cancel_);
    ConnectResult cr = connector.run(sock, logScope.level());
    if (!cr.session) {
        {
            std::lock_guard lock(sockMutex_);
            detachConnection(sock_);
            sock_ = sys::kInvalidSocket;
        }
        cr.failed.reset();  // closes the socket, now that close() cannot reach its number any more
        const bool user = closing_.load() || cr.cancelled;
        finish({user, false, 0, "", closing_ ? "Connection closed by user" : cr.error, !user});
        return;
    }
    session_ = std::move(cr.session);
    if (ctx_.listener) ctx_.listener->onServerInfo(connector.serverInfo());

    ssh_callbacks_init(&sessionCb_);
    sessionCb_.userdata = this;
    sessionCb_.channel_open_request_forwarded_tcpip_function = cbForwardedTcpip;
    sessionCb_.channel_open_request_x11_function = cbX11;
    sessionCb_.channel_open_request_auth_agent_function = cbAgent;
    ssh_set_callbacks(session_.get(), &sessionCb_);

    if (!cfg.ssh.noShell && !openMainChannel(error)) {
        teardown();
        finish({closing_.load(), false, 0, "", closing_ ? "Connection closed by user" : error, !closing_});
        return;
    }
    setupForwards();

    event_ = ssh_event_new();
    ssh_event_add_session(event_, session_.get());
    ssh_event_add_fd(event_, static_cast<socket_t>(wake_.pollHandle()), sys::kPollIn, cbWake, this);
    for (auto& l : listeners_) {
        ssh_event_add_fd(event_, static_cast<socket_t>(l->sock), sys::kPollIn, cbListener, l.get());
        l->inEvent = true;
    }
    ssh_set_blocking(session_.get(), 0);
    connected_ = true;
    if (ctx_.listener) ctx_.listener->onStatus(ConnStatus::Connected, "");

    CloseInfo info;
    info.clean = true;
    while (true) {
        if (closing_) {
            info.reason = "Connection closed by user";
            break;
        }
        processQueues();
        flushMainWrites();
        pumpRelays();
        keepalive();
        const int rc = ssh_event_dopoll(event_, 200);
        if (rc == SSH_ERROR || !ssh_is_connected(session_.get())) {
            if (closing_) {
                info.reason = "Connection closed by user";
                break;
            }
            if (main_ && (mainClosed_ || exitKnown_)) break;
            info.clean = false;
            info.error = true;
            const std::string why = sshError(session_.get(), "");
            info.reason = why.empty() ? "Server unexpectedly closed network connection" : "Network error: " + why;
            break;
        }
        if (main_ && (mainClosed_ || (mainEof_ && exitKnown_) || ssh_channel_is_closed(main_))) {
            info.reason = "Session ended";
            break;
        }
    }
    connected_ = false;
    if (exitKnown_ && exitSignal_.empty()) {
        info.hasExitCode = true;
        info.exitCode = exitCode_;
        if (info.reason == "Session ended") info.reason = "Session ended (exit code " + std::to_string(exitCode_) + ")";
    }
    info.exitSignal = exitSignal_;
    teardown();
    finish(info);
}

bool SshTransportImpl::openMainChannel(std::string& error) {
    const SessionConfig& cfg = ctx_.session;
    ssh_session s = session_.get();
    main_ = ssh_channel_new(s);
    if (!main_ || ssh_channel_open_session(main_) != SSH_OK) {
        error = "Could not open the session channel: " + sshError(s, "rejected");
        return false;
    }
    log(LogLevel::Info, "Opened main channel");
    ssh_callbacks_init(&mainCb_);
    mainCb_.userdata = this;
    mainCb_.channel_data_function = cbMainData;
    mainCb_.channel_eof_function = cbMainEof;
    mainCb_.channel_close_function = cbMainClose;
    mainCb_.channel_exit_status_function = cbExitStatus;
    mainCb_.channel_exit_signal_function = cbExitSignal;
    ssh_set_channel_callbacks(main_, &mainCb_);

    if (cfg.ssh.auth.agentForwarding) {
        if (ssh_channel_request_auth_agent(main_) == SSH_OK)
            log(LogLevel::Info, "Agent forwarding enabled");
        else
            log(LogLevel::Warn, "Server refused agent forwarding");
    }
    if (cfg.ssh.x11.enabled) {
        std::string err;
        std::string display = trim(cfg.ssh.x11.display);
        if (display.empty()) display = sys::getEnv("DISPLAY").value_or(":0");  // automatic
        if (!parseX11Display(display, x11_, err)) {
            log(LogLevel::Warn, "X11 forwarding disabled: " + err);
        } else {
            x11Fake_ = crypto::randomBytes(16);
            if (!x11_.local && !x11_.host.empty()) {
                std::string resolveError;
                const auto addresses = sys::isNumericAddress(x11_.host) ? std::vector<std::string>{x11_.host}
                                                                        : sys::resolveHost(x11_.host, sys::IpFamily::Auto, resolveError);
                if (!addresses.empty()) x11Address_ = sys::addressBytes(addresses.front());
            }
            if (ssh_channel_request_x11(main_, 0, "MIT-MAGIC-COOKIE-1", hexEncode(x11Fake_).c_str(), x11_.screen) == SSH_OK) {
                x11Enabled_ = true;
                log(LogLevel::Info, "X11 forwarding enabled (local display " + display + ")");
            } else {
                log(LogLevel::Warn, "Server refused X11 forwarding");
            }
        }
    }
    if (cfg.ssh.allocatePty) {
        std::string modes = encodePtyModes(cfg);
        const std::string term = cfg.connection.terminalType.empty() ? "xterm" : cfg.connection.terminalType;
        Size sz;
        {
            std::lock_guard lock(mutex_);
            sz = sizeDirty_ ? pendingSize_ : Size{ctx_.cols, ctx_.rows, ctx_.widthPx, ctx_.heightPx};
            sizeDirty_ = false;
        }
        if (ssh_channel_request_pty_size_modes(main_, term.c_str(), std::max(1, sz.cols), std::max(1, sz.rows),
                                               reinterpret_cast<const unsigned char*>(modes.data()), modes.size()) == SSH_OK)
            log(LogLevel::Info, "Allocated pty (" + term + ", " + std::to_string(sz.cols) + "x" + std::to_string(sz.rows) + ")");
        else
            log(LogLevel::Warn, "Server refused to allocate pty");
    }
    for (const auto& e : cfg.connection.environment) {
        if (e.name.empty()) continue;
        if (ssh_channel_request_env(main_, e.name.c_str(), e.value.c_str()) != SSH_OK)
            log(LogLevel::Warn, "Server refused to set environment variable " + e.name);
    }
    if (!cfg.ssh.remoteCommand.empty()) {
        if (ssh_channel_request_exec(main_, cfg.ssh.remoteCommand.c_str()) != SSH_OK) {
            error = "Server refused to start a command: " + sshError(s, "rejected");
            return false;
        }
        log(LogLevel::Info, "Started a command: " + cfg.ssh.remoteCommand);
    } else {
        if (ssh_channel_request_shell(main_) != SSH_OK) {
            error = "Server refused to start a shell: " + sshError(s, "rejected");
            return false;
        }
        log(LogLevel::Info, "Started a shell");
    }
    return true;
}

void SshTransportImpl::setupForwards() {
    const SessionConfig& cfg = ctx_.session;
    if (cfg.ssh.noShell) log(LogLevel::Info, "Started a session with no shell (port forwarding only)");
    for (const auto& f : cfg.ssh.tunnels) {
        if (!f.enabled || f.sourcePort < 0) continue;
        if (f.type == ForwardType::Remote) {
            const std::string bind = !f.bindAddress.empty() ? f.bindAddress : (cfg.ssh.remotePortsAcceptAll ? "" : "localhost");
            int bound = 0;
            if (ssh_channel_listen_forward(session_.get(), bind.c_str(), f.sourcePort, &bound) == SSH_OK) {
                PortForward rf = f;
                if (rf.sourcePort == 0) rf.sourcePort = bound;
                remoteForwards_.push_back(rf);
                log(LogLevel::Info, "Remote port forwarding from " + describe(rf) + " enabled");
            } else {
                log(LogLevel::Warn, "Server refused remote port forwarding for " + describe(f) + ": " + sshError(session_.get(), "denied"));
            }
            continue;
        }
        std::string err;
        int bound = 0;
        sys::SocketHandle ls = sys::listenTcp(forwardBind(f, cfg.ssh.localPortsAcceptAll), f.sourcePort, ipFamily(f.addressFamily), err, &bound);
        if (ls == sys::kInvalidSocket) {
            log(LogLevel::Warn, "Could not open " + describe(f) + ": " + err);
            continue;
        }
        sys::setNonBlocking(ls, true);
        auto l = std::make_unique<Listener>();
        l->owner = this;
        l->fwd = f;
        if (l->fwd.sourcePort == 0) l->fwd.sourcePort = bound;
        l->sock = ls;
        log(LogLevel::Info, (f.type == ForwardType::Dynamic ? "Dynamic forwarding on " : "Forwarding ") + describe(l->fwd));
        listeners_.push_back(std::move(l));
    }
}

void SshTransportImpl::processQueues() {
    std::string writes;
    std::deque<std::string> specials;
    std::deque<std::function<void(ssh_session)>> tasks;
    bool resize = false;
    Size sz{};
    {
        std::lock_guard lock(mutex_);
        writes.swap(writeQueue_);
        specials.swap(specialQueue_);
        tasks.swap(tasks_);
        if (sizeDirty_) {
            resize = true;
            sz = pendingSize_;
            sizeDirty_ = false;
        }
    }
    pendingWrite_ += writes;
    ssh_session s = session_.get();
    if (resize && main_ && ctx_.session.ssh.allocatePty) ssh_channel_change_pty_size(main_, std::max(1, sz.cols), std::max(1, sz.rows));
    for (const auto& id : specials) {
        if (!main_ && id != "ignore") continue;
        if (id == "break") {
            ssh_channel_request_send_break(main_, 500);
            log(LogLevel::Info, "Sent break");
        } else if (id == "eof") {
            ssh_channel_send_eof(main_);
            log(LogLevel::Info, "Sent EOF");
        } else if (id == "ignore") {
            ssh_send_ignore(s, "");
        } else if (startsWith(id, "signal:")) {
            const std::string sig = id.substr(7);
            ssh_channel_request_send_signal(main_, sig.c_str());
            log(LogLevel::Info, "Sent signal " + sig);
        }
    }
    if (!tasks.empty()) {
        // Posted tasks (SFTP) use blocking libssh APIs.
        ssh_set_blocking(s, 1);
        for (auto& t : tasks) {
            try {
                t(s);
            } catch (const std::exception& e) {
                applog::error(std::string("SSH task threw: ") + e.what());
            }
        }
        ssh_set_blocking(s, 0);
    }
}

void SshTransportImpl::flushMainWrites() {
    if (!main_ || pendingWrite_.empty()) return;
    while (!pendingWrite_.empty()) {
        const uint32_t window = ssh_channel_window_size(main_);
        if (window == 0) break;
        const uint32_t chunk = static_cast<uint32_t>(std::min<size_t>({pendingWrite_.size(), window, 32768}));
        const int n = ssh_channel_write(main_, pendingWrite_.data(), chunk);
        if (n <= 0) break;
        pendingWrite_.erase(0, static_cast<size_t>(n));
        lastActivity_ = Clock::now();
    }
}

void SshTransportImpl::keepalive() {
    const int secs = ctx_.session.connection.keepaliveSeconds;
    if (secs <= 0) return;
    const auto now = Clock::now();
    if (now - lastActivity_ >= std::chrono::seconds(secs)) {
        ssh_send_keepalive(session_.get());
        lastActivity_ = now;
    }
}

/* ---- forwarding ---- */

Relay* SshTransportImpl::addRelay(ssh_channel ch, sys::SocketHandle sock, const std::string& desc) {
    auto r = std::make_unique<Relay>();
    r->owner = this;
    r->channel = ch;
    r->sock = sock;
    r->desc = desc;
    if (ch) {
        ssh_callbacks_init(&r->cb);
        r->cb.userdata = r.get();
        r->cb.channel_data_function = cbRelayData;
        r->cb.channel_eof_function = cbRelayEof;
        r->cb.channel_close_function = cbRelayClose;
        ssh_set_channel_callbacks(ch, &r->cb);
    }
    Relay* raw = r.get();
    relays_.push_back(std::move(r));
    return raw;
}

void SshTransportImpl::registerRelaySocket(Relay* r) {
    if (r->sock == sys::kInvalidSocket || r->inEvent || !event_) return;
    sys::setNonBlocking(r->sock, true);
    updateRelayEvents(r);
}

/** Watches the socket for exactly what the relay can use now: reading while there is room towards the channel (and no
 *  EOF yet, which would stay readable), writing while data waits for it. A socket that is not watched cannot wake the
 *  event loop over and over. */
void SshTransportImpl::updateRelayEvents(Relay* r) {
    if (r->sock == sys::kInvalidSocket || !event_) return;
    short want = 0;
    if (!r->sockEof && r->toChan.size() < kRelayBuffer) want |= sys::kPollIn;
    if (!r->toSock.empty()) want |= sys::kPollOut;
    if (r->inEvent && want == r->events) return;
    if (r->inEvent) ssh_event_remove_fd(event_, static_cast<socket_t>(r->sock));
    r->inEvent = want != 0;
    r->events = want;
    if (want) ssh_event_add_fd(event_, static_cast<socket_t>(r->sock), want, cbRelaySocket, r);
}

void SshTransportImpl::destroyRelay(Relay* r) {
    if (r->inEvent && event_) ssh_event_remove_fd(event_, static_cast<socket_t>(r->sock));
    r->inEvent = false;
    if (r->channel) {
        ssh_remove_channel_callbacks(r->channel, &r->cb);
        if (ssh_channel_is_open(r->channel)) {
            ssh_channel_send_eof(r->channel);
            ssh_channel_close(r->channel);
        }
        ssh_channel_free(r->channel);
        r->channel = nullptr;
    }
    sys::closeSocket(r->sock);
}

void SshTransportImpl::onListener(Listener* l) {
    sys::SocketHandle c = sys::acceptConnection(l->sock, true);
    if (c == sys::kInvalidSocket) return;
    const std::string from = sys::peerName(c);
    if (l->fwd.type == ForwardType::Dynamic) {
        Relay* r = addRelay(nullptr, c, "SOCKS connection from " + from);
        r->state = Relay::State::Socks;
        registerRelaySocket(r);
        return;
    }
    Relay* r = addRelay(nullptr, c, "Forwarded connection from " + from + " to " + l->fwd.destHost + ":" + std::to_string(l->fwd.destPort));
    if (!openDirect(r, l->fwd.destHost, l->fwd.destPort)) {
        r->dead = true;
        return;
    }
    registerRelaySocket(r);
}

bool SshTransportImpl::openDirect(Relay* r, const std::string& host, int port) {
    ssh_session s = session_.get();
    ssh_channel ch = ssh_channel_new(s);
    if (!ch) return false;
    ssh_set_blocking(s, 1);
    std::string origin = "127.0.0.1";
    int originPort = 0;
    if (const auto peer = sys::peerAddress(r->sock); peer && peer->first.find(':') == std::string::npos) {  // IPv4 only
        origin = peer->first;
        originPort = peer->second;
    }
    const int rc = ssh_channel_open_forward(ch, host.c_str(), port, origin.c_str(), originPort);
    ssh_set_blocking(s, 0);
    if (rc != SSH_OK) {
        log(LogLevel::Warn, "Forwarded connection to " + host + ":" + std::to_string(port) + " refused by server: " + sshError(s, "rejected"));
        ssh_channel_free(ch);
        return false;
    }
    r->channel = ch;
    ssh_callbacks_init(&r->cb);
    r->cb.userdata = r;
    r->cb.channel_data_function = cbRelayData;
    r->cb.channel_eof_function = cbRelayEof;
    r->cb.channel_close_function = cbRelayClose;
    ssh_set_channel_callbacks(ch, &r->cb);
    log(LogLevel::Info, "Opened direct-tcpip channel to " + host + ":" + std::to_string(port));
    return true;
}

bool SshTransportImpl::handleSocks(Relay* r) {
    std::string& b = r->socksBuf;
    if (b.empty()) return true;
    const unsigned char ver = static_cast<unsigned char>(b[0]);
    auto reject = [&](const std::string& reply) {
        sys::sendAll(r->sock, reply);
        r->dead = true;
        return false;
    };
    if (ver == 4) {
        if (b.size() < 9) return true;
        size_t userEnd = b.find('\0', 8);
        if (userEnd == std::string::npos) return true;
        const int port = (static_cast<unsigned char>(b[2]) << 8) | static_cast<unsigned char>(b[3]);
        const unsigned char* ip = reinterpret_cast<const unsigned char*>(b.data() + 4);
        std::string host;
        size_t consumed = userEnd + 1;
        if (ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] != 0) {  // SOCKS4a
            size_t hostEnd = b.find('\0', userEnd + 1);
            if (hostEnd == std::string::npos) return true;
            host = b.substr(userEnd + 1, hostEnd - userEnd - 1);
            consumed = hostEnd + 1;
        } else {
            host = std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) + "." + std::to_string(ip[3]);
        }
        if (static_cast<unsigned char>(b[1]) != 1) return reject(std::string("\x00\x5b\0\0\0\0\0\0", 8));
        std::string rest = b.substr(consumed);
        b.clear();
        if (!openDirect(r, host, port)) return reject(std::string("\x00\x5b\0\0\0\0\0\0", 8));
        sys::sendAll(r->sock, std::string("\x00\x5a\0\0\0\0\0\0", 8));
        r->state = Relay::State::Open;
        r->toChan += rest;
        return true;
    }
    if (ver == 5) {
        if (b.size() < 2) return true;
        const size_t nmethods = static_cast<unsigned char>(b[1]);
        if (b.size() < 2 + nmethods) return true;
        const size_t reqStart = 2 + nmethods;
        if (r->desc.find("[greeted]") == std::string::npos) {
            sys::sendAll(r->sock, std::string("\x05\x00", 2));
            r->desc += " [greeted]";
        }
        if (b.size() < reqStart + 4) return true;
        const char* q = b.data() + reqStart;
        if (static_cast<unsigned char>(q[1]) != 1) return reject(std::string("\x05\x07\x00\x01\0\0\0\0\0\0", 10));
        const unsigned char atyp = static_cast<unsigned char>(q[3]);
        std::string host;
        size_t addrEnd;
        if (atyp == 1) {
            if (b.size() < reqStart + 10) return true;
            const unsigned char* ip = reinterpret_cast<const unsigned char*>(q + 4);
            host = std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) + "." + std::to_string(ip[3]);
            addrEnd = reqStart + 8;
        } else if (atyp == 3) {
            if (b.size() < reqStart + 5) return true;
            const size_t n = static_cast<unsigned char>(q[4]);
            if (b.size() < reqStart + 5 + n + 2) return true;
            host.assign(q + 5, n);
            addrEnd = reqStart + 5 + n;
        } else if (atyp == 4) {
            if (b.size() < reqStart + 22) return true;
            host = sys::formatAddressBytes(std::string_view(q + 4, 16));
            addrEnd = reqStart + 20;
        } else {
            return reject(std::string("\x05\x08\x00\x01\0\0\0\0\0\0", 10));
        }
        const int port = (static_cast<unsigned char>(b[addrEnd]) << 8) | static_cast<unsigned char>(b[addrEnd + 1]);
        std::string rest = b.substr(addrEnd + 2);
        b.clear();
        if (!openDirect(r, host, port)) return reject(std::string("\x05\x05\x00\x01\0\0\0\0\0\0", 10));
        sys::sendAll(r->sock, std::string("\x05\x00\x00\x01\0\0\0\0\0\0", 10));
        r->state = Relay::State::Open;
        r->toChan += rest;
        return true;
    }
    r->dead = true;
    return false;
}

void SshTransportImpl::onRelaySocket(Relay* r) {
    if (r->dead || r->sock == sys::kInvalidSocket || r->sockEof) return;
    if (r->toChan.size() >= kRelayBuffer) return;  // back-pressure: wait for the channel window (pumpRelays sends)
    char buf[16384];
    const long n = sys::recvSome(r->sock, buf, sizeof buf);
    if (n > 0) {
        if (r->state == Relay::State::Socks) {
            r->socksBuf.append(buf, static_cast<size_t>(n));
            handleSocks(r);
        } else {
            r->toChan.append(buf, static_cast<size_t>(n));
        }
    } else if (n == 0 || sys::lastNetError() != sys::NetErr::WouldBlock) {
        r->sockEof = true;
    }
}

size_t SshTransportImpl::onRelayData(Relay* r, const char* data, size_t len) {
    if (r->agent) {
        std::string reply = r->agentFwd->feed(data, len);
        if (!reply.empty()) r->toChan += reply;
        return len;
    }
    if (r->x11Refused) return len;  // turned away: what it still sends goes nowhere
    if (r->x11Gate) {
        std::string toServer, toClient;
        switch (r->x11Gate->feed(data, len, toServer, toClient)) {
            case X11AuthGate::State::Waiting: break;
            case X11AuthGate::State::Open:
                r->toSock += toServer;
                r->x11Gate.reset();
                break;
            case X11AuthGate::State::Refused:
                log(LogLevel::Warn, "X11 connection refused (" + r->x11Gate->reason() + "): " + r->desc);
                r->toChan += toClient;
                r->x11Refused = true;
                r->sockEof = true;  // the X server is not involved
                sys::shutdownBoth(r->sock);
                r->x11Gate.reset();
                break;
        }
        return len;
    }
    // Only what fits: libssh keeps the rest, and the SSH window stays shut until pumpRelays takes it.
    const size_t take = std::min(len, kRelayBuffer - std::min(kRelayBuffer, r->toSock.size()));
    r->toSock.append(data, take);
    r->chanPending = len - take;
    return take;
}

void SshTransportImpl::onRelayEof(Relay* r) {
    r->chanEof = true;
    if (r->sock != sys::kInvalidSocket && r->toSock.empty() && r->chanPending == 0) sys::shutdownWrite(r->sock);
}

void SshTransportImpl::onRelayClose(Relay* r) {
    r->chanEof = true;
    r->chanClosed = true;
}

void SshTransportImpl::pumpRelays() {
    for (auto it = relays_.begin(); it != relays_.end();) {
        Relay* r = it->get();
        // Channel data libssh still holds, as far as there is room (it is in libssh's buffer: no packets are read).
        if (!r->dead && r->channel && r->chanPending > 0 && r->toSock.size() < kRelayBuffer) {
            std::string buf(std::min(r->chanPending, kRelayBuffer - r->toSock.size()), '\0');
            const int n = ssh_channel_read_nonblocking(r->channel, buf.data(), static_cast<uint32_t>(buf.size()), 0);
            if (n > 0) {
                r->toSock.append(buf.data(), static_cast<size_t>(n));
                r->chanPending -= std::min(r->chanPending, static_cast<size_t>(n));
            } else {
                r->chanPending = 0;
            }
        }
        if (!r->dead && r->sock != sys::kInvalidSocket && !r->toSock.empty()) {
            const long n = sys::sendSome(r->sock, r->toSock.data(), std::min<size_t>(r->toSock.size(), 65536));
            if (n > 0) r->toSock.erase(0, static_cast<size_t>(n));
            else if (sys::lastNetError() != sys::NetErr::WouldBlock) r->dead = true;
            if (r->chanEof && r->toSock.empty() && r->chanPending == 0) sys::shutdownWrite(r->sock);
        }
        if (!r->dead && r->channel && !r->chanClosed && !r->toChan.empty()) {
            const uint32_t window = ssh_channel_window_size(r->channel);
            if (window > 0) {
                const uint32_t chunk = static_cast<uint32_t>(std::min<size_t>({r->toChan.size(), window, 32768}));
                const int n = ssh_channel_write(r->channel, r->toChan.data(), chunk);
                if (n > 0) r->toChan.erase(0, static_cast<size_t>(n));
                else if (n < 0) r->dead = true;
            }
        }
        if (!r->dead && r->sockEof && r->toChan.empty() && r->channel && !r->chanClosed && !ssh_channel_is_eof(r->channel)) {
            ssh_channel_send_eof(r->channel);
        }
        const bool delivered = r->toSock.empty() && r->chanPending == 0;
        const bool finished = r->dead || (r->chanClosed && delivered) ||
                              (r->sockEof && r->chanEof && delivered && r->toChan.empty());
        if (finished) {
            destroyRelay(r);
            it = relays_.erase(it);
        } else {
            updateRelayEvents(r);
            ++it;
        }
    }
}

ssh_channel SshTransportImpl::openForwardedTcpip(const char* destAddr, int destPort, const char* origAddr, int origPort) {
    const PortForward* fwd = nullptr;
    for (const auto& f : remoteForwards_)
        if (f.sourcePort == destPort) fwd = &f;
    if (!fwd) {
        log(LogLevel::Warn, "Rejected forwarded connection for unknown port " + std::to_string(destPort));
        return nullptr;
    }
    std::string err;
    sys::ConnectOptions o;
    o.timeoutMs = 5000;
    o.family = ipFamily(fwd->addressFamily);
    sys::SocketHandle s = sys::connectTcp(fwd->destHost, fwd->destPort, o, err, &cancel_);
    if (s == sys::kInvalidSocket) {
        log(LogLevel::Warn, "Forwarded connection to " + fwd->destHost + ":" + std::to_string(fwd->destPort) + " failed: " + err);
        return nullptr;
    }
    ssh_channel ch = ssh_channel_new(session_.get());
    if (!ch) {
        sys::closeSocket(s);
        return nullptr;
    }
    Relay* r = addRelay(ch, s, "Remote forward from " + std::string(origAddr ? origAddr : "?") + ":" + std::to_string(origPort));
    registerRelaySocket(r);
    log(LogLevel::Info, "Forwarded connection from " + std::string(destAddr ? destAddr : "") + ":" + std::to_string(destPort) +
                            " to " + fwd->destHost + ":" + std::to_string(fwd->destPort));
    return ch;
}

ssh_channel SshTransportImpl::openX11(const char* origAddr, int origPort) {
    if (!x11Enabled_) return nullptr;
    std::string err;
    sys::SocketHandle s = connectX11(err);
    if (s == sys::kInvalidSocket) {
        log(LogLevel::Warn, "X11 connection to the local display failed: " + err);
        return nullptr;
    }
    ssh_channel ch = ssh_channel_new(session_.get());
    if (!ch) {
        sys::closeSocket(s);
        return nullptr;
    }
    Relay* r = addRelay(ch, s, "X11 connection from " + std::string(origAddr ? origAddr : "?") + ":" + std::to_string(origPort));
    r->x11Gate = std::make_unique<X11AuthGate>(x11Fake_, x11RealCookie());
    registerRelaySocket(r);
    log(LogLevel::Info, "Opened X11 forward channel");
    return ch;
}

/** The local X server: its Unix socket, else TCP. */
sys::SocketHandle SshTransportImpl::connectX11(std::string& error) {
    std::string unixError;
    if (!x11_.unixPath.empty()) {
        sys::SocketHandle s = sys::connectUnix(x11_.unixPath, unixError);
        if (s != sys::kInvalidSocket) return s;
        unixError = x11_.unixPath + ": " + unixError;
    }
    if (x11_.host.empty()) {
        error = unixError;
        return sys::kInvalidSocket;
    }
    sys::ConnectOptions o;
    o.timeoutMs = 5000;
    std::string tcpError;
    sys::SocketHandle s = sys::connectTcp(x11_.host, x11_.port, o, tcpError, &cancel_);
    if (s == sys::kInvalidSocket)
        error = (unixError.empty() ? "" : unixError + "; ") + sys::formatHostPort(x11_.host, x11_.port) + ": " + tcpError;
    return s;
}

/** The display's real cookie from the Xauthority file ($XAUTHORITY, else ~/.Xauthority), "" when it has none. */
std::string SshTransportImpl::x11RealCookie() {
    const auto env = sys::getEnv("XAUTHORITY");
    const std::filesystem::path file = env && !env->empty() ? sys::toPath(*env) : sys::homeDirectory() / ".Xauthority";
    std::optional<std::string> cookie;
    if (const auto content = sys::readFile(file))
        cookie = findX11Cookie(parseXauthority(*content), x11_, sys::localHostName(), x11Address_);
    if (!x11CookieLogged_) {
        x11CookieLogged_ = true;
        log(LogLevel::Info, cookie ? "X11: the local display's cookie is from " + sys::fromPath(file)
                                   : "X11: no cookie for the local display in " + sys::fromPath(file) + ", connecting without one");
    }
    return cookie.value_or(std::string());
}

ssh_channel SshTransportImpl::openAgent() {
    if (!ctx_.session.ssh.auth.agentForwarding) return nullptr;
    ssh_channel ch = ssh_channel_new(session_.get());
    if (!ch) return nullptr;
    Relay* r = addRelay(ch, sys::kInvalidSocket, "Agent forwarding channel");
    r->agent = true;
    r->sockEof = true;  // no socket side
    r->agentFwd = std::make_unique<AgentForwarder>(ctx_.session.ssh.auth.agent);
    log(LogLevel::Info, "Opened agent forwarding channel");
    return ch;
}

void SshTransportImpl::teardown() {
    for (auto& r : relays_) destroyRelay(r.get());
    relays_.clear();
    for (auto& l : listeners_) {
        if (l->inEvent && event_) ssh_event_remove_fd(event_, static_cast<socket_t>(l->sock));
        sys::closeSocket(l->sock);
    }
    listeners_.clear();
    if (event_) {
        ssh_event_remove_fd(event_, static_cast<socket_t>(wake_.pollHandle()));
        if (session_) ssh_event_remove_session(event_, session_.get());
        ssh_event_free(event_);
        event_ = nullptr;
    }
    if (main_) {
        ssh_remove_channel_callbacks(main_, &mainCb_);
        if (ssh_channel_is_open(main_)) ssh_channel_close(main_);
        ssh_channel_free(main_);
        main_ = nullptr;
    }
    {
        std::lock_guard lock(sockMutex_);
        detachConnection(sock_);  // local-command proxy: stop logging into our listener, end the process
        sock_ = sys::kInvalidSocket;
    }
    if (session_) {
        ssh_set_blocking(session_.get(), 1);
        if (ssh_is_connected(session_.get())) ssh_disconnect(session_.get());
        session_.reset();
    }
}

void SshTransportImpl::finish(CloseInfo info) {
    std::deque<std::function<void(ssh_session)>> tasks;
    {
        std::lock_guard lock(mutex_);
        finished_ = true;
        tasks.swap(tasks_);
    }
    for (auto& t : tasks) t(nullptr);
    if (closedOnce_) return;
    closedOnce_ = true;
    if (info.error) log(LogLevel::Error, info.reason);
    else log(LogLevel::Info, info.reason);
    if (ctx_.listener) ctx_.listener->onClose(info);
}

}  // namespace

std::unique_ptr<Transport> makeSshTransport(TransportContext ctx) {
    if (ctx.session.host.empty()) throw std::invalid_argument("No host name specified");
    return std::make_unique<SshTransportImpl>(std::move(ctx));
}

}  // namespace st
