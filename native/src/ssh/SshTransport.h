#pragma once
// SSH-2 transport built on libssh (0.12).
//
// Responsibilities (see docs/ARCHITECTURE.md for the full list):
//  - connect via openConnection() (proxy aware) and hand the socket to libssh (SSH_OPTIONS_FD)
//  - algorithm preferences (kex/ciphers/hostkeys/macs, filtered to what libssh supports), compression
//  - host key verification against KnownHosts (logicalHostname if set) with "hostkey" prompts
//  - authentication order: none -> agent (Pageant / OpenSSH agent via a loopback relay + ssh_set_agent_socket)
//    -> private key file (OpenSSH/PEM/PPK v2/v3 through keys/KeyLoader) -> keyboard-interactive -> password;
//    username prompt when empty; saved password (Credentials) / one-time password; "remember" support;
//    banner shown via onLocalOutput when showBanner
//  - PTY (TERM, size, terminal speed modes), env vars, shell or remote command, or no shell at all
//  - keepalives, window resize, exit-status / exit-signal reporting
//  - port forwarding: local (-L), remote (-R), dynamic SOCKS4/5 (-D); X11 forwarding; agent forwarding
//  - special commands: break, eof, ignore, rekey (if supported), signal:* (INT TERM KILL HUP QUIT USR1 USR2 ...)
//  - post(): run tasks with exclusive access to the ssh_session on the connection thread (used by SFTP)
#include "transport/Transport.h"

#include <libssh/libssh.h>

#include <functional>
#include <memory>

namespace sc {

class SshTransport : public Transport {
public:
    ~SshTransport() override = default;
    SshTransport* asSsh() override { return this; }

    /**
     * Queue `task` to run on the connection thread, between I/O polls, with exclusive access to the session.
     * Tasks run in FIFO order. If the connection is not (or no longer) established, the task is invoked with
     * nullptr (so callers can fail their pending operation). Tasks must not block for long (>~50 ms);
     * long operations (file transfers) must be split into steps that re-post themselves.
     */
    virtual void post(std::function<void(ssh_session session)> task) = 0;
    virtual bool isConnected() const = 0;
};

std::unique_ptr<Transport> makeSshTransport(TransportContext ctx);

}  // namespace sc
