#pragma once
// OS error codes as readable messages: GetLastError / WSAGetLastError / FormatMessage on Windows, errno / strerror on
// POSIX. Codes are only meaningful on the OS that produced them.
#include <string>

namespace st::sys {

/** Readable UTF-8 message for an OS error code; "Error <code>" when the OS has no text for it. */
std::string errorMessage(unsigned long code);
/** Message for the last failed OS call on this thread (GetLastError() | errno). */
std::string lastErrorMessage();
/** Message for the last failed socket call on this thread (WSAGetLastError() | errno). */
std::string lastSocketErrorMessage();

}  // namespace st::sys
