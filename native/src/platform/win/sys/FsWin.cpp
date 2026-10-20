#include "platform/sys/Fs.h"

#include "platform/detail/WinApi.h"
#include "platform/sys/Errors.h"
#include "platform/win/sys/WinText.h"

#include <share.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;

namespace st::sys {

namespace {

// 100 ns intervals between 1601-01-01 and 1970-01-01.
constexpr int64_t kEpochDiff100ns = 116444736000000000LL;

int64_t fileTimeToUnix(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (static_cast<int64_t>(u.QuadPart) - kEpochDiff100ns) / 10000000LL;
}

FILETIME unixToFileTime(int64_t seconds) {
    ULARGE_INTEGER u;
    u.QuadPart = static_cast<ULONGLONG>(seconds * 10000000LL + kEpochDiff100ns);
    FILETIME ft;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    return ft;
}

/** Minimal RAII wrapper for a Win32 file HANDLE. */
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

HANDLE toHandle(std::intptr_t h) { return reinterpret_cast<HANDLE>(h); }

}  // namespace

/* ---- paths as UTF-8 text ------------------------------------------------------------------------------------ */

fs::path toPath(std::string_view utf8) { return fs::path(widen(utf8)); }

std::string fromPath(const fs::path& p) { return narrow(p.native()); }

std::string fromGenericPath(const fs::path& p) { return narrow(p.generic_wstring()); }

std::string sanitizeFileName(std::string_view utf8Name) {
    std::wstring w = widen(utf8Name);
    for (auto& ch : w) {
        if (ch < 0x20 || ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' || ch == L'/' || ch == L'\\' ||
            ch == L'|' || ch == L'?' || ch == L'*') {
            ch = L'_';
        }
    }
    while (!w.empty() && (w.back() == L'.' || w.back() == L' ')) w.pop_back();
    if (w.empty()) return "_";

    // Reserved DOS device names, also when followed by an extension ("nul.txt").
    std::wstring stem = w.substr(0, w.find(L'.'));
    for (auto& ch : stem) ch = static_cast<wchar_t>(towupper(ch));
    static const wchar_t* const kReserved[] = {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"};
    bool reserved = false;
    for (const auto* r : kReserved) reserved = reserved || stem == r;
    if (!reserved && stem.size() == 4 && (stem.rfind(L"COM", 0) == 0 || stem.rfind(L"LPT", 0) == 0) &&
        stem[3] >= L'0' && stem[3] <= L'9') {
        reserved = true;
    }
    if (reserved) w.insert(w.begin(), L'_');
    return narrow(w);
}

/* ---- information ---------------------------------------------------------------------------------------------- */

std::optional<FileInfo> fileInfo(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return std::nullopt;
    FileInfo info;
    info.isDir = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    info.isLink = (fad.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    info.size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    info.mtime = fileTimeToUnix(fad.ftLastWriteTime);
    info.writeStamp =
        (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) | fad.ftLastWriteTime.dwLowDateTime;
    return info;
}

bool isReadableNow(const fs::path& p) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

/* ---- whole files ---------------------------------------------------------------------------------------------- */

void writeFileAtomic(const fs::path& path, const std::string& content) {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);

    static std::atomic<unsigned> counter{0};
    fs::path tmp = path;
    tmp += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++counter);

    {
        FileHandle h(CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!h.valid()) throw std::runtime_error("Cannot create " + fromPath(tmp) + ": " + lastErrorMessage());
        size_t off = 0;
        while (off < content.size()) {
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(content.size() - off, 1u << 30));
            DWORD written = 0;
            if (!WriteFile(h.get(), content.data() + off, chunk, &written, nullptr) || written == 0) {
                std::string err = lastErrorMessage();
                h.reset();
                DeleteFileW(tmp.c_str());
                throw std::runtime_error("Cannot write " + fromPath(path) + ": " + err);
            }
            off += written;
        }
        if (!FlushFileBuffers(h.get())) {
            std::string err = lastErrorMessage();
            h.reset();
            DeleteFileW(tmp.c_str());
            throw std::runtime_error("Cannot flush " + fromPath(path) + ": " + err);
        }
        if (!h.reset()) {
            DeleteFileW(tmp.c_str());
            throw std::runtime_error("Cannot close " + fromPath(path) + ": " + lastErrorMessage());
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
    throw std::runtime_error("Cannot replace " + fromPath(path) + ": " + errorMessage(err));
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

bool renameReplace(const fs::path& from, const fs::path& to, std::string& error) {
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH))
        return true;
    error = lastErrorMessage();
    return false;
}

bool removeFile(const fs::path& p, std::string& error) {
    if (DeleteFileW(p.c_str())) return true;
    error = lastErrorMessage();
    return false;
}

std::FILE* openExclusiveWriter(const fs::path& p, bool append) {
    return _wfsopen(p.c_str(), append ? L"ab" : L"wb", _SH_DENYWR);
}

/* ---- open files ----------------------------------------------------------------------------------------------- */

File::~File() { close(); }

File::File(File&& other) noexcept : handle_(other.handle_), error_(std::move(other.error_)) { other.handle_ = kInvalid; }

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        error_ = std::move(other.error_);
        other.handle_ = kInvalid;
    }
    return *this;
}

bool File::open(const fs::path& p, Mode mode) {
    close();
    HANDLE h = INVALID_HANDLE_VALUE;
    switch (mode) {
        case Mode::Read:
            h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            break;
        case Mode::Write:
        case Mode::WriteKeep:
            h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            mode == Mode::WriteKeep ? OPEN_ALWAYS : CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            break;
    }
    if (h == INVALID_HANDLE_VALUE) {
        error_ = lastErrorMessage();
        return false;
    }
    handle_ = reinterpret_cast<std::intptr_t>(h);
    return true;
}

bool File::seek(std::uint64_t offset) {
    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(offset);
    if (SetFilePointerEx(toHandle(handle_), pos, nullptr, FILE_BEGIN)) return true;
    error_ = lastErrorMessage();
    return false;
}

bool File::truncate() {
    if (SetEndOfFile(toHandle(handle_))) return true;
    error_ = lastErrorMessage();
    return false;
}

std::int64_t File::read(void* buf, std::size_t n) {
    DWORD got = 0;
    if (!ReadFile(toHandle(handle_), buf, static_cast<DWORD>(std::min<std::size_t>(n, 1u << 30)), &got, nullptr)) {
        error_ = lastErrorMessage();
        return -1;
    }
    return got;
}

bool File::writeAll(const void* buf, std::size_t n) {
    const auto* p = static_cast<const char*>(buf);
    size_t off = 0;
    while (off < n) {
        DWORD written = 0;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(n - off, 1u << 30));
        if (!WriteFile(toHandle(handle_), p + off, want, &written, nullptr)) {
            error_ = lastErrorMessage();
            return false;
        }
        off += written;
    }
    return true;
}

bool File::setTimes(std::int64_t unixSeconds) {
    const FILETIME ft = unixToFileTime(unixSeconds);
    if (SetFileTime(toHandle(handle_), nullptr, &ft, &ft)) return true;
    error_ = lastErrorMessage();
    return false;
}

bool File::close() {
    if (handle_ == kInvalid) return true;
    const bool ok = CloseHandle(toHandle(handle_)) != FALSE;
    if (!ok) error_ = lastErrorMessage();
    handle_ = kInvalid;
    return ok;
}

bool caseSensitiveFolder(const fs::path&) { return false; }  // NTFS, FAT and exFAT folders ignore letter case

/* ---- downloaded files ----------------------------------------------------------------------------------------- */

bool isExecutableType(const fs::path& file) {
    static const wchar_t* kTypes[] = {L".exe", L".com", L".bat", L".cmd", L".scr", L".pif", L".msi", L".msp", L".msc",
                                      L".ps1", L".psm1", L".vbs", L".vbe", L".js", L".jse", L".wsf", L".wsh", L".hta",
                                      L".cpl", L".jar", L".lnk", L".url", L".reg", L".inf", L".appref-ms", L".application",
                                      L".gadget", L".chm", L".dll", L".sys", L".ocx", L".scf", L".settingcontent-ms"};
    std::wstring ext = file.extension().wstring();
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    for (const wchar_t* t : kTypes)
        if (ext == t) return true;
    return false;
}

void markDownloaded(const fs::path& file) {
    std::ofstream ads(file.wstring() + L":Zone.Identifier", std::ios::binary | std::ios::trunc);
    if (ads) ads << "[ZoneTransfer]\r\nZoneId=3\r\n";
}

}  // namespace st::sys
