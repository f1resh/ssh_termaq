// Owner: session module.
#include "transport/TransportFactory.h"

#include "net/Proxy.h"
#include "ssh/SshJump.h"
#include "ssh/SshTransport.h"
#include "transport/RawTransport.h"
#include "transport/RloginTransport.h"
#include "transport/SerialTransport.h"
#include "transport/TelnetTransport.h"

#include <mutex>
#include <stdexcept>

namespace st {

std::unique_ptr<Transport> createTransport(TransportContext ctx) {
    static std::once_flag once;
    std::call_once(once, [] { setSshJumpOpener(&openSshJumpTunnel); });
    switch (ctx.session.protocol) {
        case Protocol::Ssh: return makeSshTransport(std::move(ctx));
        case Protocol::Telnet: return makeTelnetTransport(std::move(ctx));
        case Protocol::Rlogin: return makeRloginTransport(std::move(ctx));
        case Protocol::Raw: return makeRawTransport(std::move(ctx));
        case Protocol::Serial: return makeSerialTransport(std::move(ctx));
    }
    throw std::invalid_argument("Unsupported protocol");
}

}  // namespace st
