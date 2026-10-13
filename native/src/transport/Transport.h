#pragma once
// Transport = one live connection (SSH / Telnet / Rlogin / Raw / Serial).
//
// Lifecycle & threading contract (all implementations MUST follow it):
//  - The factory constructs the transport; SessionManager then calls start() exactly once.
//  - start() returns immediately; the transport runs its own I/O thread(s).
//  - write()/resize()/sendSpecial()/close() are thread-safe and non-blocking (queue + wake the I/O thread).
//  - All listener callbacks are invoked from the transport's own thread(s), never from inside write()/close().
//  - listener->onClose() is invoked exactly once, after which no other callbacks are made.
//    It must be delivered even when start() fails early (connect error, auth failure, cancelled prompt).
//  - close() may be called at any time (also before connection completes); it must cancel blocking
//    operations (DNS/connect/prompts) promptly and lead to onClose().
//  - The destructor joins the I/O thread(s). It is never called from a listener callback.
#include "core/Types.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sc {

class KnownHosts;
class Credentials;
class Prompter;
class SshTransport;

struct CloseInfo {
    /** Clean = remote ended the session normally (exit status received / EOF) or user closed it. */
    bool clean = false;
    bool hasExitCode = false;
    int exitCode = 0;
    std::string exitSignal;
    /** Human readable reason ("Connection refused", "Remote side closed the connection", ...). */
    std::string reason;
    /** Was this the result of an error (connect/auth failure, network error)? */
    bool error = false;
};

/** Local echo / local line editing state requested by the transport (used when the session's TriState is 'auto'). */
struct LdiscModes {
    bool echo = false;
    bool edit = false;
};

class TransportListener {
public:
    virtual ~TransportListener() = default;
    /** Raw bytes received from the remote side (session charset; not yet decoded). */
    virtual void onData(std::string_view bytes) = 0;
    /** Progress: Connecting / Authenticating / Connected. (Disconnected/Error are reported via onClose.) */
    virtual void onStatus(ConnStatus status, const std::string& message) = 0;
    /** Event-log line (PuTTY "Event Log"). Debug level is used for verbose protocol tracing (ssh-debug logging). */
    virtual void onLog(LogLevel level, const std::string& message) = 0;
    /** Exactly once, last callback. */
    virtual void onClose(const CloseInfo& info) = 0;
    /** Transport negotiated different echo/edit behaviour (telnet ECHO/SGA). */
    virtual void onLdiscChanged(LdiscModes /*modes*/) {}
    /** Informational, e.g. "SSH-2.0-OpenSSH_9.6 | aes256-gcm@openssh.com | curve25519-sha256". */
    virtual void onServerInfo(const std::string& /*info*/) {}
    /** Text to show in the terminal that is not remote output (e.g. SSH banner, "Access denied"). UTF-8. */
    virtual void onLocalOutput(const std::string& /*utf8*/) {}
};

struct TransportContext {
    std::string connId;
    /** Fully resolved session (saved password NOT included; use credentials). */
    SessionConfig session;
    int cols = 80;
    int rows = 24;
    int widthPx = 0;
    int heightPx = 0;
    /** One-time password from CLI (-pw); empty = none. */
    std::string oneTimePassword;
    TransportListener* listener = nullptr;
    /** Blocking UI prompts (call from the transport thread only). */
    Prompter* prompter = nullptr;
    KnownHosts* knownHosts = nullptr;
    Credentials* credentials = nullptr;
    /** Look up a saved session by id (jump hosts). Thread-safe. */
    std::function<std::optional<SessionConfig>(const std::string& id)> resolveSession;
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual Protocol protocol() const = 0;
    virtual void start() = 0;
    virtual void write(std::string bytes) = 0;
    virtual void resize(int cols, int rows, int widthPx, int heightPx) = 0;
    virtual std::vector<SpecialCommandDef> specials() const = 0;
    virtual void sendSpecial(const std::string& id) = 0;
    virtual void close() = 0;
    /** Local echo/edit behaviour for TriState::Auto before any negotiation (ssh: off/off; raw: on/on; telnet: on/on). */
    virtual LdiscModes defaultLdisc() const = 0;
    /** Non-null only for SSH transports (used by SFTP). */
    virtual SshTransport* asSsh() { return nullptr; }
};

}  // namespace sc
