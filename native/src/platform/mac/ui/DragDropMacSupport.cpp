#include "platform/mac/ui/DragDropMacSupport.h"

namespace st::ui::dnd::mac {

namespace {

std::string lastComponent(const std::string& path) {
    const size_t i = path.find_last_of('/');
    return i == std::string::npos ? path : path.substr(i + 1);
}

}  // namespace

std::vector<std::string> matchDropped(const std::vector<std::string>& dropped, const std::vector<std::string>& names) {
    if (names.empty()) return dropped;
    std::vector<bool> used(dropped.size(), false);
    std::vector<std::string> out;
    for (const auto& name : names) {
        for (size_t i = 0; i < dropped.size(); ++i) {
            if (used[i] || lastComponent(dropped[i]) != name) continue;
            used[i] = true;
            out.push_back(dropped[i]);
            break;
        }
    }
    return out;
}

}  // namespace st::ui::dnd::mac
