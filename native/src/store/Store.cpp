// STUB — to be implemented (owner: host/store module).
#include "store/Store.h"

#include <stdexcept>

namespace sc {

Store::Store(std::filesystem::path dataDir)
    : dir_(std::move(dataDir)), knownHosts_(dir_ / "known_hosts.json"), credentials_(dir_ / "credentials.json") {}
Store::~Store() = default;
void Store::load() {}
Json Store::listSessionsJson() const { return Json::array(); }
std::vector<SessionConfig> Store::listSessions() const { return {}; }
std::optional<SessionConfig> Store::getSession(const std::string&) const { return std::nullopt; }
std::optional<Json> Store::getSessionJson(const std::string&) const { return std::nullopt; }
Json Store::saveSession(const Json& session) { return session; }
SessionConfig Store::saveSession(const SessionConfig& session) { return session; }
void Store::saveSessions(const Json&) {}
void Store::deleteSessions(const std::vector<std::string>&) {}
std::vector<std::string> Store::folders() const { return {}; }
void Store::createFolder(const std::string&) {}
void Store::renameFolder(const std::string&, const std::string&) {}
void Store::deleteFolder(const std::string&, bool) {}
void Store::touchSession(const std::string&) {}
bool Store::sessionNameExists(const std::string&, const std::string&) const { return false; }
Json Store::settings() const { return Json::object(); }
Json Store::updateSettings(const Json& patch) { return patch; }
SessionConfig Store::defaultSessionTemplate() const { return SessionConfig{}; }
std::filesystem::path Store::logDirectory() const { return dir_ / "logs"; }
Json Store::getState(const std::string&) const { return nullptr; }
void Store::setState(const std::string&, const Json&) {}

void writeFileAtomic(const std::filesystem::path&, const std::string&) { throw std::logic_error("not implemented"); }
std::optional<std::string> readFile(const std::filesystem::path&) { return std::nullopt; }

}  // namespace sc
