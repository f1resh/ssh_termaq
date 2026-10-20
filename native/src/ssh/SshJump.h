#pragma once
// SSH jump host support for ProxyType::Ssh (PuTTY "SSH to proxy and use port forwarding").
#include "platform/sys/Net.h"
#include "transport/Transport.h"

#include <atomic>
#include <string>

namespace st {

/**
 * Connects + authenticates to `jump` (using ctx's prompter/knownHosts/credentials), opens a direct-tcpip
 * channel to host:port and returns one end of a loopback socket pair relayed to that channel by a
 * background thread. The jump connection is torn down when the returned socket is closed by the caller
 * or the channel closes. Blocking; call from a transport thread. kInvalidSocket + error on failure.
 * Event-log lines are reported through ctx.listener->onLog prefixed with "Jump: ".
 */
sys::SocketHandle openSshJumpTunnel(const TransportContext& ctx, const SessionConfig& jump, const std::string& host,
                                    int port, std::string& error, const std::atomic<bool>* cancel);

}  // namespace st
