#pragma once
// Raw TCP connection (PuTTY "Raw"): bytes pass through unchanged; local echo/edit default on.
#include "transport/Transport.h"

#include <memory>

namespace st {

std::unique_ptr<Transport> makeRawTransport(TransportContext ctx);

}  // namespace st
