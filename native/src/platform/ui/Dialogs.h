#pragma once
// Native file dialogs owned by the main window (Windows: IFileOpenDialog / IFileSaveDialog).
// Callable from ANY thread: when called off the UI thread the call is marshalled to the UI thread and the
// caller blocks until the dialog closes. Returns UTF-8 paths.
#include "core/Types.h"

#include <optional>
#include <string>
#include <vector>

namespace st {
class Dispatcher;
}

namespace st::ui::dialogs {

/** Called once by the app after the main window exists. ownerWindow = the main window's native handle (HWND). */
void init(void* ownerWindow, Dispatcher* dispatcher);

std::optional<std::string> openFile(const FileDialogOptions& opts);
std::vector<std::string> openFiles(const FileDialogOptions& opts);
std::optional<std::string> openDirectory(const FileDialogOptions& opts);
std::optional<std::string> saveFile(const FileDialogOptions& opts);

}  // namespace st::ui::dialogs
