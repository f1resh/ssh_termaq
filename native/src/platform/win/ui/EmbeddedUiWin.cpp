// Embedded UI files on Windows: resources of type "UIFILE" in the executable (generated ui.rc).
#include "platform/ui/EmbeddedUi.h"

#include "platform/detail/WinApi.h"

namespace st::ui {

namespace {

constexpr wchar_t kResourceType[] = L"UIFILE";

/** Bytes of resource UIFILE #id (empty view for a zero-length resource); nullopt when it does not exist. */
std::optional<std::string_view> resourceBytes(HMODULE module, int id) {
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(id), kResourceType);
    if (!res) return std::nullopt;
    const DWORD size = SizeofResource(module, res);
    HGLOBAL handle = LoadResource(module, res);
    const void* data = handle ? LockResource(handle) : nullptr;
    if (!data) return size == 0 ? std::optional<std::string_view>(std::string_view{}) : std::nullopt;
    return std::string_view(static_cast<const char*>(data), size);
}

}  // namespace

std::optional<std::string_view> embeddedUiFile(int id) { return resourceBytes(GetModuleHandleW(nullptr), id); }

}  // namespace st::ui
