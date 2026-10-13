#pragma once
// SFTP browser backend. Runs on top of an existing connected SSH connection: every libssh call is executed
// through SshTransport::post() (on the connection thread). Transfers are chunked (re-posting steps of
// <= 256 KiB) so the terminal stays responsive; progress is emitted as "sftp:onProgress" events
// (throttled to ~10/s per transfer, plus state changes).
#include "core/Types.h"

#include <memory>
#include <string>
#include <vector>

namespace sc {

class Bridge;
class SessionManager;

class SftpService {
public:
    SftpService(SessionManager& sessions, Bridge& bridge);
    ~SftpService();

    /** Registers sftp:* bridge handlers (uses app/Dialogs for omitted local paths). */
    void registerHandlers();
    /** Closes all SFTP sessions (app shutdown). */
    void closeAll();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sc
