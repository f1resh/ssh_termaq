#pragma once
// RemoteFs over SCP for servers without an SFTP subsystem: folders are browsed and changed with POSIX shell commands
// (`ls -la`, mkdir, mv, rm, chmod, touch) run on exec channels, files move with the classic scp protocol
// (`scp -f` / `scp -t`, one channel per file). No resume; times may only be precise to the minute.
#include "sftp/RemoteFs.h"

#include <memory>

namespace st {
class Transport;
}

namespace st::sftpimpl {

/** Probes the server (shell, `scp`, `ls` flavour, time zone). Throws ConnectionLost or SftpError. */
std::shared_ptr<RemoteFs> openScpFs(const std::weak_ptr<Transport>& transport);

}  // namespace st::sftpimpl
