#include "app/UiResources.h"

#include "core/Strings.h"

#include <charconv>

namespace st {

namespace {

constexpr wchar_t kResourceType[] = L"UIFILE";
constexpr int kIndexId = 1;

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

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

UiResources UiResources::load(HMODULE module) {
    UiResources ui;
    const auto index = resourceBytes(module, kIndexId);
    if (!index) return ui;
    for (const auto& [path, id] : parseIndex(*index)) {
        if (auto data = resourceBytes(module, id)) ui.files_[path] = *data;
    }
    return ui;
}

std::optional<std::string_view> UiResources::find(const std::string& path) const {
    auto it = files_.find(path);
    if (it == files_.end()) return std::nullopt;
    return it->second;
}

std::map<std::string, int> UiResources::parseIndex(std::string_view text) {
    std::map<std::string, int> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;
        const size_t tab = line.find('\t');
        if (tab == std::string_view::npos || tab == 0 || tab + 1 >= line.size()) continue;
        int id = 0;
        const auto [next, ec] = std::from_chars(line.data(), line.data() + tab, id);
        if (ec != std::errc() || next != line.data() + tab || id <= kIndexId || id > 0xFFFF) continue;
        out[std::string(line.substr(tab + 1))] = id;
    }
    return out;
}

std::optional<std::string> uiPathFromUri(std::string_view uri, std::string_view origin) {
    if (origin.empty() || uri.size() < origin.size() || !iequals(uri.substr(0, origin.size()), origin))
        return std::nullopt;
    std::string_view rest = uri.substr(origin.size());
    if (!rest.empty() && rest.front() != '/' && rest.front() != '?' && rest.front() != '#')
        return std::nullopt;  // "https://app.sshtermaq.local.evil" is not our origin
    rest = rest.substr(0, rest.find_first_of("?#"));
    while (!rest.empty() && rest.front() == '/') rest.remove_prefix(1);

    std::string path;
    path.reserve(rest.size());
    for (size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] != '%') {
            path += rest[i];
            continue;
        }
        if (i + 2 >= rest.size()) return std::nullopt;
        const int hi = hexDigit(rest[i + 1]);
        const int lo = hexDigit(rest[i + 2]);
        if (hi < 0 || lo < 0) return std::nullopt;
        path += static_cast<char>(hi * 16 + lo);
        i += 2;
    }
    if (path.empty() || path.back() == '/') path += "index.html";
    return path;
}

std::string uiContentType(std::string_view path) {
    const size_t slash = path.find_last_of('/');
    const size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return "application/octet-stream";
    const std::string ext = toLower(path.substr(dot + 1));
    static const std::map<std::string, std::string, std::less<>> kTypes = {
        {"html", "text/html; charset=utf-8"},
        {"htm", "text/html; charset=utf-8"},
        {"js", "text/javascript; charset=utf-8"},
        {"mjs", "text/javascript; charset=utf-8"},
        {"css", "text/css; charset=utf-8"},
        {"json", "application/json; charset=utf-8"},
        {"map", "application/json; charset=utf-8"},
        {"txt", "text/plain; charset=utf-8"},
        {"svg", "image/svg+xml"},
        {"png", "image/png"},
        {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},
        {"webp", "image/webp"},
        {"ico", "image/x-icon"},
        {"woff", "font/woff"},
        {"woff2", "font/woff2"},
        {"ttf", "font/ttf"},
        {"wasm", "application/wasm"},
        {"wav", "audio/wav"},
        {"mp3", "audio/mpeg"},
    };
    auto it = kTypes.find(ext);
    return it != kTypes.end() ? it->second : "application/octet-stream";
}

}  // namespace st
