// The clipboard on macOS: the general pasteboard's plain text. Line endings are LF.
#import <AppKit/AppKit.h>

#include "platform/ui/Clipboard.h"

namespace st::ui::clipboard {

std::string readText(void*) {
    NSString* text = [NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString];
    return text ? std::string(text.UTF8String) : std::string();
}

void writeText(void*, const std::string& utf8) {
    std::string text;
    text.reserve(utf8.size());
    for (size_t i = 0; i < utf8.size(); ++i)
        if (!(utf8[i] == '\r' && i + 1 < utf8.size() && utf8[i + 1] == '\n')) text += utf8[i];
    NSPasteboard* pb = NSPasteboard.generalPasteboard;
    [pb clearContents];
    [pb setString:[NSString stringWithUTF8String:text.c_str()] ?: @"" forType:NSPasteboardTypeString];
}

}  // namespace st::ui::clipboard
