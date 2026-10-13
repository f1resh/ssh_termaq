// STUB — to be implemented (owner: SSH module).
#include "ssh/SshTransport.h"

#include <stdexcept>

namespace sc {

std::unique_ptr<Transport> makeSshTransport(TransportContext) { throw std::invalid_argument("SSH transport not implemented"); }

}  // namespace sc
