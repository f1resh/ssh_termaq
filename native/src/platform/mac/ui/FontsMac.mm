// Installed fonts on macOS: NSFontManager's families (WKWebView has no Local Font Access API).
#import <AppKit/AppKit.h>

#include "platform/ui/Fonts.h"

#include <algorithm>

namespace st::ui::fonts {

std::vector<std::string> installedFamilies() {
    std::vector<std::string> out;
    for (NSString* family in NSFontManager.sharedFontManager.availableFontFamilies)
        if (![family hasPrefix:@"."]) out.emplace_back(family.UTF8String);  // ".SF NS..." are private system fonts
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace st::ui::fonts
