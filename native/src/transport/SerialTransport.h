#pragma once
// Serial line (PuTTY "Serial"): Win32 CreateFile("\\\\.\\COMn") + SetCommState (baud, data bits, stop bits,
// parity, flow control none/XON-XOFF/RTS-CTS/DSR-DTR) + overlapped I/O on its own thread.
// Special commands: "serial:break" (SetCommBreak for ~250 ms). Local echo/edit default off.
#include "transport/Transport.h"

#include <memory>
#include <vector>

namespace sc {

std::unique_ptr<Transport> makeSerialTransport(TransportContext ctx);

/** Enumerate serial ports (SetupAPI GUID_DEVINTERFACE_COMPORT with friendly names; falls back to
 *  HKLM\HARDWARE\DEVICEMAP\SERIALCOMM). Sorted naturally (COM2 before COM10). */
std::vector<SerialPortInfo> listSerialPorts();

}  // namespace sc
