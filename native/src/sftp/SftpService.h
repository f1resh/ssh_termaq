#pragma once
// SFTP browser backend. Runs on top of an existing connected SSH connection: every libssh call is executed
// through SshTransport::post() (on the connection thread). Transfers are chunked (re-posting steps of
// <= 256 KiB) so the terminal stays responsive; progress is emitted as "sftp:onProgress" events
// (throttled to ~10/s per transfer, plus state changes).
#include "core/Types.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace st::dnd {
class VirtualSource;
}

namespace st {

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

    /** Tests: the virtual-file source sftp:startDrag would offer to Explorer. */
    std::shared_ptr<dnd::VirtualSource> dragSourceForTests(const std::string& sftpId, const std::vector<std::string>& paths);

    /** Tests: replaces the editor launch of sftp:edit (localPath, editor). */
    void setEditLauncherForTests(std::function<void(const std::string& localPath, const std::string& editor)> launcher);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace st
