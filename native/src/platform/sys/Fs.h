#pragma once
// Files and paths. Paths travel through the app (bridge, settings, sessions) as UTF-8 strings; these convert them to
// and from std::filesystem::path, whose native form is UTF-16 on Windows and bytes on POSIX. The rest covers what
// std::filesystem does not: atomic replacement, share modes, file times, open files, OS naming rules.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace st::sys {

/* ---- paths as UTF-8 text ------------------------------------------------------------------------------------ */

/** UTF-8 text -> path. Invalid UTF-8 is replaced (U+FFFD) on Windows and kept as bytes on POSIX. */
std::filesystem::path toPath(std::string_view utf8);
/** Path -> UTF-8 text with the native separators ('\' on Windows). */
std::string fromPath(const std::filesystem::path& p);
/** Path -> UTF-8 text with '/' separators. */
std::string fromGenericPath(const std::filesystem::path& p);

/**
 * A file name (one path component, e.g. a remote file's name) made valid for this OS, UTF-8. Windows: characters
 * NTFS rejects (<>:"/\|?* and controls) become '_', trailing dots/spaces are trimmed, reserved device names (CON,
 * NUL, COM1, ...) get a leading '_'; never empty.
 */
std::string sanitizeFileName(std::string_view utf8Name);

/* ---- information ---------------------------------------------------------------------------------------------- */

struct FileInfo {
    bool isDir = false;            // a folder (on Windows also a link or junction to one)
    bool isLink = false;           // a symbolic link, junction or other reparse point
    std::uint64_t size = 0;        // bytes (files)
    std::int64_t mtime = 0;        // last write, Unix seconds
    std::uint64_t writeStamp = 0;  // last write at the file system's full precision; only compare for equality
};
/**
 * Size, times and kind of `p`; nullopt when it does not exist or cannot be read (lastErrorMessage() says why).
 * Windows reports a symbolic link itself (a link to a file has size 0); POSIX reports what the link points to.
 */
std::optional<FileInfo> fileInfo(const std::filesystem::path& p);

/** False while another program holds `p` open without allowing others to read it (e.g. an editor still saving). */
bool isReadableNow(const std::filesystem::path& p);

/* ---- whole files ---------------------------------------------------------------------------------------------- */

/**
 * Writes `content` to `path` atomically: a temporary file next to it, flushed to the disk, then renamed over `path`
 * (Windows retries for a moment while antivirus scanners or the indexer hold the target). Creates missing parent
 * folders. Throws std::runtime_error on failure.
 */
void writeFileAtomic(const std::filesystem::path& path, const std::string& content);
/** Whole file; nullopt if it is missing, unreadable or larger than 512 MB. Other programs may keep using it. */
std::optional<std::string> readFile(const std::filesystem::path& path);
/** Renames `from` to `to`, replacing `to`; false with `error` set on failure. */
bool renameReplace(const std::filesystem::path& from, const std::filesystem::path& to, std::string& error);
/** Deletes a file; false with `error` set on failure. */
bool removeFile(const std::filesystem::path& p, std::string& error);
/**
 * fopen(p, append ? "ab" : "wb") that lets other programs read the file but no one else write it while it is open
 * (a session log). nullptr with errno set on failure (EACCES when another writer has it).
 */
std::FILE* openExclusiveWriter(const std::filesystem::path& p, bool append);

/* ---- open files ----------------------------------------------------------------------------------------------- */

/** An open file for sequential transfers. Move-only; closes on destruction. Errors: false / -1 and error(). */
class File {
public:
    enum class Mode {
        Read,       // an existing file; other programs may keep reading, writing, renaming or deleting it
        Write,      // created or truncated; other programs may read it meanwhile
        WriteKeep,  // created if missing, kept otherwise (to resume it); other programs may read it meanwhile
    };

    File() = default;
    ~File();
    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    bool open(const std::filesystem::path& p, Mode mode);
    bool isOpen() const { return handle_ != kInvalid; }
    /** Moves to `offset` bytes from the start. */
    bool seek(std::uint64_t offset);
    /** Cuts the file off at the current position. */
    bool truncate();
    /** Reads up to `n` bytes: the count, 0 at the end of the file, -1 on error. */
    std::int64_t read(void* buf, std::size_t n);
    /** Writes all `n` bytes. */
    bool writeAll(const void* buf, std::size_t n);
    /** Sets the last access and last write times (Unix seconds). */
    bool setTimes(std::int64_t unixSeconds);
    /** Closes now; false if the OS reported an error. */
    bool close();
    /** Why the last call failed. */
    const std::string& error() const { return error_; }

private:
    static constexpr std::intptr_t kInvalid = -1;
    std::intptr_t handle_ = kInvalid;  // HANDLE on Windows, file descriptor on POSIX
    std::string error_;
};

/** Whether names in `dir` (or the nearest folder above it that exists) differ by letter case: macOS per volume
 *  (APFS / HFS+ usually not), Windows not. */
bool caseSensitiveFolder(const std::filesystem::path& dir);

/* ---- downloaded files ----------------------------------------------------------------------------------------- */

/** File types that run code when opened from the file manager (Windows: .exe, .bat, .ps1, .lnk, ...). */
bool isExecutableType(const std::filesystem::path& file);
/**
 * Marks a file as downloaded from the Internet so the OS treats it with caution (Windows: Zone.Identifier stream
 * with ZoneId=3, as browsers do). Best effort.
 */
void markDownloaded(const std::filesystem::path& file);

}  // namespace st::sys
