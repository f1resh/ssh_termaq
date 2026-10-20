// The clipboard on Windows: CF_UNICODETEXT through OpenClipboard (retried while another program holds it).
#include "platform/ui/Clipboard.h"

#include "platform/detail/WinApi.h"
#include "platform/win/sys/WinText.h"

#include <cstring>

namespace st::ui::clipboard {

namespace {

bool openClipboard(HWND owner) {
    for (int i = 0; i < 10; ++i) {
        if (OpenClipboard(owner)) return true;
        Sleep(15);
    }
    return false;
}

}  // namespace

std::string readText(void* ownerWindow) {
    if (!openClipboard(static_cast<HWND>(ownerWindow))) return {};
    std::string out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            out = narrow(p);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

void writeText(void* ownerWindow, const std::string& utf8) {
    // Normalize line endings to CRLF (Windows convention) without doubling existing CRLFs.
    std::string text;
    text.reserve(utf8.size() + 16);
    for (size_t i = 0; i < utf8.size(); ++i) {
        char c = utf8[i];
        if (c == '\n' && (i == 0 || utf8[i - 1] != '\r')) text += '\r';
        text += c;
    }
    std::wstring w = widen(text);
    if (!openClipboard(static_cast<HWND>(ownerWindow))) return;
    EmptyClipboard();
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t))) {
        if (auto* p = static_cast<wchar_t*>(GlobalLock(mem))) {
            memcpy(p, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

}  // namespace st::ui::clipboard
