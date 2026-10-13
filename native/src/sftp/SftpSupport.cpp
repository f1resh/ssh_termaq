#include "sftp/SftpSupport.h"

#include "sftp/SftpUtil.h"

#include <fcntl.h>

#include <algorithm>

namespace sc::sftpimpl {

namespace {

// 100 ns intervals between 1601-01-01 and 1970-01-01.
constexpr int64_t kEpochDiff100ns = 116444736000000000LL;

}  // namespace

std::string sftpErrorText(sftp_session sftp, const std::string& what) {
    if (!sftp) return what + ": SFTP session is closed";
    const int code = sftp_get_error(sftp);
    std::string reason;
    if (code != SSH_FX_OK && code != SSH_FX_FAILURE && code != SSH_FX_BAD_MESSAGE) {
        reason = sftputil::statusText(code);
    } else {
        // Generic failure: the libssh error string usually carries the server's message ("SFTP server: ...").
        const char* e = sftp->session ? ssh_get_error(sftp->session) : nullptr;
        if (e && *e) {
            reason = e;
            const std::string prefix = "SFTP server : ";
            if (reason.rfind(prefix, 0) == 0) reason.erase(0, prefix.size());
        }
        if (reason.empty() || reason == "Success") reason = sftputil::statusText(code == SSH_FX_OK ? SSH_FX_FAILURE : code);
    }
    return what + ": " + reason;
}

void throwSftp(sftp_session sftp, const std::string& what) {
    throw SftpError(sftpErrorText(sftp, what));
}

int64_t attrMtime(sftp_attributes a) {
    if (!a) return 0;
    return a->mtime64 ? static_cast<int64_t>(a->mtime64) : static_cast<int64_t>(a->mtime);
}

SftpEntry toEntry(const std::string& dir, sftp_attributes a) {
    SftpEntry e;
    e.name = a->name ? a->name : "";
    e.path = sftputil::joinRemote(dir, e.name);
    e.mode = a->permissions;
    e.type = sftputil::entryType(a->type, a->permissions);
    e.size = static_cast<double>(a->size);
    e.mtime = attrMtime(a) * 1000;
    e.permissions = sftputil::permissionsString(a->permissions);
    e.uid = a->uid;
    e.gid = a->gid;
    e.linkTargetIsDir = false;
    return e;
}

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

/* ------------------------------------------------------------------------ */
/* RemoteFile                                                                */
/* ------------------------------------------------------------------------ */

RemoteFile::~RemoteFile() {
    // Normally closed on the SSH thread. Here we may be on any thread, so only release plain memory.
    abandon();
}

void RemoteFile::abandon() {
    for (auto& p : inflight_) sftp_aio_free(p.aio);
    inflight_.clear();
    file_ = nullptr;  // libssh handle intentionally not touched (its session may already be gone)
}

void RemoteFile::openRead(sftp_session sftp, const std::string& path) {
    path_ = path;
    writing_ = false;
    file_ = sftp_open(sftp, path.c_str(), O_RDONLY, 0);
    if (!file_) throwSftp(sftp, "Cannot open '" + path + "'");
    if (AttrPtr a{sftp_fstat(file_)}) {
        size_ = a->size;
        mtime_ = attrMtime(a.get());
        mode_ = a->permissions;
    }
    // Size 0 is treated as unknown (pseudo files report 0): read until EOF instead.
    sizeKnown_ = size_ > 0;
    sftp_file_set_nonblocking(file_);
    issueReads(sftp);
}

void RemoteFile::openWrite(sftp_session sftp, const std::string& path, uint32_t mode) {
    path_ = path;
    writing_ = true;
    file_ = sftp_open(sftp, path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, static_cast<mode_t>(mode));
    if (!file_) throwSftp(sftp, "Cannot create '" + path + "'");
    sftp_file_set_nonblocking(file_);
}

void RemoteFile::issueReads(sftp_session sftp) {
    while (!eof_ && inflight_.size() < kMaxInflight && (!sizeKnown_ || next_ < size_)) {
        const uint64_t want = sizeKnown_ ? std::min<uint64_t>(kChunkBytes, size_ - next_) : kChunkBytes;
        sftp_aio aio = nullptr;
        const ssize_t n = sftp_aio_begin_read(file_, static_cast<size_t>(want), &aio);
        if (n <= 0) throwSftp(sftp, "Read request failed for '" + path_ + "'");
        inflight_.push_back(Pending{aio, next_, static_cast<uint32_t>(n)});
        next_ += static_cast<uint64_t>(n);
    }
}

void RemoteFile::pollChannel(sftp_session sftp) {
    const int rc = ssh_channel_poll_timeout(sftp->channel, kPollMs, 0);
    if (rc == SSH_ERROR || rc == SSH_EOF) throw SftpError("The SFTP channel was closed");
}

void RemoteFile::resync(sftp_session sftp, uint64_t offset) {
    // A short read before EOF leaves a gap behind the pipelined requests: discard them and restart at `offset`.
    while (!drainStep(sftp)) {
    }
    if (sftp_seek64(file_, offset) < 0) throwSftp(sftp, "Seek failed for '" + path_ + "'");
    next_ = offset;
}

RemoteFile::ReadResult RemoteFile::readStep(sftp_session sftp) {
    ReadResult r;
    r.data = std::make_shared<std::string>();
    std::string& out = *r.data;
    out.reserve(kStepBytes + kChunkBytes);
    const auto start = Clock::now();
    while (!inflight_.empty() && out.size() < kStepBytes) {
        Pending& req = inflight_.front();
        const size_t old = out.size();
        out.resize(old + req.len);
        const ssize_t n = sftp_aio_wait_read(&req.aio, out.data() + old, req.len);
        if (n == SSH_AGAIN) {
            out.resize(old);
            if (!out.empty() || Clock::now() - start >= kStepBudget) break;
            pollChannel(sftp);
            continue;
        }
        const Pending done = req;
        inflight_.pop_front();  // the aio handle was released by sftp_aio_wait_read
        if (n < 0) {
            out.resize(old);
            throwSftp(sftp, "Read failed for '" + path_ + "'");
        }
        out.resize(old + static_cast<size_t>(n));
        if (n == 0) {
            eof_ = true;
        } else if (static_cast<uint32_t>(n) < done.len) {
            resync(sftp, done.offset + static_cast<uint64_t>(n));
        }
        if (!eof_) issueReads(sftp);
    }
    if (!eof_) issueReads(sftp);
    r.done = inflight_.empty();
    return r;
}

RemoteFile::WriteResult RemoteFile::writeStep(sftp_session sftp, const std::string& buf, size_t pos) {
    WriteResult r;
    const auto start = Clock::now();
    for (;;) {
        // Reap acknowledgements that already arrived (non-blocking handle => SSH_AGAIN when pending).
        while (!inflight_.empty()) {
            const ssize_t n = sftp_aio_wait_write(&inflight_.front().aio);
            if (n == SSH_AGAIN) break;
            inflight_.pop_front();
            if (n < 0) throwSftp(sftp, "Write failed for '" + path_ + "'");
            r.acked += static_cast<uint64_t>(n);
        }
        if (pos < buf.size() && inflight_.size() < kMaxInflight) {
            const size_t want = std::min(kChunkBytes, buf.size() - pos);
            sftp_aio aio = nullptr;
            const ssize_t n = sftp_aio_begin_write(file_, buf.data() + pos, want, &aio);
            if (n <= 0) throwSftp(sftp, "Write request failed for '" + path_ + "'");
            inflight_.push_back(Pending{aio, next_, static_cast<uint32_t>(n)});
            next_ += static_cast<uint64_t>(n);
            pos += static_cast<size_t>(n);
            continue;
        }
        if (pos >= buf.size() || Clock::now() - start >= kStepBudget) break;
        pollChannel(sftp);  // pipeline full: wait a little for acknowledgements
    }
    r.pos = pos;
    return r;
}

std::pair<bool, uint64_t> RemoteFile::flushStep(sftp_session sftp) {
    uint64_t acked = 0;
    const auto start = Clock::now();
    while (!inflight_.empty()) {
        const ssize_t n = sftp_aio_wait_write(&inflight_.front().aio);
        if (n == SSH_AGAIN) {
            if (Clock::now() - start >= kStepBudget) break;
            pollChannel(sftp);
            continue;
        }
        inflight_.pop_front();
        if (n < 0) throwSftp(sftp, "Write failed for '" + path_ + "'");
        acked += static_cast<uint64_t>(n);
    }
    return {inflight_.empty(), acked};
}

bool RemoteFile::drainStep(sftp_session sftp) {
    const auto start = Clock::now();
    while (!inflight_.empty()) {
        Pending& req = inflight_.front();
        ssize_t n;
        if (writing_) {
            n = sftp_aio_wait_write(&req.aio);
        } else {
            if (scratch_.size() < req.len) scratch_.resize(req.len);
            n = sftp_aio_wait_read(&req.aio, scratch_.data(), req.len);
        }
        if (n == SSH_AGAIN) {
            if (Clock::now() - start >= kStepBudget) return false;
            const int rc = ssh_channel_poll_timeout(sftp->channel, kPollMs, 0);
            if (rc == SSH_ERROR || rc == SSH_EOF) {
                // Channel gone: replies will never arrive.
                for (auto& p : inflight_) sftp_aio_free(p.aio);
                inflight_.clear();
                return true;
            }
            continue;
        }
        inflight_.pop_front();  // errors are irrelevant while discarding
    }
    return true;
}

void RemoteFile::close(sftp_session sftp, bool check) {
    if (!file_) return;
    for (auto& p : inflight_) sftp_aio_free(p.aio);  // only non-empty on error paths
    inflight_.clear();
    sftp_file f = file_;
    file_ = nullptr;
    if (sftp_close(f) != SSH_NO_ERROR && check) throwSftp(sftp, "Closing '" + path_ + "' failed");
}

}  // namespace sc::sftpimpl
