// Embedded UI files on macOS: two sections the linker created from cmake/EmbedUi.cmake's pack (-sectcreate) in the
// executable - __TEXT,__st_ui_dir ("<id>\t<offset>\t<size>" lines) and __TEXT,__st_ui_data (the files, one after the
// other). Read in place: the bytes stay valid for the process lifetime.
#include "platform/ui/EmbeddedUi.h"

#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>

#include <charconv>
#include <map>

namespace st::ui {

namespace {

std::string_view section(const char* name) {
    unsigned long size = 0;
    const uint8_t* data = getsectiondata(&_mh_execute_header, "__TEXT", name, &size);
    return data ? std::string_view(reinterpret_cast<const char*>(data), size) : std::string_view();
}

/** id -> bytes, from the directory section; malformed lines and ranges outside the data are skipped. */
std::map<int, std::string_view> loadTable() {
    std::map<int, std::string_view> table;
    const std::string_view dir = section("__st_ui_dir"), data = section("__st_ui_data");
    size_t pos = 0;
    while (pos < dir.size()) {
        size_t end = dir.find('\n', pos);
        if (end == std::string_view::npos) end = dir.size();
        const std::string_view line = dir.substr(pos, end - pos);
        pos = end + 1;
        int id = 0;
        unsigned long long offset = 0, size = 0;
        const char* p = line.data();
        const char* last = line.data() + line.size();
        auto r = std::from_chars(p, last, id);
        if (r.ec != std::errc() || r.ptr == last || *r.ptr != '\t') continue;
        r = std::from_chars(r.ptr + 1, last, offset);
        if (r.ec != std::errc() || r.ptr == last || *r.ptr != '\t') continue;
        r = std::from_chars(r.ptr + 1, last, size);
        if (r.ec != std::errc() || offset > data.size() || size > data.size() - offset) continue;
        table[id] = data.substr(offset, size);
    }
    return table;
}

}  // namespace

std::optional<std::string_view> embeddedUiFile(int id) {
    static const std::map<int, std::string_view> table = loadTable();
    const auto it = table.find(id);
    if (it == table.end()) return std::nullopt;
    return it->second;
}

}  // namespace st::ui
