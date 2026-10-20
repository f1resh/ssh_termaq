#pragma once
// Rlogin (RFC 1282): privileged source port when possible (1023..512), "\0localuser\0remoteuser\0term/speed\0" handshake, window-size urgent-data protocol (0x80 control byte) and flow control commands.
#include "transport/Transport.h"

#include <memory>

namespace st {

std::unique_ptr<Transport> makeRloginTransport(TransportContext ctx);

}  // namespace st
