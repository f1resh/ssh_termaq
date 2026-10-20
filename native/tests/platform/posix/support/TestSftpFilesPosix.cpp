// POSIX file access of the test SFTP server (TestSftpFiles.h): open / pread / pwrite, stat, utimensat, and renames
// that keep an existing target the way OpenSSH's sftp-server does (link + unlink for files).
#include "TestSftpFiles.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

namespace st::test::sftpfiles {

namespace {

OsError lastError() { return static_cast<OsError>(errno); }

Stat makeStat(const struct stat& st) {
    Stat s;
    s.isDir = S_ISDIR(st.st_mode);
    s.readOnly = (st.st_mode & S_IWUSR) == 0;
    s.size = static_cast<std::uint64_t>(st.st_size);
    s.atime = static_cast<std::uint32_t>(st.st_atime);
    s.mtime = static_cast<std::uint32_t>(st.st_mtime);
    return s;
}

}  // namespace

ErrorKind classify(OsError e) {
    switch (e) {
        case ENOENT:
        case ENOTDIR:
        case ENAMETOOLONG:
            return ErrorKind::NoSuchFile;
        case EACCES:
        case EPERM:
        case EROFS:
            return ErrorKind::PermissionDenied;
        default:
            return ErrorKind::Other;
    }
}

OsError stat(const std::filesystem::path& p, Stat& out) {
    struct stat st {};
    if (::stat(p.c_str(), &st) != 0) return lastError();
    out = makeStat(st);
    return 0;
}

OsError list(const std::filesystem::path& dir, std::vector<std::pair<std::string, Stat>>& out) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) return lastError();
    while (const dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        struct stat st {};
        // What a link points to, as for files; a dangling link as itself.
        if (::fstatat(dirfd(d), e->d_name, &st, 0) != 0 && ::fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
            continue;
        out.emplace_back(name, makeStat(st));
    }
    ::closedir(d);
    return 0;
}

OsError truncate(const std::filesystem::path& p, std::uint64_t size) {
    return ::truncate(p.c_str(), static_cast<off_t>(size)) == 0 ? 0 : lastError();
}

OsError setTimes(const std::filesystem::path& p, std::uint32_t atime, std::uint32_t mtime) {
    const timespec times[2] = {{static_cast<time_t>(atime), 0}, {static_cast<time_t>(mtime), 0}};
    return ::utimensat(AT_FDCWD, p.c_str(), times, 0) == 0 ? 0 : lastError();
}

OsError setWritable(const std::filesystem::path& p, bool writable) {
    struct stat st {};
    if (::stat(p.c_str(), &st) != 0) return lastError();
    if (S_ISDIR(st.st_mode)) return 0;
    const mode_t mode = st.st_mode & 07777;
    const mode_t want = writable ? (mode | S_IWUSR) : (mode & ~static_cast<mode_t>(S_IWUSR | S_IWGRP | S_IWOTH));
    if (want != mode && ::chmod(p.c_str(), want) != 0) return lastError();
    return 0;
}

OsError rename(const std::filesystem::path& from, const std::filesystem::path& to, bool replace) {
    if (replace) return ::rename(from.c_str(), to.c_str()) == 0 ? 0 : lastError();
    struct stat st {};
    if (::lstat(from.c_str(), &st) != 0) return lastError();
    if (S_ISREG(st.st_mode)) {
        // A hard link fails when the target exists; then the old name goes.
        if (::link(from.c_str(), to.c_str()) == 0) {
            if (::unlink(from.c_str()) == 0) return 0;
            const OsError err = lastError();
            ::unlink(to.c_str());
            return err;
        }
        if (errno != EPERM && errno != ENOTSUP && errno != EXDEV) return lastError();
    }
    // Folders (and file systems without hard links): rename when nothing is in the way.
    if (::lstat(to.c_str(), &st) == 0) return EEXIST;
    return ::rename(from.c_str(), to.c_str()) == 0 ? 0 : lastError();
}

OsError removeFile(const std::filesystem::path& p) { return ::unlink(p.c_str()) == 0 ? 0 : lastError(); }

OsError makeDir(const std::filesystem::path& p) { return ::mkdir(p.c_str(), 0755) == 0 ? 0 : lastError(); }

OsError removeDir(const std::filesystem::path& p) { return ::rmdir(p.c_str()) == 0 ? 0 : lastError(); }

File::~File() {
    if (h_ != -1) ::close(static_cast<int>(h_));
}

OsError File::open(const std::filesystem::path& p, const OpenFlags& f) {
    int flags = f.read && f.write ? O_RDWR : f.write ? O_WRONLY : O_RDONLY;
    if (f.create) flags |= O_CREAT;
    if (f.create && f.exclusive) flags |= O_EXCL;
    if (f.truncate) flags |= O_TRUNC;
    const int fd = ::open(p.c_str(), flags | O_CLOEXEC, 0644);
    if (fd < 0) return lastError();
    h_ = fd;
    return 0;
}

OsError File::read(std::uint64_t offset, char* buf, std::uint32_t len, std::uint32_t& got) {
    got = 0;
    ssize_t n;
    do {
        n = ::pread(static_cast<int>(h_), buf, len, static_cast<off_t>(offset));
    } while (n < 0 && errno == EINTR);
    if (n < 0) return lastError();
    got = static_cast<std::uint32_t>(n);
    return 0;
}

OsError File::write(std::uint64_t offset, const char* data, std::size_t len) {
    std::size_t done = 0;
    while (done < len) {
        const ssize_t n = ::pwrite(static_cast<int>(h_), data + done, len - done, static_cast<off_t>(offset + done));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return lastError();
        if (n == 0) return EIO;
        done += static_cast<std::size_t>(n);
    }
    return 0;
}

OsError File::size(std::uint64_t& out) {
    struct stat st {};
    if (::fstat(static_cast<int>(h_), &st) != 0) return lastError();
    out = static_cast<std::uint64_t>(st.st_size);
    return 0;
}

OsError File::stat(Stat& out) {
    struct stat st {};
    if (::fstat(static_cast<int>(h_), &st) != 0) return lastError();
    out = makeStat(st);
    return 0;
}

OsError File::flush() { return ::fsync(static_cast<int>(h_)) == 0 ? 0 : lastError(); }

}  // namespace st::test::sftpfiles
