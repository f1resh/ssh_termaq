#pragma once
// RemoteFs over an SFTP channel (libssh sftp_session) on an existing SSH connection.
#include "sftp/RemoteFs.h"

#include <memory>

namespace st {
class Transport;
}

namespace st::sftpimpl {

class SftpFs;

/**
 * Opens an SFTP channel on `transport` (must be a connected SSH transport). Throws ConnectionLost, or SftpError when
 * the server refuses the SFTP subsystem (the caller may fall back to SCP).
 */
std::shared_ptr<RemoteFs> openSftpFs(const std::weak_ptr<Transport>& transport);

}  // namespace st::sftpimpl
