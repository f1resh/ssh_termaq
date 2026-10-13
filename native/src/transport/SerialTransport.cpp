// STUB — to be implemented (owner: network/transports module).
#include "transport/SerialTransport.h"

#include <stdexcept>

namespace sc {

std::unique_ptr<Transport> makeSerialTransport(TransportContext) { throw std::invalid_argument("Serial transport not implemented"); }
std::vector<SerialPortInfo> listSerialPorts() { return {}; }

}  // namespace sc
