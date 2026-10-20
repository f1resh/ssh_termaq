#include "platform/sys/Errors.h"

#include "platform/detail/WinApi.h"
#include "platform/win/sys/WinText.h"

#include <cctype>

namespace st::sys {

std::string errorMessage(unsigned long code) {
    wchar_t* buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string msg;
    if (n && buf) {
        msg = narrow(std::wstring_view(buf, n));
        LocalFree(buf);
        size_t b = 0, e = msg.size();
        while (b < e && std::isspace(static_cast<unsigned char>(msg[b]))) ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(msg[e - 1]))) --e;
        msg = msg.substr(b, e - b);
    } else {
        msg = "Error " + std::to_string(code);
    }
    return msg;
}

std::string lastErrorMessage() { return errorMessage(GetLastError()); }

std::string lastSocketErrorMessage() { return errorMessage(static_cast<unsigned long>(WSAGetLastError())); }

}  // namespace st::sys
