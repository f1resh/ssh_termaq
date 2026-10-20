#pragma once
// The local file access of the test SFTP server (TestSftpServer.cpp), per OS (native/tests/platform/<os>/support/
// TestSftpFiles<Os>.cpp): the protocol is shared, the file system calls are the OS's own, so the server behaves like a
// real one there (sharing, errors, timestamps). Errors are the OS's codes (Windows: GetLastError(), POSIX: errno) and
// read with sys::errorMessage; 0 = success.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace st::test::sftpfiles {

using OsError = unsigned long;

struct Stat {
    bool isDir = false;
    bool readOnly = false;               // Windows: the read-only attribute; POSIX: no owner write permission
    std::uint64_t size = 0;
    std::uint32_t atime = 0, mtime = 0;  // Unix seconds
};

enum class ErrorKind { NoSuchFile, PermissionDenied, Other };
/** What an OS error means to an SFTP client. */
ErrorKind classify(OsError e);

OsError stat(const std::filesystem::path& p, Stat& out);
/** A folder's entries, with "." and ".." when the OS lists them. */
OsError list(const std::filesystem::path& dir, std::vector<std::pair<std::string, Stat>>& out);
OsError truncate(const std::filesystem::path& p, std::uint64_t size);
OsError setTimes(const std::filesystem::path& p, std::uint32_t atime, std::uint32_t mtime);
/** Makes a file read-only or writable again (folders: no change). */
OsError setWritable(const std::filesystem::path& p, bool writable);
/** Fails when `to` exists, unless `replace`. */
OsError rename(const std::filesystem::path& from, const std::filesystem::path& to, bool replace);
OsError removeFile(const std::filesystem::path& p);
OsError makeDir(const std::filesystem::path& p);
OsError removeDir(const std::filesystem::path& p);

struct OpenFlags {
    bool read = false, write = false;
    bool create = false, exclusive = false, truncate = false;
};

/** An open file that others may keep reading, writing and deleting; read and written at explicit offsets. */
class File {
public:
    File() = default;
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    OsError open(const std::filesystem::path& p, const OpenFlags& flags);
    /** got = 0 at the end of the file. */
    OsError read(std::uint64_t offset, char* buf, std::uint32_t len, std::uint32_t& got);
    OsError write(std::uint64_t offset, const char* data, std::size_t len);
    OsError size(std::uint64_t& out);
    OsError stat(Stat& out);
    OsError flush();

private:
    std::intptr_t h_ = -1;  // HANDLE on Windows, file descriptor on POSIX
};

}  // namespace st::test::sftpfiles
