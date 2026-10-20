#pragma once
// Internal building blocks of the SFTP service (not part of the public module API):
//  - error types + helpers turning libssh/SFTP status codes into readable messages
//  - runOnSsh(): run a functor on the SSH connection thread (SshTransport::post) and wait for its result
//  - RemoteFile: pipelined (sftp_aio_*) reader/writer whose steps are bounded in time so the connection
//    thread is never blocked for long
//  - small RAII wrappers for libssh handles used inside a single task
//
// Threading: runOnSsh() must be called from a background thread (pool / transfer worker), never from the
// SSH connection thread itself and never from the UI thread. Everything named "SSH thread only" may only be
// touched from functors executed by runOnSsh().
#include "core/Types.h"
#include "sftp/RemoteFs.h"
#include "ssh/SshTransport.h"
#include "transport/Transport.h"

#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace st::sftpimpl {

using Clock = std::chrono::steady_clock;

/** Bytes handled by one posted transfer step (contract: <= 256 KiB). */
constexpr size_t kStepBytes = 256 * 1024;
/** Size of one SFTP read/write request (capped further by the server's limits). */
constexpr size_t kChunkBytes = 64 * 1024;
/** Pipelined requests per transfer (kChunkBytes * kMaxInflight bytes in flight). */
constexpr size_t kMaxInflight = 16;
/** Time budget of a single SSH-thread step (metadata loops, waiting for pipelined replies). */
constexpr std::chrono::milliseconds kStepBudget{30};
/** Poll granularity while a step waits for replies. */
constexpr int kPollMs = 10;

/** "<what>: <reason>" from the SFTP status code / libssh error of `sftp`. */
std::string sftpErrorText(sftp_session sftp, const std::string& what);
[[noreturn]] void throwSftp(sftp_session sftp, const std::string& what);

struct AttrDeleter {
    void operator()(sftp_attributes a) const { sftp_attributes_free(a); }
};
using AttrPtr = std::unique_ptr<sftp_attributes_struct, AttrDeleter>;

struct SshCharDeleter {
    void operator()(char* p) const { ssh_string_free_char(p); }
};
using SshCharPtr = std::unique_ptr<char, SshCharDeleter>;

/** SftpEntry from libssh attributes (`dir` = listed directory, used to build entry.path). */
SftpEntry toEntry(const std::string& dir, sftp_attributes a);
/** Seconds since the Unix epoch (mtime) of attributes. */
int64_t attrMtime(sftp_attributes a);

/**
 * Runs fn(ssh_session) on the connection thread of `transport` and returns its result (exceptions propagate).
 * Throws ConnectionLost when the transport is gone or invokes the task with a null session.
 */
template <class F>
auto runOnSsh(const std::weak_ptr<Transport>& transport, F fn) -> std::invoke_result_t<F&, ssh_session> {
    using R = std::invoke_result_t<F&, ssh_session>;
    auto promise = std::make_shared<std::promise<R>>();
    auto future = promise->get_future();
    {
        std::shared_ptr<Transport> t = transport.lock();
        SshTransport* ssh = t ? t->asSsh() : nullptr;
        if (!ssh) throw ConnectionLost();
        ssh->post([promise, fn = std::move(fn)](ssh_session session) mutable {
            try {
                if (!session) throw ConnectionLost();
                if constexpr (std::is_void_v<R>) {
                    fn(session);
                    promise->set_value();
                } else {
                    promise->set_value(fn(session));
                }
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
        });
    }
    // The transport contract guarantees every task runs (with nullptr once the connection is gone); the
    // expiry check only guards against a transport that is destroyed without draining its queue.
    while (future.wait_for(std::chrono::milliseconds(250)) != std::future_status::ready) {
        if (transport.expired()) throw ConnectionLost();
    }
    try {
        return future.get();
    } catch (const std::future_error&) {
        throw ConnectionLost();  // task dropped without running (broken promise)
    }
}

/**
 * One open remote file with a pipeline of asynchronous requests. SSH thread only (except construction).
 * Download: open(O_RDONLY) -> readStep()* -> close(). Upload: open(O_WRONLY|O_CREAT|O_TRUNC) -> writeStep()* ->
 * flushStep()* -> close(). On error/cancel: drainStep()* -> close().
 */
class RemoteFile {
public:
    struct ReadResult {
        std::shared_ptr<std::string> data;
        bool done = false;
    };
    struct WriteResult {
        size_t pos = 0;      // new position inside the step buffer (== size: all queued)
        uint64_t acked = 0;  // bytes confirmed by the server during this step
    };

    RemoteFile() = default;
    RemoteFile(const RemoteFile&) = delete;
    RemoteFile& operator=(const RemoteFile&) = delete;
    ~RemoteFile();

    /** Opens for reading, fstat()s (size + mtime) and starts the read pipeline at `offset`. */
    void openRead(sftp_session sftp, const std::string& path, uint64_t offset = 0);
    /** Opens for writing (`mode` for new files): truncates, or with offset > 0 continues an existing file there. */
    void openWrite(sftp_session sftp, const std::string& path, uint32_t mode, uint64_t offset = 0);

    /** Collects up to kStepBytes of data (in file order). done = EOF reached and nothing in flight. */
    ReadResult readStep(sftp_session sftp);
    /** Queues buf[pos..] as pipelined writes (bounded by kMaxInflight and the step budget). */
    WriteResult writeStep(sftp_session sftp, const std::string& buf, size_t pos);
    /** Waits (bounded) for outstanding write acknowledgements. first = all acknowledged. */
    std::pair<bool, uint64_t> flushStep(sftp_session sftp);
    /** Discards outstanding requests (bounded). Returns true when nothing is left in flight. */
    bool drainStep(sftp_session sftp);
    /** Closes the handle (after the pipeline is empty); throws on failure when `check`. */
    void close(sftp_session sftp, bool check);
    /** Connection is gone: forget handles without touching libssh. */
    void abandon();

    bool isOpen() const { return file_ != nullptr; }
    uint64_t size() const { return size_; }
    int64_t mtime() const { return mtime_; }
    uint32_t mode() const { return mode_; }
    const std::string& path() const { return path_; }

private:
    struct Pending {
        sftp_aio aio = nullptr;
        uint64_t offset = 0;
        uint32_t len = 0;
    };
    void issueReads(sftp_session sftp);
    /** Waits up to kPollMs for SFTP channel data; throws when the channel is gone. */
    void pollChannel(sftp_session sftp);
    void resync(sftp_session sftp, uint64_t offset);

    sftp_file file_ = nullptr;
    std::string path_;
    bool writing_ = false;
    std::deque<Pending> inflight_;
    uint64_t next_ = 0;
    uint64_t size_ = 0;
    bool sizeKnown_ = false;
    bool eof_ = false;
    int64_t mtime_ = 0;
    uint32_t mode_ = 0;
    std::string scratch_;
};

}  // namespace st::sftpimpl
