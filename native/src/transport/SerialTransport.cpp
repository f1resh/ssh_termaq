// Serial line transport: overlapped Win32 COM port I/O on a dedicated thread, plus port enumeration.
#include "transport/SerialTransport.h"

#include "core/Strings.h"
#include "core/Win.h"

#include <devguid.h>
#include <initguid.h>
#include <setupapi.h>
#include <ntddser.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

namespace st {

namespace {

struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE x) : h(x) {}
    ~Handle() {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

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

class SerialTransportImpl final : public Transport {
public:
    explicit SerialTransportImpl(TransportContext ctx) : ctx_(std::move(ctx)) {
        wake_.h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
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
        SetEvent(wake_.h);
    }
    void resize(int, int, int, int) override {}
    std::vector<SpecialCommandDef> specials() const override { return {{"serial:break", "Break", ""}}; }
    void sendSpecial(const std::string& id) override {
        if (id != "serial:break" && id != "break") return;
        breakRequested_ = true;
        SetEvent(wake_.h);
    }
    void close() override {
        closing_ = true;
        SetEvent(wake_.h);
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
        std::wstring path = widen(line);
        if (path.rfind(L"\\\\.\\", 0) != 0) path = L"\\\\.\\" + path;
        Handle port(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        if (port.h == INVALID_HANDLE_VALUE) {
            const DWORD err = GetLastError();
            std::string why = winErrorMessage(err);
            if (err == ERROR_ACCESS_DENIED) why = "the port is in use by another program";
            finish(false, "Unable to open serial port " + line + ": " + why, true);
            return;
        }

        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(port.h, &dcb)) {
            finish(false, "Unable to read serial port settings: " + lastErrorMessage(), true);
            return;
        }
        dcb.BaudRate = static_cast<DWORD>(std::max(1, o.baudRate));
        dcb.ByteSize = static_cast<BYTE>(std::clamp(o.dataBits, 5, 8));
        dcb.fBinary = TRUE;
        switch (o.parity) {
            case Parity::Odd: dcb.Parity = ODDPARITY; break;
            case Parity::Even: dcb.Parity = EVENPARITY; break;
            case Parity::Mark: dcb.Parity = MARKPARITY; break;
            case Parity::Space: dcb.Parity = SPACEPARITY; break;
            default: dcb.Parity = NOPARITY;
        }
        dcb.fParity = o.parity != Parity::None;
        dcb.StopBits = o.stopBits >= 2 ? TWOSTOPBITS : (o.stopBits > 1 ? ONE5STOPBITS : ONESTOPBIT);
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        dcb.fOutX = dcb.fInX = FALSE;
        dcb.fDsrSensitivity = FALSE;
        dcb.fAbortOnError = FALSE;
        switch (o.flowControl) {
            case FlowControl::XonXoff:
                dcb.fOutX = dcb.fInX = TRUE;
                dcb.XonChar = 0x11;
                dcb.XoffChar = 0x13;
                break;
            case FlowControl::RtsCts:
                dcb.fOutxCtsFlow = TRUE;
                dcb.fRtsControl = RTS_CONTROL_HANDSHAKE;
                break;
            case FlowControl::DsrDtr:
                dcb.fOutxDsrFlow = TRUE;
                dcb.fDtrControl = DTR_CONTROL_HANDSHAKE;
                break;
            default: break;
        }
        if (!SetCommState(port.h, &dcb)) {
            finish(false, "Unable to configure serial port " + line + ": " + lastErrorMessage(), true);
            return;
        }
        COMMTIMEOUTS to{};
        to.ReadIntervalTimeout = MAXDWORD;
        to.ReadTotalTimeoutMultiplier = MAXDWORD;
        to.ReadTotalTimeoutConstant = 200;
        to.WriteTotalTimeoutConstant = 5000;
        SetCommTimeouts(port.h, &to);
        SetupComm(port.h, 64 * 1024, 64 * 1024);
        PurgeComm(port.h, PURGE_RXCLEAR | PURGE_TXCLEAR);

        log(LogLevel::Info, "Opened serial line " + line + " at " + std::to_string(dcb.BaudRate) + " baud, " +
                                std::to_string(dcb.ByteSize) + " data bits, parity " + parityName(o.parity) + ", " +
                                (o.stopBits >= 2 ? "2" : o.stopBits > 1 ? "1.5" : "1") + " stop bits, flow control " +
                                flowName(o.flowControl));
        if (ctx_.listener) ctx_.listener->onStatus(ConnStatus::Connected, "");

        Handle readEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        Handle writeEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        OVERLAPPED rov{};
        rov.hEvent = readEvent.h;
        char buf[4096];
        bool readPending = false;
        std::string error;

        while (!closing_) {
            if (!readPending) {
                ResetEvent(readEvent.h);
                DWORD n = 0;
                if (ReadFile(port.h, buf, sizeof buf, &n, &rov)) {
                    if (n && ctx_.listener) ctx_.listener->onData(std::string_view(buf, n));
                    continue;
                }
                if (GetLastError() != ERROR_IO_PENDING) {
                    error = "Error reading from serial device: " + lastErrorMessage();
                    break;
                }
                readPending = true;
            }

            // Pending writes / break.
            std::string out;
            {
                std::lock_guard l(mutex_);
                out.swap(out_);
            }
            if (!out.empty()) {
                OVERLAPPED wov{};
                wov.hEvent = writeEvent.h;
                ResetEvent(writeEvent.h);
                DWORD written = 0;
                if (!WriteFile(port.h, out.data(), static_cast<DWORD>(out.size()), &written, &wov)) {
                    if (GetLastError() != ERROR_IO_PENDING || !GetOverlappedResult(port.h, &wov, &written, TRUE)) {
                        error = "Error writing to serial device: " + lastErrorMessage();
                        break;
                    }
                }
            }
            if (breakRequested_.exchange(false)) {
                SetCommBreak(port.h);
                Sleep(250);
                ClearCommBreak(port.h);
                log(LogLevel::Info, "Sent break");
            }

            HANDLE waits[2] = {readEvent.h, wake_.h};
            const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0) {
                DWORD n = 0;
                readPending = false;
                if (!GetOverlappedResult(port.h, &rov, &n, FALSE)) {
                    error = "Error reading from serial device: " + lastErrorMessage();
                    break;
                }
                if (n && ctx_.listener) ctx_.listener->onData(std::string_view(buf, n));
            }
        }
        if (readPending) {
            CancelIoEx(port.h, &rov);
            DWORD n = 0;
            GetOverlappedResult(port.h, &rov, &n, TRUE);
        }
        if (!error.empty())
            finish(false, error, true);
        else
            finish(true, "Serial line closed", false);
    }

    TransportContext ctx_;
    std::thread thread_;
    Handle wake_;
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

std::string devProp(HDEVINFO set, SP_DEVINFO_DATA& dev, DWORD prop) {
    wchar_t buf[512];
    DWORD type = 0;
    if (SetupDiGetDeviceRegistryPropertyW(set, &dev, prop, &type, reinterpret_cast<PBYTE>(buf), sizeof(buf) - sizeof(wchar_t), nullptr))
        return narrow(buf);
    return {};
}

}  // namespace

std::unique_ptr<Transport> makeSerialTransport(TransportContext ctx) { return std::make_unique<SerialTransportImpl>(std::move(ctx)); }

std::vector<SerialPortInfo> listSerialPorts() {
    std::vector<SerialPortInfo> out;
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_COMPORT, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA dev{sizeof(dev)};
        for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
            HKEY key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
            if (key == INVALID_HANDLE_VALUE) continue;
            wchar_t name[64] = {};
            DWORD size = sizeof(name) - sizeof(wchar_t), type = 0;
            const bool ok = RegQueryValueExW(key, L"PortName", nullptr, &type, reinterpret_cast<LPBYTE>(name), &size) == ERROR_SUCCESS;
            RegCloseKey(key);
            if (!ok || type != REG_SZ) continue;
            SerialPortInfo p;
            p.path = narrow(name);
            p.friendlyName = devProp(set, dev, SPDRP_FRIENDLYNAME);
            p.manufacturer = devProp(set, dev, SPDRP_MFG);
            out.push_back(p);
        }
        SetupDiDestroyDeviceInfoList(set);
    }
    // Fallback / complement: HARDWARE\DEVICEMAP\SERIALCOMM lists every active serial device.
    HKEY map = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &map) == ERROR_SUCCESS) {
        for (DWORD i = 0;; ++i) {
            wchar_t valueName[256], data[64];
            DWORD vn = static_cast<DWORD>(std::size(valueName)), dn = sizeof(data) - sizeof(wchar_t), type = 0;
            if (RegEnumValueW(map, i, valueName, &vn, nullptr, &type, reinterpret_cast<LPBYTE>(data), &dn) != ERROR_SUCCESS) break;
            if (type != REG_SZ) continue;
            data[dn / sizeof(wchar_t)] = 0;
            const std::string path = narrow(data);
            if (std::none_of(out.begin(), out.end(), [&](const SerialPortInfo& p) { return iequals(p.path, path); }))
                out.push_back({path, "", ""});
        }
        RegCloseKey(map);
    }
    std::sort(out.begin(), out.end(), [](const SerialPortInfo& a, const SerialPortInfo& b) { return naturalLess(a.path, b.path); });
    return out;
}

}  // namespace st
