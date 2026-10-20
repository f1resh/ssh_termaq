#pragma once
// Private to platform/: serial port details whose form differs per Unix (implemented in mac/sys/SerialMac.cpp) for the
// POSIX port code (posix/sys/SerialPosix.cpp).
namespace st::sys::serial::detail {

/** Sets a speed the B* constants do not cover on the open, configured tty `fd` (after tcsetattr, which would reset
 *  it). False with errno set. */
bool setCustomSpeed(int fd, int baudRate);

/** c_cflag bits for DSR/DTR hardware flow control; 0 when the system has none. */
unsigned long dsrDtrFlowFlags();

}  // namespace st::sys::serial::detail
