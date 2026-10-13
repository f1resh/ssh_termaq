// STUB — to be implemented (owner: network/transports module).
#include "net/Proxy.h"

#include "net/Socket.h"

namespace sc {

namespace {
SshJumpOpener g_jumpOpener;
}
void setSshJumpOpener(SshJumpOpener opener) { g_jumpOpener = std::move(opener); }

SOCKET openConnection(const TransportContext& ctx, const std::string& host, int port, std::string& error,
                      const std::atomic<bool>* cancel) {
    ConnectOptions o;
    o.family = ctx.session.connection.addressFamily;
    o.timeoutMs = ctx.session.connection.connectTimeoutSeconds * 1000;
    o.noDelay = ctx.session.connection.tcpNoDelay;
    o.keepAlive = ctx.session.connection.tcpKeepalive;
    return connectTcp(host, port, o, error, cancel);
}

bool proxyExcludes(const ProxyConfig&, const std::string&) { return false; }

std::string expandProxyTemplate(const std::string& tmpl, const ProxyConfig&, const std::string&, int) { return tmpl; }

}  // namespace sc
