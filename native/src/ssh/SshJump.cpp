// SSH jump host: authenticate to the jump session, open a direct-tcpip channel to the target and relay it through
// a loopback socket pair so the main transport can treat it like a normal TCP socket.
#include "ssh/SshJump.h"

#include "core/Log.h"
#include "net/Proxy.h"
#include "platform/sys/Net.h"
#include "ssh/SshClient.h"
#include "ssh/SshUtil.h"

#include <libssh/callbacks.h>

#include <algorithm>
#include <memory>
#include <string>
#include <thread>

namespace st {

using namespace ssh;

namespace {

/** Bytes held per direction; beyond it the source waits (the socket is not read, the SSH window stays shut). */
constexpr size_t kRelayBuffer = 512 * 1024;

struct JumpRelay {
    SessionPtr session;
    ssh_channel channel = nullptr;
    sys::SocketHandle sock = sys::kInvalidSocket;  // our end of the pair
    std::string toSock, toChan;
    size_t chanPending = 0;  // channel data libssh still holds for us (not taken into toSock yet)
    bool chanEof = false, chanClosed = false, sockEof = false, sockShut = false;
    short events = 0;  // what sock is watched for in the event (0: not watched)
    ssh_channel_callbacks_struct cb{};
};

int onChanData(ssh_session, ssh_channel, void* data, uint32_t len, int, void* ud) {
    auto* r = static_cast<JumpRelay*>(ud);
    // Only what fits: libssh keeps the rest, and the SSH window stays shut until the loop takes it.
    const size_t take = std::min<size_t>(len, kRelayBuffer - std::min(kRelayBuffer, r->toSock.size()));
    r->toSock.append(static_cast<const char*>(data), take);
    r->chanPending = len - take;
    return static_cast<int>(take);
}
void onChanEof(ssh_session, ssh_channel, void* ud) { static_cast<JumpRelay*>(ud)->chanEof = true; }
void onChanClose(ssh_session, ssh_channel, void* ud) {
    auto* r = static_cast<JumpRelay*>(ud);
    r->chanEof = true;
    r->chanClosed = true;  // deliver what is left, then end
}
int onSock(socket_t, int, void* ud) {
    auto* r = static_cast<JumpRelay*>(ud);
    if (r->sockEof || r->toChan.size() >= kRelayBuffer) return 0;
    char buf[16384];
    const long n = sys::recvSome(r->sock, buf, sizeof buf);
    if (n > 0) r->toChan.append(buf, static_cast<size_t>(n));
    else if (n == 0 || sys::lastNetError() != sys::NetErr::WouldBlock) r->sockEof = true;
    return 0;
}

/** Watches the socket for exactly what the relay can use now: reading while there is room towards the channel (and no
 *  EOF yet, which would stay readable), writing while data waits for it, so it never wakes the loop for nothing. */
void watch(ssh_event ev, JumpRelay* r) {
    short want = 0;
    if (!r->sockEof && r->toChan.size() < kRelayBuffer) want |= sys::kPollIn;
    if (!r->toSock.empty()) want |= sys::kPollOut;
    if (want == r->events) return;
    if (r->events) ssh_event_remove_fd(ev, static_cast<socket_t>(r->sock));
    if (want) ssh_event_add_fd(ev, static_cast<socket_t>(r->sock), want, onSock, r);
    r->events = want;
}

void relayLoop(std::unique_ptr<JumpRelay> r) {
    ssh_session s = r->session.get();
    ssh_event ev = ssh_event_new();
    ssh_event_add_session(ev, s);
    ssh_set_blocking(s, 0);
    for (;;) {
        // Channel data libssh still holds, as far as there is room (it is in libssh's buffer: no packets are read).
        if (r->chanPending > 0 && r->toSock.size() < kRelayBuffer) {
            std::string buf(std::min(r->chanPending, kRelayBuffer - r->toSock.size()), '\0');
            const int n = ssh_channel_read_nonblocking(r->channel, buf.data(), static_cast<uint32_t>(buf.size()), 0);
            if (n > 0) {
                r->toSock.append(buf.data(), static_cast<size_t>(n));
                r->chanPending -= std::min(r->chanPending, static_cast<size_t>(n));
            } else {
                r->chanPending = 0;
            }
        }
        if (!r->toSock.empty()) {
            const long n = sys::sendSome(r->sock, r->toSock.data(), std::min<size_t>(r->toSock.size(), 65536));
            if (n > 0) r->toSock.erase(0, static_cast<size_t>(n));
            else if (sys::lastNetError() != sys::NetErr::WouldBlock) break;
        }
        const bool delivered = r->toSock.empty() && r->chanPending == 0;
        if (r->chanEof && delivered && !r->sockShut) {
            sys::shutdownWrite(r->sock);
            r->sockShut = true;
        }
        if (r->chanClosed && delivered) break;
        while (!r->chanClosed && !r->toChan.empty()) {
            uint32_t window = ssh_channel_window_size(r->channel);
            if (window == 0) break;
            int n = ssh_channel_write(r->channel, r->toChan.data(),
                                      static_cast<uint32_t>(std::min<size_t>({r->toChan.size(), window, 32768})));
            if (n <= 0) break;
            r->toChan.erase(0, static_cast<size_t>(n));
        }
        if (r->sockEof && r->toChan.empty()) {
            if (!r->chanClosed && !ssh_channel_is_eof(r->channel)) ssh_channel_send_eof(r->channel);
            if (r->chanEof && delivered) break;
        }
        watch(ev, r.get());
        if (ssh_event_dopoll(ev, 200) == SSH_ERROR || !ssh_is_connected(s)) break;
    }
    if (r->events) ssh_event_remove_fd(ev, static_cast<socket_t>(r->sock));
    ssh_event_remove_session(ev, s);
    ssh_event_free(ev);
    ssh_remove_channel_callbacks(r->channel, &r->cb);
    ssh_set_blocking(s, 1);
    if (ssh_channel_is_open(r->channel)) ssh_channel_close(r->channel);
    ssh_channel_free(r->channel);
    sys::closeSocket(r->sock);
    if (ssh_is_connected(s)) ssh_disconnect(s);
}

}  // namespace

sys::SocketHandle openSshJumpTunnel(const TransportContext& ctx, const SessionConfig& jumpIn, const std::string& host,
                                    int port, std::string& error, const std::atomic<bool>* cancel) {
    static const std::atomic<bool> never{false};
    const std::atomic<bool>& cancelRef = cancel ? *cancel : never;
    ensureInit();

    SessionConfig jump = jumpIn;
    if (jump.proxy.type == ProxyType::Ssh) jump.proxy.type = ProxyType::None;  // no nested jump chains (loop guard)
    if (jump.host.empty()) {
        error = "Jump host has no host name";
        return sys::kInvalidSocket;
    }
    TransportContext jctx = ctx;
    jctx.session = jump;
    jctx.oneTimePassword.clear();

    auto log = [&](LogLevel l, const std::string& m) {
        if (ctx.listener) ctx.listener->onLog(l, "Jump: " + m);
    };
    log(LogLevel::Info, "Connecting to jump host " + jump.host + ":" + std::to_string(jump.port));
    sys::SocketHandle jsock = openConnection(jctx, jump.host, jump.port > 0 ? jump.port : 22, error, cancel);
    if (jsock == sys::kInvalidSocket) {
        error = "Jump host: " + error;
        return sys::kInvalidSocket;
    }
    SshConnector connector(jctx, jump, "Jump: ", cancelRef);
    ConnectResult cr = connector.run(jsock, SSH_LOG_NOLOG);
    // The relay thread below outlives the caller's listener: stop routing proxy-command output to it.
    detachConnection(jsock, !cr.session);
    if (!cr.session) {
        error = "Jump host: " + cr.error;
        return sys::kInvalidSocket;
    }
    auto relay = std::make_unique<JumpRelay>();
    relay->session = std::move(cr.session);
    relay->channel = ssh_channel_new(relay->session.get());
    if (!relay->channel || ssh_channel_open_forward(relay->channel, host.c_str(), port, "127.0.0.1", 0) != SSH_OK) {
        error = "Jump host refused to connect to " + host + ":" + std::to_string(port) + ": " +
                sshError(relay->session.get(), "rejected");
        if (relay->channel) ssh_channel_free(relay->channel);
        return sys::kInvalidSocket;
    }
    log(LogLevel::Info, "Opened tunnel to " + host + ":" + std::to_string(port));
    ssh_callbacks_init(&relay->cb);
    relay->cb.userdata = relay.get();
    relay->cb.channel_data_function = onChanData;
    relay->cb.channel_eof_function = onChanEof;
    relay->cb.channel_close_function = onChanClose;
    ssh_set_channel_callbacks(relay->channel, &relay->cb);

    sys::SocketHandle a = sys::kInvalidSocket, b = sys::kInvalidSocket;
    if (!sys::makeSocketPair(a, b, error)) {
        ssh_channel_free(relay->channel);
        return sys::kInvalidSocket;
    }
    sys::setNonBlocking(a, true);
    relay->sock = a;
    std::thread(relayLoop, std::move(relay)).detach();
    return b;
}

}  // namespace st
