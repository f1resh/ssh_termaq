// STUB — to be implemented (owner: host app module).
#include "app/Dialogs.h"

namespace sc::dialogs {

void init(void*, Dispatcher*) {}
std::optional<std::string> openFile(const FileDialogOptions&) { return std::nullopt; }
std::vector<std::string> openFiles(const FileDialogOptions&) { return {}; }
std::optional<std::string> openDirectory(const FileDialogOptions&) { return std::nullopt; }
std::optional<std::string> saveFile(const FileDialogOptions&) { return std::nullopt; }

}  // namespace sc::dialogs
