// Serial line transport: the port's I/O loop (platform/sys/Serial.h) on a dedicated thread, plus port enumeration.
#include "transport/SerialTransport.h"

#include "core/Strings.h"
#include "platform/sys/Serial.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <thread>

namespace st {

namespace {

std::string parityName(Parity p) {
    switch (p) {
        case Parity::Odd: return "odd";
        case Parity::Even: return "even";
        case Parity::Mark: return "mark";
        case Parity::Space: return "space";
        default: return "none";
    }
}

std::string flowName(FlowControl f) {
    switch (f) {
        case FlowControl::XonXoff: return "XON/XOFF";
        case FlowControl::RtsCts: return "RTS/CTS";
        case FlowControl::DsrDtr: return "DSR/DTR";
        default: return "none";
    }
}

sys::serial::Settings toSettings(const SerialOptions& o, const std::string& line) {
    sys::serial::Settings s;
    s.line = line;
    s.baudRate = o.baudRate;
    s.dataBits = o.dataBits;
    s.stopBits = o.stopBits;
    switch (o.parity) {
        case Parity::Odd: s.parity = sys::serial::Parity::Odd; break;
        case Parity::Even: s.parity = sys::serial::Parity::Even; break;
        case Parity::Mark: s.parity = sys::serial::Parity::Mark; break;
        case Parity::Space: s.parity = sys::serial::Parity::Space; break;
        default: s.parity = sys::serial::Parity::None; break;
    }
    switch (o.flowControl) {
        case FlowControl::XonXoff: s.flowControl = sys::serial::FlowControl::XonXoff; break;
        case FlowControl::RtsCts: s.flowControl = sys::serial::FlowControl::RtsCts; break;
        case FlowControl::DsrDtr: s.flowControl = sys::serial::FlowControl::DsrDtr; break;
        default: s.flowControl = sys::serial::FlowControl::None; break;
    }
    return s;
}

class SerialTransportImpl final : public Transport {
public:
    explicit SerialTransportImpl(TransportContext ctx) : ctx_(std::move(ctx)) {}
    ~SerialTransportImpl() override {
        close();
        if (thread_.joinable()) thread_.join();
    }

    Protocol protocol() const override { return Protocol::Serial; }
    void start() override { thread_ = std::thread([this] { run(); }); }
    void write(std::string bytes) override {
        {
            std::lock_guard l(mutex_);
            out_ += bytes;
        }
        port_.wake();
    }
    void resize(int, int, int, int) override {}
    std::vector<SpecialCommandDef> specials() const override { return {{"serial:break", "Break", ""}}; }
    void sendSpecial(const std::string& id) override {
        if (id != "serial:break" && id != "break") return;
        breakRequested_ = true;
        port_.wake();
    }
    void close() override {
        closing_ = true;
        port_.wake();
    }
    LdiscModes defaultLdisc() const override { return {false, false}; }

private:
    void log(LogLevel l, const std::string& m) {
        if (ctx_.listener) ctx_.listener->onLog(l, m);
    }

    void finish(bool clean, const std::string& reason, bool error) {
        CloseInfo info;
        info.clean = clean;
        info.reason = reason;
        info.error = error;
        if (error) log(LogLevel::Error, reason);
        else log(LogLevel::Info, reason);
        if (ctx_.listener) ctx_.listener->onClose(info);
    }

    void run() {
        const SerialOptions& o = ctx_.session.serial;
        std::string line = trim(o.line);
        if (line.empty()) {
            finish(false, "No serial line specified", true);
            return;
        }
        if (ctx_.listener) ctx_.listener->onStatus(ConnStatus::Connecting, "Opening " + line);
        std::string error;
        if (!port_.open(toSettings(o, line), error)) {
            finish(false, error, true);
            return;
        }
        log(LogLevel::Info, "Opened serial line " + line + " at " + std::to_string(port_.baudRate()) + " baud, " +
                                std::to_string(port_.dataBits()) + " data bits, parity " + parityName(o.parity) + ", " +
                                (o.stopBits >= 2 ? "2" : o.stopBits > 1 ? "1.5" : "1") + " stop bits, flow control " +
                                flowName(o.flowControl));
        if (ctx_.listener) ctx_.listener->onStatus(ConnStatus::Connected, "");

        sys::serial::PortIo io;
        io.received = [this](std::string_view bytes) {
            if (ctx_.listener) ctx_.listener->onData(bytes);
        };
        io.takeOutput = [this] {
            std::string out;
            std::lock_guard l(mutex_);
            out.swap(out_);
            return out;
        };
        io.takeBreak = [this] { return breakRequested_.exchange(false); };
        io.stopRequested = [this] { return closing_.load(); };
        io.log = [this](const std::string& m) { log(LogLevel::Info, m); };
        error = port_.run(io);
        if (!error.empty())
            finish(false, error, true);
        else
            finish(true, "Serial line closed", false);
    }

    TransportContext ctx_;
    std::thread thread_;
    sys::serial::Port port_;
    std::mutex mutex_;
    std::string out_;
    std::atomic<bool> closing_{false};
    std::atomic<bool> breakRequested_{false};
};

/** "COM10" sorts after "COM2". */
bool naturalLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
            size_t ei = i, ej = j;
            while (ei < a.size() && std::isdigit(static_cast<unsigned char>(a[ei]))) ++ei;
            while (ej < b.size() && std::isdigit(static_cast<unsigned char>(b[ej]))) ++ej;
            const long long na = std::stoll(a.substr(i, ei - i)), nb = std::stoll(b.substr(j, ej - j));
            if (na != nb) return na < nb;
            i = ei;
            j = ej;
        } else {
            const int ca = std::toupper(static_cast<unsigned char>(a[i])), cb = std::toupper(static_cast<unsigned char>(b[j]));
            if (ca != cb) return ca < cb;
            ++i;
            ++j;
        }
    }
    return a.size() - i < b.size() - j;
}

}  // namespace

std::unique_ptr<Transport> makeSerialTransport(TransportContext ctx) { return std::make_unique<SerialTransportImpl>(std::move(ctx)); }

std::vector<SerialPortInfo> listSerialPorts() {
    std::vector<SerialPortInfo> out;
    for (const auto& p : sys::serial::listPorts()) out.push_back({p.path, p.friendlyName, p.manufacturer});
    std::sort(out.begin(), out.end(), [](const SerialPortInfo& a, const SerialPortInfo& b) { return naturalLess(a.path, b.path); });
    return out;
}

}  // namespace st
