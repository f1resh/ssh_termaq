#include "store/JsonFile.h"

#include "core/Log.h"
#include "core/Strings.h"
#include "core/Win.h"
#include "store/Store.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <optional>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;

namespace sc {

namespace {

/** Minimal RAII wrapper for a Win32 file HANDLE (sc_store does not depend on WIL). */
class FileHandle {
public:
    explicit FileHandle(HANDLE h) : h_(h) {}
    ~FileHandle() { reset(); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    bool valid() const { return h_ != INVALID_HANDLE_VALUE && h_ != nullptr; }
    HANDLE get() const { return h_; }
    /** Closes now; returns false if CloseHandle failed. */
    bool reset() {
        bool ok = true;
        if (valid()) ok = CloseHandle(h_) != FALSE;
        h_ = INVALID_HANDLE_VALUE;
        return ok;
    }

private:
    HANDLE h_;
};

bool isTransientShareError(DWORD err) {
    return err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION;
}

std::string timestampForFileName() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec);
    return buf;
}

}  // namespace

void writeFileAtomic(const fs::path& path, const std::string& content) {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);

    static std::atomic<unsigned> counter{0};
    fs::path tmp = path;
    tmp += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++counter);

    {
        FileHandle h(CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!h.valid()) throw std::runtime_error("Cannot create " + narrow(tmp.wstring()) + ": " + lastErrorMessage());
        size_t off = 0;
        while (off < content.size()) {
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(content.size() - off, 1u << 30));
            DWORD written = 0;
            if (!WriteFile(h.get(), content.data() + off, chunk, &written, nullptr) || written == 0) {
                std::string err = lastErrorMessage();
                h.reset();
                DeleteFileW(tmp.c_str());
                throw std::runtime_error("Cannot write " + narrow(path.wstring()) + ": " + err);
            }
            off += written;
        }
        if (!FlushFileBuffers(h.get())) {
            std::string err = lastErrorMessage();
            h.reset();
            DeleteFileW(tmp.c_str());
            throw std::runtime_error("Cannot flush " + narrow(path.wstring()) + ": " + err);
        }
        if (!h.reset()) {
            DeleteFileW(tmp.c_str());
            throw std::runtime_error("Cannot close " + narrow(path.wstring()) + ": " + lastErrorMessage());
        }
    }

    // Antivirus scanners / the search indexer may briefly hold the destination open: retry a few times.
    DWORD err = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return;
        err = GetLastError();
        if (!isTransientShareError(err)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(15 * (attempt + 1)));
    }
    DeleteFileW(tmp.c_str());
    throw std::runtime_error("Cannot replace " + narrow(path.wstring()) + ": " + winErrorMessage(err));
}

std::optional<std::string> readFile(const fs::path& path) {
    FileHandle h(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!h.valid()) return std::nullopt;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h.get(), &size) || size.QuadPart < 0) return std::nullopt;
    constexpr long long kMax = 512ll * 1024 * 1024;  // refuse absurdly large files
    if (size.QuadPart > kMax) return std::nullopt;
    std::string out(static_cast<size_t>(size.QuadPart), '\0');
    size_t off = 0;
    while (off < out.size()) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(out.size() - off, 1u << 30));
        DWORD got = 0;
        if (!ReadFile(h.get(), out.data() + off, chunk, &got, nullptr)) return std::nullopt;
        if (got == 0) break;  // file shrank while reading
        off += got;
    }
    out.resize(off);
    return out;
}

namespace storeio {

fs::path backupCorrupt(const fs::path& path, const std::string& reason) {
    fs::path target = path;
    target += ".corrupt-" + timestampForFileName();
    bool ok = MoveFileExW(path.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ||
              CopyFileW(path.c_str(), target.c_str(), FALSE);
    if (ok) {
        applog::warn("Store: " + narrow(path.filename().wstring()) + " is corrupt (" + reason + "); backed up as " +
                     narrow(target.filename().wstring()));
        return target;
    }
    applog::error("Store: " + narrow(path.filename().wstring()) + " is corrupt (" + reason +
                  ") and could not be backed up: " + lastErrorMessage());
    return {};
}

LoadStatus loadJson(const fs::path& path, Json& out) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return LoadStatus::Missing;
    auto text = readFile(path);
    if (!text) {
        applog::warn("Store: cannot read " + narrow(path.filename().wstring()) + ": " + lastErrorMessage());
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
    writeFileAtomic(path, text);
}

}  // namespace storeio
}  // namespace sc
