#pragma once
// Serial line (PuTTY "Serial"): the port (platform/sys/Serial.h: baud, data bits, stop bits, parity, flow control
// none/XON-XOFF/RTS-CTS/DSR-DTR) and its I/O loop on the transport's own thread.
// Special commands: "serial:break" (a break of ~250 ms). Local echo/edit default off.
#include "transport/Transport.h"

#include <memory>
#include <vector>

namespace st {

std::unique_ptr<Transport> makeSerialTransport(TransportContext ctx);

/** The serial ports present (sys::serial::listPorts), sorted naturally (COM2 before COM10). */
std::vector<SerialPortInfo> listSerialPorts();

}  // namespace st
