// STUB — to be implemented (owner: session module).
#include "session/SessionLog.h"

namespace sc {

struct SessionLog::Impl {};
SessionLog::SessionLog(const SessionConfig&, std::filesystem::path) : impl_(std::make_unique<Impl>()) {}
SessionLog::~SessionLog() = default;
bool SessionLog::open(Prompter*, std::string&) { return true; }
bool SessionLog::isOpen() const { return false; }
std::filesystem::path SessionLog::path() const { return {}; }
void SessionLog::output(std::string_view, std::string_view) {}
void SessionLog::event(const std::string&) {}
void SessionLog::close() {}
std::string SessionLog::expandPath(const std::string& tmpl, const SessionConfig&, const std::tm&) { return tmpl; }
std::string SessionLog::stripEscapes(std::string_view text) { return std::string(text); }

}  // namespace sc
