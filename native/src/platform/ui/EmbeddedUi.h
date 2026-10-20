#pragma once
// The web UI's files, embedded in the executable at build time (cmake/EmbedUi.cmake) and read in place.
#include <optional>
#include <string_view>

namespace st::ui {

/**
 * Embedded file number `id` (1 = the index; layout: app/UiResources.h): nullopt when absent, an empty view for an
 * empty file. Windows: resource #id of type "UIFILE" in the executable; macOS: a slice of the executable's
 * __TEXT,__st_ui_data section, listed in __TEXT,__st_ui_dir. The bytes stay valid for the process lifetime.
 */
std::optional<std::string_view> embeddedUiFile(int id);

}  // namespace st::ui
