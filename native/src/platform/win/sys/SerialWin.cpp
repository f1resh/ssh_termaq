// Serial ports on Windows: CreateFile("\\.\COMn") + SetCommState + overlapped I/O, SetupAPI enumeration.
#include "platform/sys/Serial.h"

#include "platform/detail/WinApi.h"
#include "platform/sys/Errors.h"
#include "platform/win/sys/WinText.h"

#include <devguid.h>
#include <initguid.h>
#include <setupapi.h>
#include <ntddser.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iterator>
#include <thread>

namespace st::sys::serial {

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

bool equalsNoCase(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}

std::string devProp(HDEVINFO set, SP_DEVINFO_DATA& dev, DWORD prop) {
    wchar_t buf[512];
    DWORD type = 0;
    if (SetupDiGetDeviceRegistryPropertyW(set, &dev, prop, &type, reinterpret_cast<PBYTE>(buf), sizeof(buf) - sizeof(wchar_t), nullptr))
        return narrow(buf);
    return {};
}

}  // namespace

std::vector<PortInfo> listPorts() {
    std::vector<PortInfo> out;
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
            PortInfo p;
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
            if (std::none_of(out.begin(), out.end(), [&](const PortInfo& p) { return equalsNoCase(p.path, path); }))
                out.push_back({path, "", ""});
        }
        RegCloseKey(map);
    }
    return out;
}

struct Port::Impl {
    Handle wake;  // auto-reset: write / break / close requests
    Handle port;
    std::string line;
    DWORD baudRate = 0;
    BYTE dataBits = 0;

    /** Closes the device as soon as the session is done with it, so it can be opened again right away. */
    void closePort() {
        if (port.h && port.h != INVALID_HANDLE_VALUE) CloseHandle(port.h);
        port.h = nullptr;
    }
};

Port::Port() : impl_(std::make_unique<Impl>()) { impl_->wake.h = CreateEventW(nullptr, FALSE, FALSE, nullptr); }

Port::~Port() = default;

void Port::wake() { SetEvent(impl_->wake.h); }

int Port::baudRate() const { return static_cast<int>(impl_->baudRate); }

int Port::dataBits() const { return impl_->dataBits; }

bool Port::open(const Settings& o, std::string& error) {
    const std::string& line = o.line;
    impl_->line = line;
    std::wstring path = widen(line);
    if (path.rfind(L"\\\\.\\", 0) != 0) path = L"\\\\.\\" + path;
    impl_->port.h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    HANDLE port = impl_->port.h;
    if (port == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        std::string why = errorMessage(err);
        if (err == ERROR_ACCESS_DENIED) why = "the port is in use by another program";
        error = "Unable to open serial port " + line + ": " + why;
        return false;
    }

    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(port, &dcb)) {
        error = "Unable to read serial port settings: " + lastErrorMessage();
        impl_->closePort();
        return false;
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
    if (!SetCommState(port, &dcb)) {
        error = "Unable to configure serial port " + line + ": " + lastErrorMessage();
        impl_->closePort();
        return false;
    }
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = MAXDWORD;
    to.ReadTotalTimeoutConstant = 200;
    to.WriteTotalTimeoutConstant = 5000;
    SetCommTimeouts(port, &to);
    SetupComm(port, 64 * 1024, 64 * 1024);
    PurgeComm(port, PURGE_RXCLEAR | PURGE_TXCLEAR);
    impl_->baudRate = dcb.BaudRate;
    impl_->dataBits = dcb.ByteSize;
    return true;
}

std::string Port::run(const PortIo& io) {
    HANDLE port = impl_->port.h;
    Handle readEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    Handle writeEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    OVERLAPPED rov{};
    rov.hEvent = readEvent.h;
    char buf[4096];
    bool readPending = false;
    std::string error;

    while (!io.stopRequested()) {
        if (!readPending) {
            ResetEvent(readEvent.h);
            DWORD n = 0;
            if (ReadFile(port, buf, sizeof buf, &n, &rov)) {
                if (n) io.received(std::string_view(buf, n));
                continue;
            }
            if (GetLastError() != ERROR_IO_PENDING) {
                error = "Error reading from serial device: " + lastErrorMessage();
                break;
            }
            readPending = true;
        }

        // Pending writes / break.
        const std::string out = io.takeOutput();
        if (!out.empty()) {
            OVERLAPPED wov{};
            wov.hEvent = writeEvent.h;
            ResetEvent(writeEvent.h);
            DWORD written = 0;
            if (!WriteFile(port, out.data(), static_cast<DWORD>(out.size()), &written, &wov)) {
                if (GetLastError() != ERROR_IO_PENDING || !GetOverlappedResult(port, &wov, &written, TRUE)) {
                    error = "Error writing to serial device: " + lastErrorMessage();
                    break;
                }
            }
        }
        if (io.takeBreak()) {
            SetCommBreak(port);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            ClearCommBreak(port);
            io.log("Sent break");
        }

        HANDLE waits[2] = {readEvent.h, impl_->wake.h};
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0) {
            DWORD n = 0;
            readPending = false;
            if (!GetOverlappedResult(port, &rov, &n, FALSE)) {
                error = "Error reading from serial device: " + lastErrorMessage();
                break;
            }
            if (n) io.received(std::string_view(buf, n));
        }
    }
    if (readPending) {
        CancelIoEx(port, &rov);
        DWORD n = 0;
        GetOverlappedResult(port, &rov, &n, TRUE);
    }
    impl_->closePort();
    return error;
}

}  // namespace st::sys::serial
