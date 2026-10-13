// Telnet (RFC 854) client with option negotiation, modelled on PuTTY's telnet backend.
//   We offer : TTYPE (24), NAWS (31), TSPEED (32), NEW-ENVIRON (39) [OLD-ENVIRON (36) on request], SGA (3), BINARY (0)
//   We ask   : ECHO (1), SGA (3)      Server-side BINARY is accepted.
// Local echo / line editing follow PuTTY: echo while the server does not echo, edit while the server does not SGA.
#include "transport/TelnetTransport.h"

#include "core/Strings.h"
#include "net/SocketTransport.h"

#include <algorithm>
#include <array>

namespace sc {

namespace {

enum : unsigned char {
    IAC = 255, DONT = 254, DO = 253, WONT = 252, WILL = 251, SB = 250, GA = 249, EL = 248, EC = 247, AYT = 246,
    AO = 245, IP = 244, BRK = 243, DM = 242, NOP = 241, SE = 240, EOR_CMD = 239, ABORT = 238, SUSP = 237, xEOF = 236
};
enum : unsigned char {
    OPT_BINARY = 0, OPT_ECHO = 1, OPT_SGA = 3, OPT_TTYPE = 24, OPT_NAWS = 31, OPT_TSPEED = 32, OPT_OLD_ENVIRON = 36,
    OPT_NEW_ENVIRON = 39
};
enum : unsigned char { TELQUAL_IS = 0, TELQUAL_SEND = 1, ENV_VAR = 0, ENV_VALUE = 1, ENV_ESC = 2, ENV_USERVAR = 3 };

std::string optName(unsigned char o) {
    switch (o) {
        case OPT_BINARY: return "BINARY";
        case OPT_ECHO: return "ECHO";
        case OPT_SGA: return "SGA";
        case OPT_TTYPE: return "TTYPE";
        case OPT_NAWS: return "NAWS";
        case OPT_TSPEED: return "TSPEED";
        case OPT_OLD_ENVIRON: return "OLD-ENVIRON";
        case OPT_NEW_ENVIRON: return "NEW-ENVIRON";
        default: return "<" + std::to_string(o) + ">";
    }
}

std::string cmdName(unsigned char c) {
    switch (c) {
        case WILL: return "WILL";
        case WONT: return "WONT";
        case DO: return "DO";
        case DONT: return "DONT";
        default: return std::to_string(c);
    }
}

/** RFC 1143 option state. */
enum class Q { No, Yes, WantYes, WantNo };

struct SpecialDef {
    const char* id;
    const char* label;
    unsigned char code;
};
const SpecialDef kSpecials[] = {
    {"telnet:ayt", "Are You There", AYT},          {"break", "Break", BRK},
    {"telnet:synch", "Synch", DM},                 {"telnet:ec", "Erase Character", EC},
    {"telnet:el", "Erase Line", EL},               {"telnet:ga", "Go Ahead", GA},
    {"telnet:nop", "No Operation", NOP},           {"telnet:abort", "Abort Process", ABORT},
    {"telnet:ao", "Abort Output", AO},             {"telnet:ip", "Interrupt Process", IP},
    {"telnet:susp", "Suspend Process", SUSP},      {"telnet:eor", "End Of Record", EOR_CMD},
    {"telnet:eof", "End Of File", xEOF},
};

class TelnetTransport final : public SocketTransport {
public:
    explicit TelnetTransport(TransportContext ctx) : SocketTransport(std::move(ctx)) {}
    ~TelnetTransport() override { stopAndJoin(); }

    Protocol protocol() const override { return Protocol::Telnet; }
    std::vector<SpecialCommandDef> specials() const override {
        std::vector<SpecialCommandDef> out;
        for (const auto& s : kSpecials) out.push_back({s.id, s.label, "Telnet"});
        return out;
    }
    LdiscModes defaultLdisc() const override { return {true, true}; }

protected:
    std::string protocolName() const override { return "Telnet"; }

    void onConnected() override {
        if (session().telnet.passive) {
            log(LogLevel::Info, "Passive negotiation: waiting for the server");
            return;
        }
        // PuTTY's active negotiation set.
        requestLocal(OPT_NAWS);
        requestLocal(OPT_TSPEED);
        requestLocal(OPT_TTYPE);
        requestLocal(OPT_NEW_ENVIRON);
        requestRemote(OPT_ECHO);
        requestLocal(OPT_SGA);
        requestRemote(OPT_SGA);
    }

    void onReceive(std::string_view data) override {
        std::string out;
        for (unsigned char c : data) {
            switch (state_) {
                case St::Data:
                    if (c == IAC) {
                        state_ = St::Iac;
                    } else if (c == '\r' && !remoteBinary()) {
                        out.push_back('\r');
                        state_ = St::Cr;
                    } else {
                        out.push_back(static_cast<char>(c));
                    }
                    break;
                case St::Cr:
                    state_ = St::Data;
                    if (c == 0) break;  // CR NUL = bare CR
                    if (c == IAC) {
                        state_ = St::Iac;
                        break;
                    }
                    out.push_back(static_cast<char>(c));
                    break;
                case St::Iac:
                    if (c == IAC) {
                        out.push_back(static_cast<char>(IAC));
                        state_ = St::Data;
                    } else if (c == WILL || c == WONT || c == DO || c == DONT) {
                        negCmd_ = c;
                        state_ = St::Neg;
                    } else if (c == SB) {
                        sb_.clear();
                        state_ = St::Sb;
                    } else {
                        if (c == DM) log(LogLevel::Debug, "server: DM");
                        state_ = St::Data;
                    }
                    break;
                case St::Neg:
                    if (!out.empty()) {
                        deliver(out);
                        out.clear();
                    }
                    handleNegotiation(negCmd_, c);
                    state_ = St::Data;
                    break;
                case St::Sb:
                    if (c == IAC) state_ = St::SbIac;
                    else sb_.push_back(static_cast<char>(c));
                    break;
                case St::SbIac:
                    if (c == SE) {
                        handleSubnegotiation();
                        state_ = St::Data;
                    } else if (c == IAC) {
                        sb_.push_back(static_cast<char>(IAC));
                        state_ = St::Sb;
                    } else {
                        state_ = St::Sb;  // malformed; keep parsing
                    }
                    break;
            }
        }
        if (!out.empty()) deliver(out);
    }

    void onUserData(std::string_view data) override {
        const auto& t = session().telnet;
        const bool backspaceDel = session().keyboard.backspace == BackspaceKey::Del;
        std::string out;
        for (unsigned char c : data) {
            if (t.keyboardSendsSpecials) {
                if ((backspaceDel && c == 0x7f) || (!backspaceDel && c == 0x08)) {
                    out += std::string{static_cast<char>(IAC), static_cast<char>(EC)};
                    continue;
                }
                if (c == 0x03) {
                    out += std::string{static_cast<char>(IAC), static_cast<char>(IP)};
                    continue;
                }
                if (c == 0x1a) {
                    out += std::string{static_cast<char>(IAC), static_cast<char>(SUSP)};
                    continue;
                }
            }
            if (c == IAC) {
                out += std::string(2, static_cast<char>(IAC));
            } else if (c == '\r' && !localBinary()) {
                out += t.returnSendsNewline ? std::string("\r\n") : std::string("\r\0", 2);
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
        sendRaw(out);
    }

    void onResize() override {
        if (local_[OPT_NAWS] == Q::Yes) sendNaws();
    }

    void onSpecial(const std::string& id) override {
        for (const auto& s : kSpecials) {
            if (id != s.id) continue;
            if (s.code == DM) {
                // Synch: IAC DM with the DM byte sent as TCP urgent data.
                sendRaw(std::string(1, static_cast<char>(IAC)));
                sendUrgent(DM);
            } else {
                sendRaw(std::string{static_cast<char>(IAC), static_cast<char>(s.code)});
            }
            log(LogLevel::Info, std::string("Sent ") + s.label);
            return;
        }
    }

    int keepaliveIntervalMs() const override { return std::max(0, session().connection.keepaliveSeconds) * 1000; }
    void onKeepalive() override { sendRaw(std::string{static_cast<char>(IAC), static_cast<char>(NOP)}); }

private:
    enum class St { Data, Cr, Iac, Neg, Sb, SbIac } state_ = St::Data;
    unsigned char negCmd_ = 0;
    std::string sb_;
    std::array<Q, 256> local_{};   // options we perform (WILL/WONT)
    std::array<Q, 256> remote_{};  // options the server performs (DO/DONT)

    bool localBinary() const { return local_[OPT_BINARY] == Q::Yes; }
    bool remoteBinary() const { return remote_[OPT_BINARY] == Q::Yes; }

    static bool weSupport(unsigned char o) {
        return o == OPT_TTYPE || o == OPT_NAWS || o == OPT_TSPEED || o == OPT_NEW_ENVIRON || o == OPT_OLD_ENVIRON ||
               o == OPT_SGA || o == OPT_BINARY;
    }
    static bool theySupport(unsigned char o) { return o == OPT_ECHO || o == OPT_SGA || o == OPT_BINARY; }

    void send3(unsigned char cmd, unsigned char opt) {
        sendRaw(std::string{static_cast<char>(IAC), static_cast<char>(cmd), static_cast<char>(opt)});
        log(LogLevel::Debug, "client: " + cmdName(cmd) + " " + optName(opt));
    }

    void requestLocal(unsigned char o) {
        if (local_[o] == Q::No) {
            local_[o] = Q::WantYes;
            send3(WILL, o);
        }
    }
    void requestRemote(unsigned char o) {
        if (remote_[o] == Q::No) {
            remote_[o] = Q::WantYes;
            send3(DO, o);
        }
    }

    void handleNegotiation(unsigned char cmd, unsigned char o) {
        log(LogLevel::Debug, "server: " + cmdName(cmd) + " " + optName(o));
        const bool wasRemoteEcho = remote_[OPT_ECHO] == Q::Yes, wasRemoteSga = remote_[OPT_SGA] == Q::Yes;
        switch (cmd) {
            case WILL:
                if (remote_[o] == Q::Yes) break;
                if (remote_[o] == Q::WantYes) {
                    remote_[o] = Q::Yes;
                } else if (theySupport(o)) {
                    remote_[o] = Q::Yes;
                    send3(DO, o);
                } else {
                    send3(DONT, o);
                }
                break;
            case WONT:
                if (remote_[o] == Q::No) break;
                if (remote_[o] == Q::Yes) send3(DONT, o);
                remote_[o] = Q::No;
                break;
            case DO:
                if (local_[o] == Q::Yes) break;
                if (local_[o] == Q::WantYes || weSupport(o)) {
                    if (local_[o] != Q::WantYes) send3(WILL, o);
                    local_[o] = Q::Yes;
                    if (o == OPT_NAWS) sendNaws();
                } else {
                    send3(WONT, o);
                }
                break;
            case DONT:
                if (local_[o] == Q::No) break;
                if (local_[o] == Q::Yes) send3(WONT, o);
                local_[o] = Q::No;
                break;
        }
        const bool echo = remote_[OPT_ECHO] == Q::Yes, sga = remote_[OPT_SGA] == Q::Yes;
        if (echo != wasRemoteEcho || sga != wasRemoteSga) ldiscChanged({!echo, !sga});
    }

    void sendSb(unsigned char opt, const std::string& payload) {
        std::string out{static_cast<char>(IAC), static_cast<char>(SB), static_cast<char>(opt)};
        for (char c : payload) {
            out.push_back(c);
            if (static_cast<unsigned char>(c) == IAC) out.push_back(c);
        }
        out += std::string{static_cast<char>(IAC), static_cast<char>(SE)};
        sendRaw(out);
    }

    void sendNaws() {
        const int w = std::clamp(cols(), 1, 65535), h = std::clamp(rows(), 1, 65535);
        std::string p{static_cast<char>(w >> 8), static_cast<char>(w & 0xff), static_cast<char>(h >> 8), static_cast<char>(h & 0xff)};
        sendSb(OPT_NAWS, p);
        log(LogLevel::Debug, "client: SB NAWS " + std::to_string(w) + "," + std::to_string(h));
    }

    void handleSubnegotiation() {
        if (sb_.size() < 2) return;
        const unsigned char opt = static_cast<unsigned char>(sb_[0]);
        const unsigned char qual = static_cast<unsigned char>(sb_[1]);
        if (qual != TELQUAL_SEND) return;
        if (opt == OPT_TTYPE && local_[OPT_TTYPE] == Q::Yes) {
            std::string term = session().connection.terminalType.empty() ? "xterm" : session().connection.terminalType;
            sendSb(OPT_TTYPE, std::string(1, static_cast<char>(TELQUAL_IS)) + term);
            log(LogLevel::Info, "client: SB TTYPE IS " + term);
        } else if (opt == OPT_TSPEED && local_[OPT_TSPEED] == Q::Yes) {
            const std::string speed = session().connection.terminalSpeed.empty() ? "38400,38400" : session().connection.terminalSpeed;
            sendSb(OPT_TSPEED, std::string(1, static_cast<char>(TELQUAL_IS)) + speed);
            log(LogLevel::Info, "client: SB TSPEED IS " + speed);
        } else if ((opt == OPT_NEW_ENVIRON || opt == OPT_OLD_ENVIRON) && local_[opt] == Q::Yes) {
            // OLD-ENVIRON implementations in the wild swap VAR and VALUE (RFC 1408 vs BSD); PuTTY sends VAR=1/VALUE=0 for it.
            const bool old = opt == OPT_OLD_ENVIRON;
            const char var = old ? 1 : ENV_VAR, value = old ? 0 : ENV_VALUE;
            std::string p(1, static_cast<char>(TELQUAL_IS));
            auto add = [&](char kind, const std::string& name, const std::string& val) {
                p.push_back(kind);
                for (char c : name) {
                    if (c >= 0 && c <= 3) p.push_back(ENV_ESC);
                    p.push_back(c);
                }
                p.push_back(value);
                for (char c : val) {
                    if (c >= 0 && c <= 3) p.push_back(ENV_ESC);
                    p.push_back(c);
                }
            };
            if (!session().username.empty()) add(var, "USER", session().username);
            for (const auto& e : session().connection.environment)
                if (!e.name.empty()) add(old ? var : static_cast<char>(ENV_USERVAR), e.name, e.value);
            sendSb(opt, p);
            log(LogLevel::Info, std::string("client: SB ") + optName(opt) + " IS (" + std::to_string(session().connection.environment.size() + (session().username.empty() ? 0 : 1)) + " variables)");
        }
    }
};

}  // namespace

std::unique_ptr<Transport> makeTelnetTransport(TransportContext ctx) {
    if (ctx.session.host.empty()) throw std::invalid_argument("No host name specified");
    return std::make_unique<TelnetTransport>(std::move(ctx));
}

}  // namespace sc
