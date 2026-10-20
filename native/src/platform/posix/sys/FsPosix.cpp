// Files on POSIX: open(2) with O_CLOEXEC, atomic replacement by rename(2), advisory flock(2) for exclusive writers.
#include "platform/sys/Fs.h"

#include "platform/detail/PosixFs.h"
#include "platform/sys/Errors.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

namespace st::sys {

namespace {

int openFd(const fs::path& p, int flags, mode_t mode = 0) {
    int fd;
    do {
        fd = ::open(p.c_str(), flags | O_CLOEXEC, mode);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

void closeFd(int fd) {
    if (fd >= 0) ::close(fd);
}

/** Writes all of `n` bytes; false with errno set on failure. */
bool writeFully(int fd, const char* p, size_t n) {
    while (n > 0) {
        const ssize_t w = ::write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

/** Length of the valid UTF-8 sequence starting at s[i], or 0 if it is invalid. */
size_t utf8SequenceLength(std::string_view s, size_t i) {
    const auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    if (c < 0x80) return 1;
    size_t len;
    unsigned char lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) {
        len = 2;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3;
        if (c == 0xE0) lo = 0xA0;
        if (c == 0xED) hi = 0x9F;  // no surrogates
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
        if (c == 0xF0) lo = 0x90;
        if (c == 0xF4) hi = 0x8F;
    } else {
        return 0;
    }
    if (i + len > s.size()) return 0;
    if (b(i + 1) < lo || b(i + 1) > hi) return 0;
    for (size_t k = 2; k < len; ++k)
        if (b(i + k) < 0x80 || b(i + k) > 0xBF) return 0;
    return len;
}

}  // namespace

/* ---- paths as UTF-8 text ------------------------------------------------------------------------------------ */

fs::path toPath(std::string_view utf8) { return fs::path(std::string(utf8)); }

std::string fromPath(const fs::path& p) { return p.native(); }

std::string fromGenericPath(const fs::path& p) { return p.generic_string(); }

std::string sanitizeFileName(std::string_view utf8Name) {
    // '/' and NUL cannot appear in a name, control characters are kept out, and names must be valid UTF-8 (APFS
    // refuses anything else): each offending byte becomes '_'. "." and ".." would name a folder, not a file.
    std::string out;
    out.reserve(utf8Name.size());
    for (size_t i = 0; i < utf8Name.size();) {
        const size_t len = utf8SequenceLength(utf8Name, i);
        const unsigned char c = static_cast<unsigned char>(utf8Name[i]);
        if (len == 0 || c < 0x20 || c == 0x7F || c == '/') {
            out += '_';
            ++i;
        } else {
            out.append(utf8Name.substr(i, len));
            i += len;
        }
    }
    if (out.empty()) return "_";
    if (out == "." || out == "..") return std::string(out.size(), '_');
    return out;
}

/* ---- information ---------------------------------------------------------------------------------------------- */

std::optional<FileInfo> fileInfo(const fs::path& p) {
    struct stat lst {};
    if (::lstat(p.c_str(), &lst) != 0) return std::nullopt;
    struct stat st = lst;
    const bool link = S_ISLNK(lst.st_mode);
    if (link && ::stat(p.c_str(), &st) != 0) st = lst;  // a dangling link: report the link itself
    FileInfo info;
    info.isDir = S_ISDIR(st.st_mode);
    info.isLink = link;
    info.size = info.isDir ? 0 : static_cast<std::uint64_t>(st.st_size);
    info.mtime = static_cast<std::int64_t>(st.st_mtimespec.tv_sec);
    info.writeStamp = static_cast<std::uint64_t>(st.st_mtimespec.tv_sec) * 1000000000ULL +
                      static_cast<std::uint64_t>(st.st_mtimespec.tv_nsec);
    return info;
}

bool isReadableNow(const fs::path& p) {
    const int fd = openFd(p, O_RDONLY);
    if (fd < 0) return false;
    closeFd(fd);
    return true;
}

/* ---- whole files ---------------------------------------------------------------------------------------------- */

void writeFileAtomic(const fs::path& path, const std::string& content) {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);

    static std::atomic<unsigned> counter{0};
    fs::path tmp = path;
    tmp += ".tmp-" + std::to_string(::getpid()) + "-" + std::to_string(++counter);

    // New files are private to the user; a replaced file keeps its permissions.
    mode_t mode = 0600;
    struct stat existing {};
    if (::stat(path.c_str(), &existing) == 0) mode = existing.st_mode & 07777;

    const int fd = openFd(tmp, O_WRONLY | O_CREAT | O_EXCL, mode);
    if (fd < 0) throw std::runtime_error("Cannot create " + fromPath(tmp) + ": " + lastErrorMessage());
    ::fchmod(fd, mode);  // not narrowed by the umask
    if (!writeFully(fd, content.data(), content.size())) {
        const std::string err = lastErrorMessage();
        closeFd(fd);
        ::unlink(tmp.c_str());
        throw std::runtime_error("Cannot write " + fromPath(path) + ": " + err);
    }
    if (!detail::syncToDisk(fd)) {
        const std::string err = lastErrorMessage();
        closeFd(fd);
        ::unlink(tmp.c_str());
        throw std::runtime_error("Cannot flush " + fromPath(path) + ": " + err);
    }
    if (::close(fd) != 0) {
        const std::string err = lastErrorMessage();
        ::unlink(tmp.c_str());
        throw std::runtime_error("Cannot close " + fromPath(path) + ": " + err);
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        const std::string err = lastErrorMessage();
        ::unlink(tmp.c_str());
        throw std::runtime_error("Cannot replace " + fromPath(path) + ": " + err);
    }
    // Make the new directory entry durable too (best effort).
    if (path.has_parent_path()) {
        const int dir = openFd(path.parent_path(), O_RDONLY);
        if (dir >= 0) {
            ::fsync(dir);
            closeFd(dir);
        }
    }
}

std::optional<std::string> readFile(const fs::path& path) {
    const int fd = openFd(path, O_RDONLY);
    if (fd < 0) return std::nullopt;
    struct stat st {};
    constexpr long long kMax = 512ll * 1024 * 1024;  // refuse absurdly large files
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > kMax) {
        closeFd(fd);
        return std::nullopt;
    }
    std::string out(static_cast<size_t>(st.st_size), '\0');
    size_t off = 0;
    while (off < out.size()) {
        const ssize_t got = ::read(fd, out.data() + off, out.size() - off);
        if (got < 0) {
            if (errno == EINTR) continue;
            closeFd(fd);
            return std::nullopt;
        }
        if (got == 0) break;  // file shrank while reading
        off += static_cast<size_t>(got);
    }
    closeFd(fd);
    out.resize(off);
    return out;
}

bool renameReplace(const fs::path& from, const fs::path& to, std::string& error) {
    if (::rename(from.c_str(), to.c_str()) == 0) return true;
    if (errno != EXDEV) {
        error = lastErrorMessage();
        return false;
    }
    // Another volume: copy, then remove the original (as MOVEFILE_COPY_ALLOWED does on Windows).
    std::error_code ec;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (!ec) fs::remove(from, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    return true;
}

bool removeFile(const fs::path& p, std::string& error) {
    if (::unlink(p.c_str()) == 0) return true;
    error = lastErrorMessage();
    return false;
}

std::FILE* openExclusiveWriter(const fs::path& p, bool append) {
    // Open without truncating, take the lock, and only then truncate: a refused second writer must not wipe the file.
    const int fd = openFd(p, O_WRONLY | O_CREAT | (append ? O_APPEND : 0), 0644);
    if (fd < 0) return nullptr;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        closeFd(fd);
        errno = EACCES;
        return nullptr;
    }
    if (!append && ::ftruncate(fd, 0) != 0) {
        const int err = errno;
        closeFd(fd);
        errno = err;
        return nullptr;
    }
    std::FILE* f = ::fdopen(fd, append ? "ab" : "wb");
    if (!f) {
        const int err = errno;
        closeFd(fd);
        errno = err;
    }
    return f;
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
    int flags = O_RDONLY;
    if (mode == Mode::Write) flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (mode == Mode::WriteKeep) flags = O_WRONLY | O_CREAT;
    const int fd = openFd(p, flags, 0666);
    if (fd < 0) {
        error_ = lastErrorMessage();
        return false;
    }
    handle_ = fd;
    return true;
}

bool File::seek(std::uint64_t offset) {
    if (::lseek(static_cast<int>(handle_), static_cast<off_t>(offset), SEEK_SET) >= 0) return true;
    error_ = lastErrorMessage();
    return false;
}

bool File::truncate() {
    const off_t pos = ::lseek(static_cast<int>(handle_), 0, SEEK_CUR);
    if (pos >= 0 && ::ftruncate(static_cast<int>(handle_), pos) == 0) return true;
    error_ = lastErrorMessage();
    return false;
}

std::int64_t File::read(void* buf, std::size_t n) {
    for (;;) {
        const ssize_t got = ::read(static_cast<int>(handle_), buf, n);
        if (got >= 0) return got;
        if (errno == EINTR) continue;
        error_ = lastErrorMessage();
        return -1;
    }
}

bool File::writeAll(const void* buf, std::size_t n) {
    if (writeFully(static_cast<int>(handle_), static_cast<const char*>(buf), n)) return true;
    error_ = lastErrorMessage();
    return false;
}

bool File::setTimes(std::int64_t unixSeconds) {
    const timespec times[2] = {{static_cast<time_t>(unixSeconds), 0}, {static_cast<time_t>(unixSeconds), 0}};
    if (::futimens(static_cast<int>(handle_), times) == 0) return true;
    error_ = lastErrorMessage();
    return false;
}

bool File::close() {
    if (handle_ == kInvalid) return true;
    const bool ok = ::close(static_cast<int>(handle_)) == 0;
    if (!ok) error_ = lastErrorMessage();
    handle_ = kInvalid;
    return ok;
}

}  // namespace st::sys
