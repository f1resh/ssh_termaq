// STUB — to be implemented (owner: network/transports module).
#include "transport/RawTransport.h"

#include <stdexcept>

namespace sc {

std::unique_ptr<Transport> makeRawTransport(TransportContext) { throw std::invalid_argument("Raw transport not implemented"); }

}  // namespace sc
