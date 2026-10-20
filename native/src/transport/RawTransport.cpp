// Raw TCP connection (PuTTY "Raw"): bytes pass through unchanged in both directions.
#include "transport/RawTransport.h"

#include "net/SocketTransport.h"

namespace st {

namespace {

class RawTransport final : public SocketTransport {
public:
    explicit RawTransport(TransportContext ctx) : SocketTransport(std::move(ctx)) {}
    ~RawTransport() override { stopAndJoin(); }

    Protocol protocol() const override { return Protocol::Raw; }
    std::vector<SpecialCommandDef> specials() const override { return {}; }
    LdiscModes defaultLdisc() const override { return {true, true}; }

protected:
    std::string protocolName() const override { return "Raw"; }
    void onReceive(std::string_view data) override { deliver(data); }
    void onSpecial(const std::string& id) override {
        // Not listed (PuTTY offers no Raw specials) but honoured: half-close our sending direction.
        if (id == "eof") {
            log(LogLevel::Info, "Sent EOF (half-closed the connection)");
            sendEof();
        }
    }
};

}  // namespace

std::unique_ptr<Transport> makeRawTransport(TransportContext ctx) { return std::make_unique<RawTransport>(std::move(ctx)); }

}  // namespace st
