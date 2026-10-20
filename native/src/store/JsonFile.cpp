#include "store/JsonFile.h"

#include "core/Log.h"
#include "core/Strings.h"
#include "platform/sys/Errors.h"
#include "platform/sys/Fs.h"
#include "platform/sys/Time.h"

#include <cstdio>
#include <ctime>
#include <optional>
#include <stdexcept>

namespace fs = std::filesystem;

namespace st {

namespace {

std::string timestampForFileName() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    sys::localTime(t, tm);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec);
    return buf;
}

}  // namespace

namespace storeio {

fs::path backupCorrupt(const fs::path& path, const std::string& reason) {
    fs::path target = path;
    target += ".corrupt-" + timestampForFileName();
    std::string error;
    bool ok = sys::renameReplace(path, target, error);
    if (!ok) {
        std::error_code ec;
        ok = fs::copy_file(path, target, fs::copy_options::overwrite_existing, ec);
        if (!ok) error = ec.message();
    }
    if (ok) {
        applog::warn("Store: " + sys::fromPath(path.filename()) + " is corrupt (" + reason + "); backed up as " +
                     sys::fromPath(target.filename()));
        return target;
    }
    applog::error("Store: " + sys::fromPath(path.filename()) + " is corrupt (" + reason +
                  ") and could not be backed up: " + error);
    return {};
}

LoadStatus loadJson(const fs::path& path, Json& out) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return LoadStatus::Missing;
    auto text = sys::readFile(path);
    if (!text) {
        applog::warn("Store: cannot read " + sys::fromPath(path.filename()) + ": " + sys::lastErrorMessage());
        return LoadStatus::Missing;
    }
    std::string_view view(*text);
    if (startsWith(view, "\xEF\xBB\xBF")) view.remove_prefix(3);
    if (trim(view).empty()) {
        backupCorrupt(path, "empty file");
        return LoadStatus::Corrupt;
    }
    try {
        out = Json::parse(view.begin(), view.end(), nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
        return LoadStatus::Ok;
    } catch (const std::exception& e) {
        backupCorrupt(path, e.what());
        return LoadStatus::Corrupt;
    }
}

void saveJson(const fs::path& path, const Json& value) {
    std::string text = value.dump(2, ' ', false, Json::error_handler_t::replace);
    text.push_back('\n');
    sys::writeFileAtomic(path, text);
}

}  // namespace storeio
}  // namespace st
