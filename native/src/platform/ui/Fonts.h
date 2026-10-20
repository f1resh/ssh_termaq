#pragma once
// Installed fonts, for the terminal font picker.
#include <string>
#include <vector>

namespace st::ui::fonts {

/** Installed font family names, sorted. Empty where the page lists them itself (Windows: Local Font Access). */
std::vector<std::string> installedFamilies();

}  // namespace st::ui::fonts
