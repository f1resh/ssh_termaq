#pragma once
#include "transport/Transport.h"

#include <memory>

namespace sc {

/** Creates the transport for ctx.session.protocol. Throws std::invalid_argument for unsupported configs. */
std::unique_ptr<Transport> createTransport(TransportContext ctx);

}  // namespace sc
