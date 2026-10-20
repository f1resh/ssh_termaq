// Installed fonts on Windows: WebView2 offers the page the Local Font Access API (window.queryLocalFonts), so the host
// lists none itself.
#include "platform/ui/Fonts.h"

namespace st::ui::fonts {

std::vector<std::string> installedFamilies() { return {}; }

}  // namespace st::ui::fonts
