// STUB — to be implemented (owner: network/transports module).
#include "session/LineDiscipline.h"

namespace sc {

LineDiscipline::LineDiscipline(TriState echo, TriState edit, LdiscModes transportModes, Sinks sinks)
    : echo_(echo), edit_(edit), modes_(transportModes), sinks_(std::move(sinks)) {}
void LineDiscipline::setTransportModes(LdiscModes modes) { modes_ = modes; }
void LineDiscipline::input(const std::string& utf8) {
    if (sinks_.toRemote) sinks_.toRemote(utf8);
}
bool LineDiscipline::echoing() const { return echo_ == TriState::On || (echo_ == TriState::Auto && modes_.echo); }
bool LineDiscipline::editing() const { return edit_ == TriState::On || (edit_ == TriState::Auto && modes_.edit); }

}  // namespace sc
