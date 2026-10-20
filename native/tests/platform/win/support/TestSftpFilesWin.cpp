// Windows file access of the test SFTP server (TestSftpFiles.h): CreateFileW sharing read / write / delete,
// overlapped offsets, FindFirstFileW listings, the read-only attribute.
#include "TestSftpFiles.h"

#include "platform/detail/WinApi.h"
#include "platform/win/sys/WinText.h"

#include <algorithm>
#include <memory>

namespace st::test::sftpfiles {

namespace {

constexpr uint64_t kEpochDiff = 116444736000000000ULL;  // 1601-01-01 -> 1970-01-01 in 100ns units

HANDLE handle(std::intptr_t h) { return reinterpret_cast<HANDLE>(h); }

uint32_t toUnix(const FILETIME& ft) {
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    if (u.QuadPart < kEpochDiff) return 0;
    return static_cast<uint32_t>((u.QuadPart - kEpochDiff) / 10000000ULL);
}

FILETIME fromUnix(uint32_t t) {
    ULARGE_INTEGER u{};
    u.QuadPart = static_cast<uint64_t>(t) * 10000000ULL + kEpochDiff;
    FILETIME ft{};
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    return ft;
}

Stat makeStat(DWORD fileAttributes, uint64_t size, const FILETIME& atime, const FILETIME& mtime) {
    Stat s;
    s.isDir = (fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    s.readOnly = (fileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    s.size = size;
    s.atime = toUnix(atime);
    s.mtime = toUnix(mtime);
    return s;
}

struct HandleCloser {
    void operator()(HANDLE h) const {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct FindCloser {
    void operator()(HANDLE h) const {
        if (h && h != INVALID_HANDLE_VALUE) FindClose(h);
    }
};
using UniqueFind = std::unique_ptr<void, FindCloser>;

}  // namespace

ErrorKind classify(OsError e) {
    switch (e) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME:
        case ERROR_INVALID_DRIVE:
            return ErrorKind::NoSuchFile;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_WRITE_PROTECT:
            return ErrorKind::PermissionDenied;
        default:
            return ErrorKind::Other;
    }
}

OsError stat(const std::filesystem::path& p, Stat& out) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return GetLastError();
    const uint64_t size = (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    out = makeStat(d.dwFileAttributes, size, d.ftLastAccessTime, d.ftLastWriteTime);
    return ERROR_SUCCESS;
}

OsError list(const std::filesystem::path& dir, std::vector<std::pair<std::string, Stat>>& out) {
    WIN32_FIND_DATAW fd{};
    UniqueFind find(FindFirstFileW((dir / L"*").c_str(), &fd));
    if (find.get() == INVALID_HANDLE_VALUE) return GetLastError();
    do {
        const uint64_t size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        out.emplace_back(st::narrow(fd.cFileName), makeStat(fd.dwFileAttributes, size, fd.ftLastAccessTime, fd.ftLastWriteTime));
    } while (FindNextFileW(find.get(), &fd));
    return ERROR_SUCCESS;
}

OsError truncate(const std::filesystem::path& p, std::uint64_t size) {
    UniqueHandle h(CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (h.get() == INVALID_HANDLE_VALUE) return GetLastError();
    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFilePointerEx(h.get(), li, nullptr, FILE_BEGIN) || !SetEndOfFile(h.get())) return GetLastError();
    return ERROR_SUCCESS;
}

OsError setTimes(const std::filesystem::path& p, std::uint32_t atime, std::uint32_t mtime) {
    UniqueHandle h(CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (h.get() == INVALID_HANDLE_VALUE) return GetLastError();
    const FILETIME at = fromUnix(atime), mt = fromUnix(mtime);
    if (!SetFileTime(h.get(), nullptr, &at, &mt)) return GetLastError();
    return ERROR_SUCCESS;
}

OsError setWritable(const std::filesystem::path& p, bool writable) {
    const DWORD fa = GetFileAttributesW(p.c_str());
    if (fa == INVALID_FILE_ATTRIBUTES) return GetLastError();
    if (!(fa & FILE_ATTRIBUTE_DIRECTORY)) {
        const DWORD want = writable ? (fa & ~FILE_ATTRIBUTE_READONLY) : (fa | FILE_ATTRIBUTE_READONLY);
        if (want != fa && !SetFileAttributesW(p.c_str(), want)) return GetLastError();
    }
    return ERROR_SUCCESS;
}

OsError rename(const std::filesystem::path& from, const std::filesystem::path& to, bool replace) {
    if (!MoveFileExW(from.c_str(), to.c_str(), replace ? MOVEFILE_REPLACE_EXISTING : 0)) return GetLastError();
    return ERROR_SUCCESS;
}

OsError removeFile(const std::filesystem::path& p) { return DeleteFileW(p.c_str()) ? ERROR_SUCCESS : GetLastError(); }

OsError makeDir(const std::filesystem::path& p) {
    return CreateDirectoryW(p.c_str(), nullptr) ? ERROR_SUCCESS : GetLastError();
}

OsError removeDir(const std::filesystem::path& p) { return RemoveDirectoryW(p.c_str()) ? ERROR_SUCCESS : GetLastError(); }

File::~File() {
    if (h_ != -1) CloseHandle(handle(h_));
}

OsError File::open(const std::filesystem::path& p, const OpenFlags& f) {
    DWORD access = 0;
    if (f.read) access |= GENERIC_READ;
    if (f.write) access |= GENERIC_WRITE;
    DWORD disposition = OPEN_EXISTING;
    if (f.create) {
        if (f.exclusive) disposition = CREATE_NEW;
        else if (f.truncate) disposition = CREATE_ALWAYS;
        else disposition = OPEN_ALWAYS;
    } else if (f.truncate) {
        disposition = TRUNCATE_EXISTING;
    }
    HANDLE h = CreateFileW(p.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disposition,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    h_ = reinterpret_cast<std::intptr_t>(h);
    return ERROR_SUCCESS;
}

OsError File::read(std::uint64_t offset, char* buf, std::uint32_t len, std::uint32_t& got) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD n = 0;
    got = 0;
    if (!ReadFile(handle(h_), buf, len, &n, &ov)) {
        const DWORD err = GetLastError();
        return err == ERROR_HANDLE_EOF ? ERROR_SUCCESS : err;
    }
    got = n;
    return ERROR_SUCCESS;
}

OsError File::write(std::uint64_t offset, const char* data, std::size_t len) {
    size_t done = 0;
    while (done < len) {
        OVERLAPPED ov{};
        const uint64_t at = offset + done;
        ov.Offset = static_cast<DWORD>(at);
        ov.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD wrote = 0;
        const auto chunk = static_cast<DWORD>(std::min<size_t>(len - done, 1 << 20));
        if (!WriteFile(handle(h_), data + done, chunk, &wrote, &ov)) return GetLastError();
        if (wrote == 0) return ERROR_WRITE_FAULT;
        done += wrote;
    }
    return ERROR_SUCCESS;
}

OsError File::size(std::uint64_t& out) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle(h_), &size)) return GetLastError();
    out = static_cast<uint64_t>(size.QuadPart);
    return ERROR_SUCCESS;
}

OsError File::stat(Stat& out) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle(h_), &info)) return GetLastError();
    const uint64_t size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    out = makeStat(info.dwFileAttributes, size, info.ftLastAccessTime, info.ftLastWriteTime);
    return ERROR_SUCCESS;
}

OsError File::flush() { return FlushFileBuffers(handle(h_)) ? ERROR_SUCCESS : GetLastError(); }

}  // namespace st::test::sftpfiles
