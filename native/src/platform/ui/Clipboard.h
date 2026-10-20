#pragma once
// The system clipboard as UTF-8 text (UI thread). Line endings follow the OS convention when writing (Windows: CRLF).
#include <string>

namespace st::ui::clipboard {

/** The clipboard's text, '' when it holds none or cannot be opened. ownerWindow: native handle (HWND), may be null. */
std::string readText(void* ownerWindow);
/** Replaces the clipboard's content with `utf8`. */
void writeText(void* ownerWindow, const std::string& utf8);

}  // namespace st::ui::clipboard
