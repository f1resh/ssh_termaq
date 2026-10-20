#pragma once
// Windows building blocks of platform/ui/DragDrop.h, exposed for the tests in native/tests/platform/win/ui.
#include "platform/ui/DragDrop.h"

#include <string>
#include <vector>

struct IDataObject;

namespace st::ui::dnd {

/** Shell data object for local paths (refcount 1), nullptr when the paths cannot be resolved. */
IDataObject* createShellDataObject(const std::vector<std::string>& paths);

}  // namespace st::ui::dnd
