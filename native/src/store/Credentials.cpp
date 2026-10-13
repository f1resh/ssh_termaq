// STUB — to be implemented (owner: host/store module).
#include "store/Credentials.h"

namespace sc {

Credentials::Credentials(std::filesystem::path file) : file_(std::move(file)) {}
void Credentials::load() {}
std::optional<std::string> Credentials::get(const std::string&) const { return std::nullopt; }
void Credentials::set(const std::string&, const std::string&) {}
void Credentials::remove(const std::string&) {}
bool Credentials::has(const std::string&) const { return false; }
std::string Credentials::protect(const std::string& plain) { return plain; }
std::optional<std::string> Credentials::unprotect(const std::string& b64) { return b64; }
void Credentials::saveLocked() const {}

}  // namespace sc
