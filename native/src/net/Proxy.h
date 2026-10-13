#pragma once
// Establishes the byte stream to host:port for TCP based transports, honouring session.proxy:
//   none    : direct connectTcp
//   socks4  : SOCKS4 / SOCKS4a (4a when DNS at proxy)        socks5: RFC 1928 + RFC 1929 user/pass auth
//   http    : HTTP CONNECT with Basic auth                    telnet: PuTTY telnet-proxy command template
//   command : local proxy command (CreateProcess, stdio relayed through a loopback socket pair)
//   ssh     : SSH jump host (openSshJumpTunnel)
// Exclusions (excludeHosts patterns, localhost unless proxyLocalhost) bypass the proxy.
// Always returns a connected, blocking SOCKET (relay sockets for command/ssh).
#include "core/Win.h"
#include "transport/Transport.h"

#include <atomic>
#include <functional>
#include <string>

namespace sc {

/** Signature of the SSH jump-host tunnel opener (ssh/SshJump.h: openSshJumpTunnel). Injected to avoid a
 *  dependency from the network layer on the SSH module; registered once by transport/TransportFactory. */
using SshJumpOpener = std::function<SOCKET(const TransportContext& ctx, const SessionConfig& jump, const std::string& host,
                                           int port, std::string& error, const std::atomic<bool>* cancel)>;
void setSshJumpOpener(SshJumpOpener opener);

SOCKET openConnection(const TransportContext& ctx, const std::string& host, int port, std::string& error,
                      const std::atomic<bool>* cancel);

/** True when host should bypass the proxy per the config (exported for tests). */
bool proxyExcludes(const ProxyConfig& proxy, const std::string& host);

/** PuTTY template substitution: %host %port %user %pass %proxyhost %proxyport %% and \n \r \t \\ \xHH escapes. */
std::string expandProxyTemplate(const std::string& tmpl, const ProxyConfig& proxy, const std::string& host, int port);

}  // namespace sc
