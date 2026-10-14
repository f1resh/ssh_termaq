// SSH jump host: authenticate to the jump session, open a direct-tcpip channel to the target and relay it through
// a loopback socket pair so the main transport can treat it like a normal TCP socket.
#include "ssh/SshJump.h"

#include "core/Log.h"
#include "net/Proxy.h"
#include "net/Socket.h"
#include "ssh/SshClient.h"
#include "ssh/SshUtil.h"

#include <libssh/callbacks.h>

#include <memory>
#include <thread>

namespace st {

using namespace ssh;

namespace {

struct JumpRelay {
    SessionPtr session;
    ssh_channel channel = nullptr;
    SOCKET sock = INVALID_SOCKET;  // our end of the pair
    std::string toSock, toChan;
    bool chanEof = false, sockEof = false, dead = false;
    ssh_channel_callbacks_struct cb{};
};

int onChanData(ssh_session, ssh_channel, void* data, uint32_t len, int, void* ud) {
    static_cast<JumpRelay*>(ud)->toSock.append(static_cast<const char*>(data), len);
    return static_cast<int>(len);
}
void onChanEof(ssh_session, ssh_channel, void* ud) { static_cast<JumpRelay*>(ud)->chanEof = true; }
void onChanClose(ssh_session, ssh_channel, void* ud) {
    auto* r = static_cast<JumpRelay*>(ud);
    r->chanEof = true;
    r->dead = true;
}
int onSock(socket_t, int, void* ud) {
    auto* r = static_cast<JumpRelay*>(ud);
    if (r->toChan.size() > 512 * 1024) return 0;
    char buf[16384];
    int n = recv(r->sock, buf, sizeof buf, 0);
    if (n > 0) r->toChan.append(buf, static_cast<size_t>(n));
    else if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) r->sockEof = true;
    return 0;
}

void relayLoop(std::unique_ptr<JumpRelay> r) {
    ssh_session s = r->session.get();
    ssh_event ev = ssh_event_new();
    ssh_event_add_session(ev, s);
    ssh_event_add_fd(ev, static_cast<socket_t>(r->sock), POLLIN, onSock, r.get());
    ssh_set_blocking(s, 0);
    while (!r->dead) {
        if (!r->toSock.empty()) {
            int n = send(r->sock, r->toSock.data(), static_cast<int>(std::min<size_t>(r->toSock.size(), 65536)), 0);
            if (n > 0) r->toSock.erase(0, static_cast<size_t>(n));
            else if (WSAGetLastError() != WSAEWOULDBLOCK) break;
        }
        if (r->chanEof && r->toSock.empty()) shutdown(r->sock, SD_SEND);
        while (!r->toChan.empty()) {
            uint32_t window = ssh_channel_window_size(r->channel);
            if (window == 0) break;
            int n = ssh_channel_write(r->channel, r->toChan.data(),
                                      static_cast<uint32_t>(std::min<size_t>({r->toChan.size(), window, 32768})));
            if (n <= 0) break;
            r->toChan.erase(0, static_cast<size_t>(n));
        }
        if (r->sockEof && r->toChan.empty()) {
            if (!ssh_channel_is_eof(r->channel)) ssh_channel_send_eof(r->channel);
            if (r->chanEof) break;
        }
        if (ssh_event_dopoll(ev, 200) == SSH_ERROR || !ssh_is_connected(s)) break;
    }
    ssh_event_remove_fd(ev, static_cast<socket_t>(r->sock));
    ssh_event_remove_session(ev, s);
    ssh_event_free(ev);
    ssh_remove_channel_callbacks(r->channel, &r->cb);
    ssh_set_blocking(s, 1);
    if (ssh_channel_is_open(r->channel)) ssh_channel_close(r->channel);
    ssh_channel_free(r->channel);
    closesocket(r->sock);
    if (ssh_is_connected(s)) ssh_disconnect(s);
}

}  // namespace

SOCKET openSshJumpTunnel(const TransportContext& ctx, const SessionConfig& jumpIn, const std::string& host, int port,
                         std::string& error, const std::atomic<bool>* cancel) {
    static const std::atomic<bool> never{false};
    const std::atomic<bool>& cancelRef = cancel ? *cancel : never;
    ensureInit();

    SessionConfig jump = jumpIn;
    if (jump.proxy.type == ProxyType::Ssh) jump.proxy.type = ProxyType::None;  // no nested jump chains (loop guard)
    if (jump.host.empty()) {
        error = "Jump host has no host name";
        return INVALID_SOCKET;
    }
    TransportContext jctx = ctx;
    jctx.session = jump;
    jctx.oneTimePassword.clear();

    auto log = [&](LogLevel l, const std::string& m) {
        if (ctx.listener) ctx.listener->onLog(l, "Jump: " + m);
    };
    log(LogLevel::Info, "Connecting to jump host " + jump.host + ":" + std::to_string(jump.port));
    SOCKET jsock = openConnection(jctx, jump.host, jump.port > 0 ? jump.port : 22, error, cancel);
    if (jsock == INVALID_SOCKET) {
        error = "Jump host: " + error;
        return INVALID_SOCKET;
    }
    SshConnector connector(jctx, jump, "Jump: ", cancelRef);
    ConnectResult cr = connector.run(jsock, SSH_LOG_NOLOG);
    // The relay thread below outlives the caller's listener: stop routing proxy-command output to it.
    detachConnection(jsock, !cr.session);
    if (!cr.session) {
        error = "Jump host: " + cr.error;
        return INVALID_SOCKET;
    }
    auto relay = std::make_unique<JumpRelay>();
    relay->session = std::move(cr.session);
    relay->channel = ssh_channel_new(relay->session.get());
    if (!relay->channel || ssh_channel_open_forward(relay->channel, host.c_str(), port, "127.0.0.1", 0) != SSH_OK) {
        error = "Jump host refused to connect to " + host + ":" + std::to_string(port) + ": " +
                sshError(relay->session.get(), "rejected");
        if (relay->channel) ssh_channel_free(relay->channel);
        return INVALID_SOCKET;
    }
    log(LogLevel::Info, "Opened tunnel to " + host + ":" + std::to_string(port));
    ssh_callbacks_init(&relay->cb);
    relay->cb.userdata = relay.get();
    relay->cb.channel_data_function = onChanData;
    relay->cb.channel_eof_function = onChanEof;
    relay->cb.channel_close_function = onChanClose;
    ssh_set_channel_callbacks(relay->channel, &relay->cb);

    SOCKET a = INVALID_SOCKET, b = INVALID_SOCKET;
    if (!makeSocketPair(a, b, error)) {
        ssh_channel_free(relay->channel);
        return INVALID_SOCKET;
    }
    setNonBlocking(a, true);
    relay->sock = a;
    std::thread(relayLoop, std::move(relay)).detach();
    return b;
}

}  // namespace st
