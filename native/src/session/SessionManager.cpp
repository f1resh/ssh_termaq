// STUB — to be implemented (owner: session module).
#include "session/SessionManager.h"

#include "bridge/Bridge.h"
#include "store/Store.h"
#include "transport/Transport.h"

#include <stdexcept>

namespace sc {

struct SessionManager::Impl {};

SessionManager::SessionManager(Store&, Bridge&) : impl_(std::make_unique<Impl>()) {}
SessionManager::~SessionManager() = default;
void SessionManager::open(const ConnOpenRequest&) { throw std::logic_error("SessionManager not implemented"); }
void SessionManager::write(const std::string&, const std::string&, bool) {}
void SessionManager::resize(const std::string&, int, int, int, int) {}
void SessionManager::close(const std::string&) {}
void SessionManager::special(const std::string&, const std::string&) {}
std::vector<SpecialCommandDef> SessionManager::specials(const std::string&) const { return {}; }
std::vector<EventLogEntry> SessionManager::eventLog(const std::string&) const { return {}; }
void SessionManager::respondPrompt(const std::string&, const PromptResponse&) {}
std::shared_ptr<Transport> SessionManager::transport(const std::string&) const { return nullptr; }
int SessionManager::activeCount() const { return 0; }
void SessionManager::closeAll() {}

void registerConnHandlers(Bridge&, SessionManager&) {}

}  // namespace sc
