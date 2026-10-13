// STUB — to be implemented (owner: network/transports module).
#include "transport/RloginTransport.h"

#include <stdexcept>

namespace sc {

std::unique_ptr<Transport> makeRloginTransport(TransportContext) { throw std::invalid_argument("Rlogin transport not implemented"); }

}  // namespace sc
