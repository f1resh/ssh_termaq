// STUB — to be implemented (owner: SSH module).
#include "ssh/SshJump.h"

namespace sc {

SOCKET openSshJumpTunnel(const TransportContext&, const SessionConfig&, const std::string&, int, std::string& error,
                         const std::atomic<bool>*) {
    error = "SSH jump host not implemented";
    return INVALID_SOCKET;
}

}  // namespace sc
