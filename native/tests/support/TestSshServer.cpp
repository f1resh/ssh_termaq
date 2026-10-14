// In-process SSH server for integration tests (libssh server API). See TestServers.h for the behaviour contract.
//
// Structure: one accept thread; one thread per client connection running an ssh_event loop. Authentication,
// channel opens and global requests arrive through the message callback; per-channel requests (pty, shell, exec,
// env, subsystem, window-change, signal, x11, agent) through ssh_channel_callbacks. All channel output is queued
// and flushed by the loop according to the channel window, so large outputs never block inside callbacks.
#include "TestExecProcess.h"
#include "TestNet.h"
#include "TestServers.h"
#include "TestSftpServer.h"

#include "core/Crypto.h"
#include "core/Strings.h"

#include <libssh/callbacks.h>
#include <libssh/libssh.h>
#include <libssh/server.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace st::test {

namespace {

using Clock = std::chrono::steady_clock;

void libsshInitOnce() {
    static std::once_flag once;
    std::call_once(once, [] { ssh_init(); });
}

std::string keyBlob(ssh_key key) {
    char* b64 = nullptr;
    if (ssh_pki_export_pubkey_base64(key, &b64) != SSH_OK || !b64) return {};
    std::string out;
    st::crypto::base64Decode(b64, out);
    ssh_string_free_char(b64);
    return out;
}

/** Parses "type base64 [comment]" into a public key (nullptr on failure). */
ssh_key parseAuthorizedLine(const std::string& line) {
    auto parts = split(trim(line), ' ', true);
    if (parts.size() < 2) return nullptr;
    ssh_key key = nullptr;
    if (ssh_pki_import_pubkey_base64(parts[1].c_str(), ssh_key_type_from_name(parts[0].c_str()), &key) != SSH_OK)
        return nullptr;
    return key;
}

struct Connection;

struct ChannelState {
    enum class Kind { Session, Tcp } kind = Kind::Session;
    Connection* conn = nullptr;
    ssh_channel channel = nullptr;
    ssh_channel_callbacks_struct cb{};
    std::string out;  // pending output to the client
    bool closeAfterFlush = false;
    bool eofSent = false;
    bool remoteClosed = false;
    // session channels
    bool pty = false;
    std::string term;
    int cols = 0, rows = 0;
    std::map<std::string, std::string> env;
    enum class Mode { None, Shell, Exec, ExecProc, Sftp } mode = Mode::None;
    std::unique_ptr<ExecProcess> proc;  // Mode::ExecProc
    std::string errOut;                 // pending stderr to the client
    std::string line;
    bool lastCr = false;
    bool waitingForSignal = false;
    std::unique_ptr<SftpServer> sftp;
    // tcp relays (direct-tcpip / forwarded-tcpip)
    net::Socket sock;
    std::string toSocket;
    bool chanEof = false;      // client sent EOF: half-close the socket once toSocket is flushed
    bool sockShutdown = false;
};

struct Listener {
    net::Socket sock;
    std::string address;
    int port = 0;
};

}  // namespace

struct TestSshServer::Impl {
    TestSshServerOptions opts;
    ssh_bind bind = nullptr;
    ssh_key hostKey = nullptr;
    std::string hostKeyTypeName, hostKeyB64, hostKeyFp;
    int port = 0;
    std::atomic<bool> stopping{false};
    std::thread acceptThread;
    std::mutex connMutex;
    std::list<std::thread> connThreads;
    std::vector<ssh_key> authorized;

    mutable std::mutex evMutex;
    mutable std::condition_variable evCv;
    std::vector<std::string> events;
    std::atomic<int> lastRemoteForward{0};

    void event(const std::string& e) {
        {
            std::lock_guard lock(evMutex);
            events.push_back(e);
        }
        evCv.notify_all();
    }

    int authMethods() const {
        int m = 0;
        if (opts.allowPassword) m |= SSH_AUTH_METHOD_PASSWORD;
        if (opts.allowPublicKey) m |= SSH_AUTH_METHOD_PUBLICKEY;
        if (opts.allowKbdInt) m |= SSH_AUTH_METHOD_INTERACTIVE;
        return m;
    }

    void acceptLoop();
    void runConnection(ssh_session session);
};

namespace {

struct Connection {
    TestSshServer::Impl* server = nullptr;
    ssh_session session = nullptr;
    std::string user;
    std::string kbdUser;  // username of the pending keyboard-interactive exchange (responses carry none)
    bool authenticated = false;
    std::list<std::unique_ptr<ChannelState>> channels;
    std::vector<std::unique_ptr<Listener>> listeners;
};

/* ---- output helpers ---- */

void queue(ChannelState* ch, std::string_view data) { ch->out.append(data); }

void finishChannel(ChannelState* ch, int exitStatus) {
    if (ch->closeAfterFlush) return;
    if (ch->channel && exitStatus >= 0) ssh_channel_request_send_exit_status(ch->channel, exitStatus);
    ch->closeAfterFlush = true;
}

/* ---- shell ---- */

void shellCommand(ChannelState* ch, const std::string& rawLine) {
    TestSshServer::Impl* s = ch->conn->server;
    const std::string line = trim(rawLine);
    if (line.empty()) {
        queue(ch, "$ ");
        return;
    }
    auto sp = line.find(' ');
    const std::string cmd = line.substr(0, sp);
    const std::string arg = sp == std::string::npos ? "" : trim(line.substr(sp + 1));
    s->event("shell:" + line);
    if (cmd == "exit") {
        const int code = arg.empty() ? 0 : std::atoi(arg.c_str());
        queue(ch, "logout\r\n");
        finishChannel(ch, code);
        return;
    }
    if (cmd == "env") {
        auto it = ch->env.find(arg);
        queue(ch, (it == ch->env.end() ? std::string("<unset>") : it->second) + "\r\n");
    } else if (cmd == "size") {
        queue(ch, std::to_string(ch->cols) + "x" + std::to_string(ch->rows) + "\r\n");
    } else if (cmd == "term") {
        queue(ch, (ch->pty ? ch->term : std::string("<none>")) + "\r\n");
    } else if (cmd == "user") {
        queue(ch, ch->conn->user + "\r\n");
    } else if (cmd == "big") {
        long n = std::atol(arg.c_str());
        if (n < 0) n = 0;
        queue(ch, std::string(static_cast<size_t>(n), 'x') + "\r\nEND\r\n");
    } else if (cmd == "signal") {
        ch->waitingForSignal = true;
        return;  // the prompt follows the signal
    } else {
        queue(ch, "echo: " + line + "\r\n");
    }
    queue(ch, "$ ");
}

void shellInput(ChannelState* ch, std::string_view data) {
    for (char k : data) {
        if (ch->closeAfterFlush) return;
        if (k == '\n' && ch->lastCr) {
            ch->lastCr = false;
            continue;
        }
        ch->lastCr = k == '\r';
        if (k == '\r' || k == '\n') {
            if (ch->pty) queue(ch, "\r\n");
            std::string line;
            line.swap(ch->line);
            shellCommand(ch, line);
        } else if (k == 0x7f || k == 0x08) {
            if (!ch->line.empty()) {
                ch->line.pop_back();
                if (ch->pty) queue(ch, "\b \b");
            }
        } else if (k == 0x03) {
            ch->line.clear();
            queue(ch, "^C\r\n$ ");
        } else if (k == 0x04 && ch->line.empty()) {
            queue(ch, "logout\r\n");
            finishChannel(ch, 0);
        } else {
            ch->line.push_back(k);
            if (ch->pty) queue(ch, std::string_view(&k, 1));
        }
    }
}

void execCommand(ChannelState* ch, const std::string& command) {
    const std::string c = trim(command);
    if (startsWith(c, "echo ")) {
        queue(ch, c.substr(5) + "\n");
        finishChannel(ch, 0);
    } else if (c == "echo") {
        queue(ch, "\n");
        finishChannel(ch, 0);
    } else if (startsWith(c, "exit")) {
        finishChannel(ch, std::atoi(trim(c.substr(4)).c_str()));
    } else {
        queue(ch, c + ": command not found\n");
        finishChannel(ch, 127);
    }
}

/* ---- channel callbacks (userdata = ChannelState*) ---- */

ChannelState* st(void* ud) { return static_cast<ChannelState*>(ud); }

int onData(ssh_session, ssh_channel, void* data, uint32_t len, int, void* ud) {
    ChannelState* ch = st(ud);
    std::string_view d(static_cast<const char*>(data), len);
    if (ch->kind == ChannelState::Kind::Tcp) {
        ch->toSocket.append(d);
        return static_cast<int>(len);
    }
    switch (ch->mode) {
        case ChannelState::Mode::Shell: shellInput(ch, d); break;
        case ChannelState::Mode::Sftp: {
            std::string out;
            if (!ch->sftp->feed(d, out)) finishChannel(ch, -1);
            queue(ch, out);
            break;
        }
        case ChannelState::Mode::ExecProc: ch->proc->writeInput(std::string(d)); break;
        default: break;  // exec ignores stdin
    }
    return static_cast<int>(len);
}

void onEof(ssh_session, ssh_channel, void* ud) {
    ChannelState* ch = st(ud);
    if (ch->kind == ChannelState::Kind::Tcp) {
        ch->chanEof = true;  // pumpChannel half-closes after flushing pending data
    } else if (ch->mode == ChannelState::Mode::Sftp) {
        finishChannel(ch, 0);
    } else if (ch->mode == ChannelState::Mode::ExecProc) {
        ch->proc->closeInput();
    }
}

void onClose(ssh_session, ssh_channel, void* ud) { st(ud)->remoteClosed = true; }

void onSignal(ssh_session, ssh_channel, const char* signal, void* ud) {
    ChannelState* ch = st(ud);
    const std::string name = signal ? signal : "";
    ch->conn->server->event("signal:" + name);
    if (ch->waitingForSignal) {
        ch->waitingForSignal = false;
        queue(ch, "got signal " + name + "\r\n$ ");
    }
}

int onPty(ssh_session, ssh_channel, const char* term, int w, int h, int, int, void* ud) {
    ChannelState* ch = st(ud);
    ch->pty = true;
    ch->term = term ? term : "";
    ch->cols = w;
    ch->rows = h;
    ch->conn->server->event("pty:" + ch->term + ":" + std::to_string(w) + "x" + std::to_string(h));
    return 0;
}

int onWindowChange(ssh_session, ssh_channel, int w, int h, int, int, void* ud) {
    ChannelState* ch = st(ud);
    ch->cols = w;
    ch->rows = h;
    ch->conn->server->event("window-change:" + std::to_string(w) + "x" + std::to_string(h));
    return 0;
}

int onShell(ssh_session, ssh_channel, void* ud) {
    ChannelState* ch = st(ud);
    if (ch->mode != ChannelState::Mode::None) return -1;
    ch->mode = ChannelState::Mode::Shell;
    ch->conn->server->event("shell");
    queue(ch, "Welcome to the SSHTerm test server.\r\n$ ");
    return 0;
}

int onExec(ssh_session, ssh_channel, const char* command, void* ud) {
    ChannelState* ch = st(ud);
    if (ch->mode != ChannelState::Mode::None) return -1;
    const std::string c = command ? command : "";
    TestSshServer::Impl* s = ch->conn->server;
    s->event("exec:" + c);
    if (!s->opts.execShell.empty()) {
        ch->mode = ChannelState::Mode::ExecProc;
        try {
            ch->proc = ExecProcess::spawn(s->opts.execShell, s->opts.execCwd.empty() ? s->opts.sftpRoot : s->opts.execCwd, c);
        } catch (const std::exception& e) {
            ch->mode = ChannelState::Mode::Exec;
            queue(ch, std::string("cannot run: ") + e.what() + "\n");
            finishChannel(ch, 127);
        }
        return 0;
    }
    ch->mode = ChannelState::Mode::Exec;
    execCommand(ch, c);
    return 0;
}

int onEnv(ssh_session, ssh_channel, const char* name, const char* value, void* ud) {
    ChannelState* ch = st(ud);
    const std::string n = name ? name : "", v = value ? value : "";
    ch->env[n] = v;
    ch->conn->server->event("env:" + n + "=" + v);
    return 0;
}

int onSubsystem(ssh_session, ssh_channel, const char* subsystem, void* ud) {
    ChannelState* ch = st(ud);
    const std::string name = subsystem ? subsystem : "";
    ch->conn->server->event("subsystem:" + name);
    TestSshServer::Impl* s = ch->conn->server;
    if (name != "sftp" || !s->opts.enableSftp || s->opts.sftpRoot.empty() || ch->mode != ChannelState::Mode::None)
        return -1;
    ch->mode = ChannelState::Mode::Sftp;
    ch->sftp = std::make_unique<SftpServer>(s->opts.sftpRoot);
    return 0;
}

void onX11(ssh_session, ssh_channel, int, const char* proto, const char*, uint32_t screen, void* ud) {
    st(ud)->conn->server->event("x11-req:" + std::string(proto ? proto : "") + ":" + std::to_string(screen));
}

void onAuthAgent(ssh_session, ssh_channel, void* ud) { st(ud)->conn->server->event("auth-agent-req"); }

ChannelState* addChannel(Connection* c, ssh_channel channel, ChannelState::Kind kind) {
    auto ch = std::make_unique<ChannelState>();
    ch->conn = c;
    ch->channel = channel;
    ch->kind = kind;
    ssh_callbacks_init(&ch->cb);
    ch->cb.userdata = ch.get();
    ch->cb.channel_data_function = onData;
    ch->cb.channel_eof_function = onEof;
    ch->cb.channel_close_function = onClose;
    if (kind == ChannelState::Kind::Session) {
        ch->cb.channel_signal_function = onSignal;
        ch->cb.channel_pty_request_function = onPty;
        ch->cb.channel_pty_window_change_function = onWindowChange;
        ch->cb.channel_shell_request_function = onShell;
        ch->cb.channel_exec_request_function = onExec;
        ch->cb.channel_env_request_function = onEnv;
        ch->cb.channel_subsystem_request_function = onSubsystem;
        ch->cb.channel_x11_req_function = onX11;
        ch->cb.channel_auth_agent_req_function = onAuthAgent;
    }
    ssh_set_channel_callbacks(channel, &ch->cb);
    ChannelState* raw = ch.get();
    c->channels.push_back(std::move(ch));
    return raw;
}

/* ---- message callback (auth, channel opens, global requests) ---- */

int onMessage(ssh_session session, ssh_message msg, void* ud) {
    auto* c = static_cast<Connection*>(ud);
    TestSshServer::Impl* s = c->server;
    const int type = ssh_message_type(msg);
    const int sub = ssh_message_subtype(msg);

    if (type == SSH_REQUEST_AUTH) {
        const char* user = ssh_message_auth_user(msg);
        std::string u = user ? user : "";
        if (sub == SSH_AUTH_METHOD_INTERACTIVE && ssh_message_auth_kbdint_is_response(msg) && u.empty()) u = c->kbdUser;
        const bool userOk = u == s->opts.username;
        auto fail = [&](const std::string& ev) {
            s->event(ev);
            ssh_message_auth_set_methods(msg, s->authMethods());
            ssh_message_reply_default(msg);
            return 0;
        };
        auto succeed = [&](const std::string& ev) {
            s->event(ev);
            c->user = u;
            c->authenticated = true;
            ssh_message_auth_reply_success(msg, 0);
            return 0;
        };
        switch (sub) {
            case SSH_AUTH_METHOD_PASSWORD: {
                const char* pw = ssh_message_auth_password(msg);
                if (s->opts.allowPassword && userOk && pw && s->opts.password == pw) return succeed("auth:password:ok");
                return fail("auth:password:fail");
            }
            case SSH_AUTH_METHOD_PUBLICKEY: {
                if (!s->opts.allowPublicKey || !userOk) return fail("auth:publickey:fail");
                ssh_key key = ssh_message_auth_pubkey(msg);
                bool known = false;
                for (ssh_key k : s->authorized)
                    if (key && ssh_key_cmp(k, key, SSH_KEY_CMP_PUBLIC) == 0) known = true;
                if (!known) return fail("auth:publickey:fail");
#pragma warning(suppress : 4996)  // deprecated but the simplest way to tell probe from signed request
                const auto state = ssh_message_auth_publickey_state(msg);
                if (state == SSH_PUBLICKEY_STATE_NONE) {
                    s->event("auth:publickey:probe");
                    ssh_message_auth_reply_pk_ok_simple(msg);
                    return 0;
                }
                if (state == SSH_PUBLICKEY_STATE_VALID) return succeed("auth:publickey:ok");
                return fail("auth:publickey:fail");
            }
            case SSH_AUTH_METHOD_INTERACTIVE: {
                if (!s->opts.allowKbdInt) return fail("auth:kbdint:fail");
                auto questions = s->opts.kbdIntQuestions;
                if (questions.empty()) questions = {{"Password: ", s->opts.password}};
                if (!ssh_message_auth_kbdint_is_response(msg)) {
                    std::vector<const char*> prompts;
                    std::vector<char> echo;
                    for (size_t i = 0; i < questions.size(); ++i) {
                        prompts.push_back(questions[i].first.c_str());
                        echo.push_back(i < s->opts.kbdIntEcho.size() && s->opts.kbdIntEcho[i] ? 1 : 0);
                    }
                    c->kbdUser = u;
                    s->event("auth:kbdint:request");
                    ssh_message_auth_interactive_request(msg, s->opts.kbdIntName.c_str(), s->opts.kbdIntInstruction.c_str(),
                                                         static_cast<unsigned>(prompts.size()), prompts.data(), echo.data());
                    return 0;
                }
                const int n = ssh_userauth_kbdint_getnanswers(session);
                bool ok = userOk && n == static_cast<int>(questions.size());
                for (int i = 0; ok && i < n; ++i) {
                    const char* a = ssh_userauth_kbdint_getanswer(session, static_cast<unsigned>(i));
                    ok = a && questions[static_cast<size_t>(i)].second == a;
                }
                return ok ? succeed("auth:kbdint:ok") : fail("auth:kbdint:fail");
            }
            case SSH_AUTH_METHOD_NONE:
            default:
                s->event("auth:none");
                ssh_message_auth_set_methods(msg, s->authMethods());
                ssh_message_reply_default(msg);
                return 0;
        }
    }

    if (!c->authenticated) return 1;  // default reply (reject)

    if (type == SSH_REQUEST_CHANNEL_OPEN) {
        if (sub == SSH_CHANNEL_SESSION) {
            ssh_channel channel = ssh_message_channel_request_open_reply_accept(msg);
            if (!channel) return 0;
            addChannel(c, channel, ChannelState::Kind::Session);
            s->event("channel:session");
            return 0;
        }
        if (sub == SSH_CHANNEL_DIRECT_TCPIP && s->opts.allowTcpForwarding) {
            const std::string host = ssh_message_channel_request_open_destination(msg);
            const int port = ssh_message_channel_request_open_destination_port(msg);
            s->event("direct-tcpip:" + host + ":" + std::to_string(port));
            net::Socket sock = net::connectTcp(host, port, 3000);
            if (!sock) return 1;  // open failure
            ssh_channel channel = ssh_message_channel_request_open_reply_accept(msg);
            if (!channel) return 0;
            net::setNonBlocking(sock.get(), true);
            ChannelState* ch = addChannel(c, channel, ChannelState::Kind::Tcp);
            ch->sock = std::move(sock);
            return 0;
        }
        return 1;
    }

    if (type == SSH_REQUEST_GLOBAL) {
        if (sub == SSH_GLOBAL_REQUEST_TCPIP_FORWARD && s->opts.allowTcpForwarding) {
            const char* addr = ssh_message_global_request_address(msg);
            const int port = ssh_message_global_request_port(msg);
            s->event("tcpip-forward:" + std::string(addr ? addr : "") + ":" + std::to_string(port));
            try {
                auto l = std::make_unique<Listener>();
                int bound = 0;
                l->sock = net::listenLoopback(port, bound);
                net::setNonBlocking(l->sock.get(), true);
                l->address = addr ? addr : "";
                l->port = bound;
                s->lastRemoteForward = bound;
                c->listeners.push_back(std::move(l));
                ssh_message_global_request_reply_success(msg, static_cast<uint16_t>(bound));
                return 0;
            } catch (const std::exception&) {
                return 1;
            }
        }
        if (sub == SSH_GLOBAL_REQUEST_CANCEL_TCPIP_FORWARD) {
            const int port = ssh_message_global_request_port(msg);
            s->event("cancel-tcpip-forward:" + std::to_string(port));
            std::erase_if(c->listeners, [&](const std::unique_ptr<Listener>& l) { return l->port == port; });
            ssh_message_global_request_reply_success(msg, 0);
            return 0;
        }
        if (sub == SSH_GLOBAL_REQUEST_KEEPALIVE) {
            s->event("keepalive");
            return 1;
        }
        return 1;
    }
    return 1;
}

/* ---- loop helpers ---- */

/** Flushes queued output; returns false when the channel is finished and can be dropped. */
bool pumpChannel(ChannelState* ch) {
    if (!ch->channel) return false;
    // TCP relay: socket -> channel, pending channel data -> socket.
    if (ch->kind == ChannelState::Kind::Tcp && ch->sock) {
        if (!ch->toSocket.empty()) {
            int n = send(ch->sock.get(), ch->toSocket.data(), static_cast<int>(std::min<size_t>(ch->toSocket.size(), 65536)), 0);
            if (n > 0) ch->toSocket.erase(0, static_cast<size_t>(n));
            else if (!net::wouldBlock()) ch->closeAfterFlush = true;
        }
        if (ch->chanEof && ch->toSocket.empty() && !ch->sockShutdown) {
            shutdown(ch->sock.get(), SD_SEND);
            ch->sockShutdown = true;
        }
        if (ch->out.size() < 256 * 1024) {
            char buf[16384];
            int n = recv(ch->sock.get(), buf, sizeof buf, 0);
            if (n > 0) ch->out.append(buf, static_cast<size_t>(n));
            else if (n == 0 || !net::wouldBlock()) ch->closeAfterFlush = true;
        }
    }
    if (ch->mode == ChannelState::Mode::ExecProc && ch->proc && !ch->closeAfterFlush) {
        if (ch->out.size() < 256 * 1024) ch->proc->takeOutput(ch->out, ch->errOut, 256 * 1024 - ch->out.size());
        int code = 0;
        if (ch->out.empty() && ch->errOut.empty() && ch->proc->finished(code)) finishChannel(ch, code);
    }
    while (!ch->errOut.empty() && !ch->remoteClosed) {
        uint32_t window = ssh_channel_window_size(ch->channel);
        if (window == 0) break;
        const size_t chunk = std::min<size_t>({ch->errOut.size(), window, 32768});
        int n = ssh_channel_write_stderr(ch->channel, ch->errOut.data(), static_cast<uint32_t>(chunk));
        if (n <= 0) break;
        ch->errOut.erase(0, static_cast<size_t>(n));
    }
    while (!ch->out.empty() && !ch->remoteClosed) {
        uint32_t window = ssh_channel_window_size(ch->channel);
        if (window == 0) break;
        const size_t chunk = std::min<size_t>({ch->out.size(), window, 32768});
        int n = ssh_channel_write(ch->channel, ch->out.data(), static_cast<uint32_t>(chunk));
        if (n <= 0) break;
        ch->out.erase(0, static_cast<size_t>(n));
    }
    if (ch->remoteClosed) return false;
    if (ch->closeAfterFlush && ch->out.empty() && ch->errOut.empty()) {
        if (!ch->eofSent) {
            ssh_channel_send_eof(ch->channel);
            ch->eofSent = true;
        }
        ssh_channel_close(ch->channel);
        return false;
    }
    return true;
}

void acceptForwards(Connection* c) {
    for (auto& l : c->listeners) {
        net::Socket client = net::acceptNonBlocking(l->sock.get());
        if (!client) continue;
        auto [peerHost, peerPort] = net::peerAddress(client.get());
        ssh_channel channel = ssh_channel_new(c->session);
        if (!channel) continue;
        // Blocking open is fine here: the client answers promptly.
        if (ssh_channel_open_reverse_forward(channel, l->address.empty() ? "127.0.0.1" : l->address.c_str(), l->port,
                                             peerHost.c_str(), peerPort) != SSH_OK) {
            ssh_channel_free(channel);
            continue;
        }
        c->server->event("forwarded-tcpip:" + std::to_string(l->port));
        net::setNonBlocking(client.get(), true);
        ChannelState* ch = addChannel(c, channel, ChannelState::Kind::Tcp);
        ch->sock = std::move(client);
    }
}

}  // namespace

/* ------------------------------------------------------------------------ */
/* Server threads                                                            */
/* ------------------------------------------------------------------------ */

void TestSshServer::Impl::acceptLoop() {
    const socket_t fd = ssh_bind_get_fd(bind);
    while (!stopping) {
        if (!net::waitReadable(static_cast<SOCKET>(fd), 100)) continue;
        if (stopping) break;
        ssh_session session = ssh_new();
        if (!session) continue;
        if (ssh_bind_accept(bind, session) != SSH_OK) {
            ssh_free(session);
            continue;
        }
        std::lock_guard lock(connMutex);
        connThreads.emplace_back([this, session] { runConnection(session); });
    }
}

void TestSshServer::Impl::runConnection(ssh_session session) {
    Connection c;
    c.server = this;
    c.session = session;
    ssh_set_auth_methods(session, authMethods());
    // libssh only dispatches per-channel callbacks when server callbacks are installed; with no functions set,
    // everything else still reaches the message callback.
    ssh_server_callbacks_struct serverCb{};
    ssh_callbacks_init(&serverCb);
    serverCb.userdata = &c;
    ssh_set_server_callbacks(session, &serverCb);
    ssh_set_message_callback(session, onMessage, &c);

    if (ssh_handle_key_exchange(session) != SSH_OK) {
        event("kex:fail");
        ssh_disconnect(session);
        ssh_free(session);
        return;
    }
    event("connect");
    if (!opts.banner.empty()) {
        ssh_string b = ssh_string_from_char(opts.banner.c_str());
        ssh_send_issue_banner(session, b);
        ssh_string_free(b);
    }

    ssh_event ev = ssh_event_new();
    ssh_event_add_session(ev, session);
    const auto start = Clock::now();
    while (!stopping) {
        const int rc = ssh_event_dopoll(ev, 20);
        if (rc == SSH_ERROR) break;
        if (!ssh_is_connected(session)) break;
        if (!c.authenticated && Clock::now() - start > std::chrono::seconds(120)) break;
        acceptForwards(&c);
        for (auto it = c.channels.begin(); it != c.channels.end();) {
            if (!pumpChannel(it->get())) {
                ssh_remove_channel_callbacks((*it)->channel, &(*it)->cb);
                ssh_channel_free((*it)->channel);
                it = c.channels.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& ch : c.channels) {
        ssh_remove_channel_callbacks(ch->channel, &ch->cb);
        ssh_channel_free(ch->channel);
    }
    c.channels.clear();
    c.listeners.clear();
    ssh_event_remove_session(ev, session);
    ssh_event_free(ev);
    event("disconnect");
    ssh_disconnect(session);
    ssh_free(session);
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

TestSshServer::TestSshServer(TestSshServerOptions opts) : impl_(std::make_unique<Impl>()) {
    libsshInitOnce();
    net::ensureWinsock();
    Impl& s = *impl_;
    s.opts = std::move(opts);
    for (const auto& line : s.opts.authorizedKeys)
        if (ssh_key k = parseAuthorizedLine(line)) s.authorized.push_back(k);

    if (!s.opts.hostKeyFile.empty()) {
        if (ssh_pki_import_privkey_file(narrow(s.opts.hostKeyFile.wstring()).c_str(), nullptr, nullptr, nullptr, &s.hostKey) != SSH_OK)
            throw std::runtime_error("TestSshServer: cannot load host key file");
    } else {
        enum ssh_keytypes_e type = SSH_KEYTYPE_ED25519;
        int bits = 0;
        if (s.opts.hostKeyType == "rsa") {
            type = SSH_KEYTYPE_RSA;
            bits = 2048;
        } else if (s.opts.hostKeyType == "ecdsa") {
            type = SSH_KEYTYPE_ECDSA_P256;
            bits = 256;
        }
        if (ssh_pki_generate(type, bits, &s.hostKey) != SSH_OK) throw std::runtime_error("TestSshServer: keygen failed");
    }
    const std::string blob = keyBlob(s.hostKey);
    s.hostKeyTypeName = crypto::keyTypeOfBlob(blob);
    s.hostKeyB64 = crypto::base64Encode(blob);
    s.hostKeyFp = crypto::fingerprintSha256(blob);

    s.bind = ssh_bind_new();
    ssh_bind_options_set(s.bind, SSH_BIND_OPTIONS_BINDADDR, "127.0.0.1");
    unsigned int port = static_cast<unsigned int>(s.opts.port);
    ssh_bind_options_set(s.bind, SSH_BIND_OPTIONS_BINDPORT, &port);
    if (ssh_bind_options_set(s.bind, SSH_BIND_OPTIONS_IMPORT_KEY, s.hostKey) != SSH_OK)
        throw std::runtime_error("TestSshServer: cannot set host key");
    if (ssh_bind_listen(s.bind) != SSH_OK)
        throw std::runtime_error(std::string("TestSshServer: listen failed: ") + ssh_get_error(s.bind));
    s.port = net::localPort(static_cast<SOCKET>(ssh_bind_get_fd(s.bind)));
    s.acceptThread = std::thread([this] { impl_->acceptLoop(); });
}

TestSshServer::~TestSshServer() {
    stop();
    for (ssh_key k : impl_->authorized) ssh_key_free(k);
    // the host key is owned by the bind after IMPORT_KEY
}

int TestSshServer::port() const { return impl_->port; }
std::string TestSshServer::hostKeyType() const { return impl_->hostKeyTypeName; }
std::string TestSshServer::hostKeyBase64() const { return impl_->hostKeyB64; }
std::string TestSshServer::hostKeyFingerprint() const { return impl_->hostKeyFp; }
int TestSshServer::lastRemoteForwardPort() const { return impl_->lastRemoteForward.load(); }

std::vector<std::string> TestSshServer::events() const {
    std::lock_guard lock(impl_->evMutex);
    return impl_->events;
}

bool TestSshServer::waitForEvent(const std::string& prefix, int timeoutMs) const {
    std::unique_lock lock(impl_->evMutex);
    return impl_->evCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
        for (const auto& e : impl_->events)
            if (startsWith(e, prefix)) return true;
        return false;
    });
}

void TestSshServer::stop() {
    Impl& s = *impl_;
    if (s.stopping.exchange(true)) return;
    if (s.acceptThread.joinable()) s.acceptThread.join();
    std::list<std::thread> threads;
    {
        std::lock_guard lock(s.connMutex);
        threads.swap(s.connThreads);
    }
    for (auto& t : threads)
        if (t.joinable()) t.join();
    if (s.bind) {
        ssh_bind_free(s.bind);
        s.bind = nullptr;
    }
}

}  // namespace st::test
