// STUB — to be implemented (owner: host/store module).
#include "store/KnownHosts.h"

namespace sc {

KnownHosts::KnownHosts(std::filesystem::path file) : file_(std::move(file)) {}
void KnownHosts::load() {}
std::vector<KnownHost> KnownHosts::list() const { return {}; }
std::vector<KnownHost> KnownHosts::find(const std::string&, int) const { return {}; }
HostKeyCheck KnownHosts::check(const std::string&, int, const std::string&, const std::string&) const { return HostKeyCheck::Unknown; }
KnownHost KnownHosts::add(KnownHost entry) { return entry; }
int KnownHosts::addMany(const std::vector<KnownHost>&) { return 0; }
void KnownHosts::remove(const std::vector<std::string>&) {}
void KnownHosts::saveLocked() const {}

}  // namespace sc
