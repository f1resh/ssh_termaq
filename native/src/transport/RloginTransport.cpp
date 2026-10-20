// Rlogin (RFC 1282): privileged source port, "\0local\0remote\0term/speed\0" handshake, out-of-band control
// bytes from the server (0x80 = send window size; 0x02 = flush; 0x10/0x20 = flow control) and window-size messages.
#include "transport/RloginTransport.h"

#include "core/Strings.h"
#include "net/SocketTransport.h"
#include "platform/sys/Env.h"

#include <algorithm>

namespace st {

namespace {

std::string localUserName() {
    std::string name = sys::userName();
    return name.empty() ? "user" : name;
}

class RloginTransport final : public SocketTransport {
public:
    explicit RloginTransport(TransportContext ctx) : SocketTransport(std::move(ctx)) {}
    ~RloginTransport() override { stopAndJoin(); }

    Protocol protocol() const override { return Protocol::Rlogin; }
    std::vector<SpecialCommandDef> specials() const override { return {}; }
    LdiscModes defaultLdisc() const override { return {false, false}; }

protected:
    std::string protocolName() const override { return "Rlogin"; }

    OpenOptions openOptions() override {
        OpenOptions o;
        o.localPortHigh = 1023;  // privileged ports, as rlogind requires (falls back to an ephemeral port)
        o.localPortLow = 512;
        return o;
    }

    bool urgentInline() const override { return false; }

    void onConnected() override {
        const auto& s = session();
        const std::string local = s.rlogin.localUsername.empty() ? localUserName() : s.rlogin.localUsername;
        const std::string remote = s.username.empty() ? local : s.username;
        std::string term = s.connection.terminalType.empty() ? "xterm" : s.connection.terminalType;
        std::string speed = s.connection.terminalSpeed;
        if (auto comma = speed.find(','); comma != std::string::npos) speed = speed.substr(0, comma);
        if (speed.empty()) speed = "38400";
        std::string hs;
        hs.push_back('\0');
        hs += local;
        hs.push_back('\0');
        hs += remote;
        hs.push_back('\0');
        hs += term + "/" + speed;
        hs.push_back('\0');
        sendRaw(hs);
        log(LogLevel::Info, "Sent rlogin handshake (local user \"" + local + "\", remote user \"" + remote + "\")");
    }

    void onReceive(std::string_view data) override {
        if (!gotAck_ && !data.empty()) {
            gotAck_ = true;
            if (data.front() == '\0') data.remove_prefix(1);  // server acknowledgement byte
        }
        if (!data.empty()) deliver(data);
    }

    void onUrgent(unsigned char b) override {
        if (b & 0x80) {
            if (!sendWindow_) log(LogLevel::Info, "Server requested window size updates");
            sendWindow_ = true;
            sendWindowSize();
        }
        if (b & 0x02) log(LogLevel::Debug, "Server requested an output flush");
        if (b & 0x10) log(LogLevel::Debug, "Server switched to raw mode (no local flow control)");
        if (b & 0x20) log(LogLevel::Debug, "Server switched to cooked mode (local flow control)");
    }

    void onResize() override {
        if (sendWindow_) sendWindowSize();
    }

private:
    void sendWindowSize() {
        auto be16 = [](std::string& out, int v) {
            v = std::clamp(v, 0, 65535);
            out.push_back(static_cast<char>((v >> 8) & 0xff));
            out.push_back(static_cast<char>(v & 0xff));
        };
        std::string msg("\xff\xffss", 4);
        be16(msg, rows());
        be16(msg, cols());
        be16(msg, widthPx());
        be16(msg, heightPx());
        sendRaw(msg);
    }

    bool gotAck_ = false;
    bool sendWindow_ = false;
};

}  // namespace

std::unique_ptr<Transport> makeRloginTransport(TransportContext ctx) {
    if (ctx.session.host.empty()) throw std::invalid_argument("No host name specified");
    return std::make_unique<RloginTransport>(std::move(ctx));
}

}  // namespace st
