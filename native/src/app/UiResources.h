#pragma once
// The web UI (ui/dist) embedded in SSHTerm.exe and served to WebView2 from memory, so the program needs no `ui`
// folder next to it.
//
// Build: cmake/EmbedUi.cmake turns ui/dist into a generated resource script. Every file becomes a resource of type
// "UIFILE" with a numeric id (2, 3, ...); resource UIFILE #1 is the index, one "<id>\t<path>" line per file, paths
// relative to ui/dist with '/' separators ("index.html", "assets/index-1a2b3c.js").
// Run time: MainWindow answers WebView2's requests for https://app.sshterm.local/<path> with find(path). Lookups are
// exact matches in an in-memory table, never file system access.
#include "core/Win.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace st {

class UiResources {
public:
    /** Reads the index and the files from `module`'s resources; empty when it was built without ui/dist. */
    static UiResources load(HMODULE module);

    bool empty() const { return files_.empty(); }
    size_t size() const { return files_.size(); }

    /** Content of an embedded file (points into the loaded module, valid for its lifetime); nullopt if absent. */
    std::optional<std::string_view> find(const std::string& path) const;

    /** Parses index text into path -> resource id. Blank lines, '#' comments and malformed lines are skipped. */
    static std::map<std::string, int> parseIndex(std::string_view text);

private:
    std::map<std::string, std::string_view> files_;
};

/**
 * Path relative to the UI root for a request URI under `origin` ("https://app.sshterm.local"): query and fragment
 * dropped, percent-decoded, "" or a trailing '/' -> "index.html". nullopt when the URI is not under `origin` or has a
 * malformed escape. The origin comparison ignores case.
 */
std::optional<std::string> uiPathFromUri(std::string_view uri, std::string_view origin);

/** HTTP Content-Type for a file name, by extension ("application/octet-stream" when unknown). */
std::string uiContentType(std::string_view path);

}  // namespace st
