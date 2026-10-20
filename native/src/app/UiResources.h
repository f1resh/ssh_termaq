#pragma once
// The web UI (ui/dist) embedded in the executable and served to the web view from memory, so the program needs no
// `ui` folder next to it.
//
// Build: cmake/EmbedUi.cmake numbers the files of ui/dist (2, 3, ...) and embeds them (Windows: a generated resource
// script, resources of type "UIFILE"; macOS: two sections of the executable); file #1 is the index, one "<id>\t<path>"
// line per file, paths relative to ui/dist with '/' separators ("index.html", "assets/index-1a2b3c.js").
// platform/ui/EmbeddedUi.h reads them. Run time: the host window answers the web view's requests under its app origin
// (Windows: https://app.sshtermaq.local/<path>, macOS: sshtermaq://app/<path>) with find(path) (app/App.cpp). Lookups are exact matches in an in-memory table, never file system access.
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace st {

class UiResources {
public:
    /** Reads the index and the files embedded in this executable; empty when it was built without ui/dist. */
    static UiResources load();

    bool empty() const { return files_.empty(); }
    size_t size() const { return files_.size(); }

    /** Content of an embedded file (points into the executable's image, valid for the process); nullopt if absent. */
    std::optional<std::string_view> find(const std::string& path) const;

    /** Parses index text into path -> resource id. Blank lines, '#' comments and malformed lines are skipped. */
    static std::map<std::string, int> parseIndex(std::string_view text);

private:
    std::map<std::string, std::string_view> files_;
};

/**
 * Path relative to the UI root for a request URI under `origin` ("https://app.sshtermaq.local"): query and fragment
 * dropped, percent-decoded, "" or a trailing '/' -> "index.html". nullopt when the URI is not under `origin` or has a
 * malformed escape. The origin comparison ignores case.
 */
std::optional<std::string> uiPathFromUri(std::string_view uri, std::string_view origin);

/** HTTP Content-Type for a file name, by extension ("application/octet-stream" when unknown). */
std::string uiContentType(std::string_view path);

}  // namespace st
