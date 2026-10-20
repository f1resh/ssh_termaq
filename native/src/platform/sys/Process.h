#pragma once
// Local proxy commands (PuTTY's "Local" proxy type): a shell command whose stdin/stdout carry the connection.
#include "platform/sys/Net.h"

#include <functional>
#include <string>

namespace st::sys {

/**
 * Starts `command` through the OS shell (Windows: "%ComSpec%" /d /s /c) with its stdin and stdout relayed to the
 * returned socket; each line it writes to stderr goes to `onStderrLine` (called from a background thread). The command
 * and the processes it starts end when the socket is closed with closeProxyCommandSocket(), detached with terminate,
 * or its other end stops reading. Only the command's own stdio is inherited. kInvalidSocket + `error` on failure.
 */
SocketHandle startProxyCommand(const std::string& command, std::function<void(const std::string& line)> onStderrLine,
                               std::string& error);
/**
 * For a socket from startProxyCommand: no onStderrLine calls happen once this returns; `terminate` also ends the
 * command (and forgets the socket). No-op for any other socket.
 */
void detachProxyCommand(SocketHandle s, bool terminate);
/** Closes `s` (any socket); one from startProxyCommand is detached first and its command ended afterwards. */
void closeProxyCommandSocket(SocketHandle& s);

}  // namespace st::sys
