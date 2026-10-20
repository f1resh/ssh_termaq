#pragma once
// Private to platform/mac/ui: the plain C++ part of drag & drop on macOS (DragDropMac.mm), apart so that tests can use
// it - finding the paths of the files dropped from Finder.
#include <string>
#include <vector>

namespace st::ui::dnd::mac {

/**
 * The paths of a drop for the page's File objects, by name (File.name is the last path component; both in the same
 * Unicode normalization form), in the names' order; each path is used once and names without one are left out. No
 * names: all paths.
 */
std::vector<std::string> matchDropped(const std::vector<std::string>& dropped, const std::vector<std::string>& names);

}  // namespace st::ui::dnd::mac
