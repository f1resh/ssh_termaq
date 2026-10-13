// STUB — to be implemented (owner: network/transports module).
#include "transport/TelnetTransport.h"

#include <stdexcept>

namespace sc {

std::unique_ptr<Transport> makeTelnetTransport(TransportContext) { throw std::invalid_argument("Telnet transport not implemented"); }

}  // namespace sc
