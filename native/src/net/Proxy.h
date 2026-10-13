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

/* ---- additions (net module) ------------------------------------------------------------------------ */

struct OpenOptions {
    /** Local source port range for DIRECT connections (rlogin privileged ports, see ConnectOptions). */
    int localPortHigh = 0;
    int localPortLow = 0;
    /** Receives a description of the established path for the event log, e.g. "127.0.0.1 port 23" or
     *  "proxy.example 1080 (SOCKS 5 proxy)". Optional. */
    std::string* connectedTo = nullptr;
    /** Receives true when the connection goes through a proxy (not excluded). Optional. */
    bool* viaProxy = nullptr;
};

/** openConnection with extra options. Event-log lines go to ctx.listener->onLog (listener may be null). */
SOCKET openConnection(const TransportContext& ctx, const std::string& host, int port, std::string& error,
                      const std::atomic<bool>* cancel, const OpenOptions& options);

/**
 * Closes a socket returned by openConnection. For relayed connections (local proxy command) it also detaches the
 * relay from ctx.listener (no proxy log callbacks happen after it returns) and terminates the proxy process tree.
 * Transports call this (instead of closesocket) before delivering onClose. No-op for INVALID_SOCKET.
 */
void closeConnection(SOCKET& s);

/**
 * Like closeConnection, but for sockets whose ownership moved elsewhere (libssh closes the SSH transport's socket
 * itself): detaches a local-command relay from ctx.listener and terminates its process, without closing `s`.
 * Call before the listener is destroyed. No-op for plain sockets. `terminate` = false only detaches the event-log
 * listener and leaves the proxy process running (the connection stays in use).
 */
void detachConnection(SOCKET s, bool terminate = true);

/** Whether host names are passed to the proxy unresolved (PuTTY "Do DNS name lookup at proxy end"; auto => yes
 *  for every proxy type except SOCKS 4). */
bool proxyResolvesRemotely(const ProxyConfig& proxy);

}  // namespace sc
