#pragma once
// Protocol-neutral access to the files of a remote host (implemented over SFTP by SftpFs and over SCP + shell
// commands by ScpFs). Used by the transfer engine, find, synchronize and edit.
//
// Threading: every method blocks the calling thread and may be called from several background threads at once
// (transfer workers, bridge pool jobs). Implementations run their libssh work in bounded steps on the SSH connection
// thread (SshTransport::post), so a method must never be called from that thread or from the UI thread.
#include "core/Types.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace st::sftpimpl {

/** Operation failed (message is user-facing). */
struct SftpError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
/** The SSH connection is gone (or never was SSH). */
struct ConnectionLost : SftpError {
    ConnectionLost() : SftpError("The SSH connection is closed") {}
};
/** User cancelled. */
struct Cancelled : std::runtime_error {
    Cancelled() : std::runtime_error("Cancelled") {}
};

/** A directory entry plus the size of what a symlink points to (links: target size, 0 when dangling or a folder). */
struct RemoteEntry {
    SftpEntry entry;
    uint64_t targetSize = 0;
};

/** Called with progress (bytes) while a step completes; may throw Cancelled. */
using ProgressFn = std::function<void(uint64_t bytes)>;

/** Sequential reader of one remote file. */
class RemoteReader {
public:
    virtual ~RemoteReader() = default;
    /** Size when opened (0 = unknown / empty). */
    virtual uint64_t size() const = 0;
    /** Modification time, Unix seconds (0 = unknown). */
    virtual int64_t mtime() const = 0;
    /** Next chunk in file order (<= 256 KiB); done = end of file reached. */
    virtual std::shared_ptr<std::string> read(bool& done) = 0;
    /** Normal end; throws when the server reports an error. */
    virtual void close() = 0;
    /** Error / cancel path: best effort, never throws. */
    virtual void abort() noexcept = 0;
};

/** Sequential writer of one remote file. */
class RemoteWriter {
public:
    virtual ~RemoteWriter() = default;
    /** Queues data; progress gets bytes acknowledged by the server. */
    virtual void write(const std::shared_ptr<std::string>& data, const ProgressFn& progress) = 0;
    /** Waits for outstanding acknowledgements and closes; sets mtime when > 0 (best effort). */
    virtual void finish(int64_t mtime, const ProgressFn& progress) = 0;
    /** Error / cancel path: best effort, never throws. */
    virtual void abort() noexcept = 0;
};

struct WriteSpec {
    uint32_t mode = 0644;
    /** Resume: continue at this offset of an existing file (requires capabilities().resume). */
    uint64_t offset = 0;
    /** Bytes that will be written in total (SCP announces it up front). */
    uint64_t size = 0;
    /** Unix seconds to preserve (0 = don't). */
    int64_t mtime = 0;
};

class RemoteFs {
public:
    virtual ~RemoteFs() = default;

    /** "sftp" | "scp" */
    virtual std::string protocol() const = 0;
    virtual RemoteCapabilities capabilities() const = 0;
    /** True once the connection is known to be gone. */
    virtual bool lost() const = 0;

    /** Initial (home) folder. */
    virtual std::string home() = 0;
    virtual std::string realpath(const std::string& path) = 0;
    /** Folder entries without "." / "..". statLinks resolves symlink targets (linkTargetIsDir, targetSize). */
    virtual std::vector<RemoteEntry> list(const std::string& path, bool statLinks,
                                          const std::atomic<bool>* cancel = nullptr) = 0;
    /** nullopt when nothing exists at path; follow = stat (true) or lstat (false). */
    virtual std::optional<SftpEntry> stat(const std::string& path, bool follow) = 0;
    virtual void mkdir(const std::string& path, uint32_t mode = 0755) = 0;
    virtual void rename(const std::string& from, const std::string& to) = 0;
    /** Removes a file or a symlink. */
    virtual void unlink(const std::string& path) = 0;
    /** Removes an empty folder. */
    virtual void rmdir(const std::string& path) = 0;
    /** Recursive delete; symlinks are removed, never followed. Default: list + unlink + rmdir. */
    virtual void removeTree(const std::string& path, const std::atomic<bool>* cancel = nullptr);
    virtual void chmod(const std::string& path, uint32_t mode) = 0;
    /** Best effort. */
    virtual void setMtime(const std::string& path, int64_t mtime) = 0;

    /** offset > 0 resumes (requires capabilities().resume). */
    virtual std::unique_ptr<RemoteReader> openRead(const std::string& path, uint64_t offset = 0) = 0;
    virtual std::unique_ptr<RemoteWriter> openWrite(const std::string& path, const WriteSpec& spec) = 0;

    /** Releases protocol resources (blocking). Further calls fail. */
    virtual void close() = 0;
};

/** "'path'" for messages. */
inline std::string quotePath(const std::string& path) { return "'" + path + "'"; }

}  // namespace st::sftpimpl
