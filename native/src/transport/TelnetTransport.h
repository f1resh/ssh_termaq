#pragma once
// Telnet (RFC 854) with option negotiation: ECHO, SGA, NAWS, TTYPE, TSPEED, NEW-ENVIRON (and OLD-ENVIRON), BINARY; active/passive negotiation; telnet special commands (AYT, Break, Synch, EC, EL, GA, NOP, Abort Process, Abort Output, Interrupt Process, Suspend, End of Record, End of File); IAC escaping; CR NUL / CR LF handling per returnSendsNewline.
#include "transport/Transport.h"

#include <memory>

namespace st {

std::unique_ptr<Transport> makeTelnetTransport(TransportContext ctx);

}  // namespace st
