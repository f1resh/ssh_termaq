#pragma once
// Serial ports: enumeration, opening/configuring a port and its I/O loop (Windows: COMn with overlapped I/O; macOS:
// the /dev/cu.* tty with termios and select).
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace st::sys::serial {

enum class Parity { None, Odd, Even, Mark, Space };
enum class FlowControl { None, XonXoff, RtsCts, DsrDtr };

struct Settings {
    std::string line;  // "COM3"; macOS: "cu.usbserial-1410" (in /dev) or a path
    int baudRate = 9600;
    int dataBits = 8;
    double stopBits = 1;  // 1, 1.5 or 2
    Parity parity = Parity::None;
    FlowControl flowControl = FlowControl::XonXoff;
};

struct PortInfo {
    std::string path;
    std::string friendlyName;
    std::string manufacturer;
};
/** Serial ports present now (unsorted; each path once). */
std::vector<PortInfo> listPorts();

/** What Port::run() does with the session, called on run()'s thread. */
struct PortIo {
    std::function<void(std::string_view bytes)> received;
    std::function<std::string()> takeOutput;  // bytes to send now ('' = none)
    std::function<bool()> takeBreak;          // true = send a break now
    std::function<bool()> stopRequested;
    std::function<void(const std::string& line)> log;  // informational event-log lines ("Sent break")
};

class Port {
public:
    Port();
    ~Port();
    Port(const Port&) = delete;
    Port& operator=(const Port&) = delete;

    /** Thread-safe: makes run() look at takeOutput / takeBreak / stopRequested again (kept until run() waits). */
    void wake();
    /** Opens and configures the port, for this object alone; false with a readable `error` ("Unable to open serial
     *  port COM3: ..."). What the system lacks (macOS: mark/space parity) is an error too. */
    bool open(const Settings& settings, std::string& error);
    /** The speed and data bits the port was set to (after open). */
    int baudRate() const;
    int dataBits() const;
    /** I/O loop until stopRequested() or a device error: '' or the error text. */
    std::string run(const PortIo& io);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace st::sys::serial
