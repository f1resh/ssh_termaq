// STUB — to be implemented (owner: SFTP module).
#include "sftp/SftpService.h"

#include "bridge/Bridge.h"
#include "session/SessionManager.h"

namespace sc {

struct SftpService::Impl {};

SftpService::SftpService(SessionManager&, Bridge&) : impl_(std::make_unique<Impl>()) {}
SftpService::~SftpService() = default;
void SftpService::registerHandlers() {}
void SftpService::closeAll() {}

}  // namespace sc
