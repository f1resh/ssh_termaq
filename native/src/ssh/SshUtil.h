#pragma once
// Internal helpers of the SSH module: RAII wrappers for libssh handles, per-thread routing of
// libssh's log output, algorithm-list filtering, PTY mode encoding and small text/parse utilities.
#include "core/Types.h"

#include <libssh/callbacks.h>
#include <libssh/libssh.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace st::ssh {

/* ---- RAII ------------------------------------------------------------------------------------------ */

struct SessionDeleter {
    void operator()(ssh_session s) const noexcept { ssh_free(s); }
};
struct ChannelDeleter {
    void operator()(ssh_channel c) const noexcept { ssh_channel_free(c); }
};
struct KeyDeleter {
    void operator()(ssh_key k) const noexcept { ssh_key_free(k); }
};
struct EventDeleter {
    void operator()(ssh_event e) const noexcept { ssh_event_free(e); }
};
struct SshCharDeleter {
    void operator()(char* p) const noexcept { ssh_string_free_char(p); }
};
using SessionPtr = std::unique_ptr<std::remove_pointer_t<ssh_session>, SessionDeleter>;
using ChannelPtr = std::unique_ptr<std::remove_pointer_t<ssh_channel>, ChannelDeleter>;
using KeyPtr = std::unique_ptr<std::remove_pointer_t<ssh_key>, KeyDeleter>;
using EventPtr = std::unique_ptr<std::remove_pointer_t<ssh_event>, EventDeleter>;
using SshCharPtr = std::unique_ptr<char, SshCharDeleter>;

/* ---- libssh process / thread setup ----------------------------------------------------------------- */

/** ssh_init() once per process (thread-safe). */
void ensureInit();

using LogFn = std::function<void(LogLevel, const std::string&)>;

/**
 * Routes libssh log output produced on the current thread to `fn` (as LogLevel::Debug) while alive.
 * libssh keeps its log level and callback in thread-local storage, so every connection thread installs
 * its own scope. When `debug` is false only libssh warnings are collected (into the application log).
 * Scopes nest (the jump-host code runs inside a transport's scope).
 */
class ThreadLogScope {
public:
    ThreadLogScope(LogFn fn, bool debug);
    ~ThreadLogScope();
    ThreadLogScope(const ThreadLogScope&) = delete;
    ThreadLogScope& operator=(const ThreadLogScope&) = delete;
    /** libssh verbosity matching the scope (also applied to sessions via SSH_OPTIONS_LOG_VERBOSITY). */
    int level() const { return level_; }

    struct State;

private:
    std::unique_ptr<State> state_;
    State* prev_ = nullptr;
    int prevLevel_ = 0;
    int level_ = 0;
};

/* ---- protocol helpers ------------------------------------------------------------------------------ */

/** Names of `wanted` that libssh supports for `type` (order kept, duplicates removed); others go to `dropped`. */
std::vector<std::string> filterAlgorithms(const std::vector<std::string>& wanted, ssh_kex_types_e type,
                                          std::vector<std::string>& dropped);
/** Host key algorithm names that verify keys of `keyType` ("ssh-rsa" -> rsa-sha2-512, rsa-sha2-256, ssh-rsa). */
std::vector<std::string> hostKeyAlgorithmsForKeyType(const std::string& keyType);
/** libssh's default host key algorithm preference (filtered to what this build supports). */
std::vector<std::string> defaultHostKeyAlgorithms();

/** Encoded terminal modes (RFC 4254 section 8) as PuTTY sends them: ERASE from the backspace setting,
 *  IUTF8 for UTF-8 sessions and the terminal speeds ("38400,38400" = output,input). Ends with TTY_OP_END. */
std::string encodePtyModes(const SessionConfig& s, int* ospeed = nullptr, int* ispeed = nullptr);

/** Last libssh error of `obj` (session/bind), or `fallback` when empty. */
std::string sshError(void* obj, const std::string& fallback);

/** Normalizes line endings to CRLF (for text written to the terminal). */
std::string toCrlf(std::string_view text);

/** Overwrites a string's buffer before it is released (passwords / passphrases). */
void wipe(std::string& secret);

/** Big-endian helpers. */
inline uint32_t getU32(const unsigned char* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline void putU32(std::string& out, uint32_t v) {
    out.push_back(char((v >> 24) & 0xff));
    out.push_back(char((v >> 16) & 0xff));
    out.push_back(char((v >> 8) & 0xff));
    out.push_back(char(v & 0xff));
}

}  // namespace st::ssh
