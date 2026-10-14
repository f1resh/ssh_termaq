#include "sftp/ScpFs.h"

#include "core/Strings.h"
#include "sftp/ScpProtocol.h"
#include "sftp/SftpSupport.h"
#include "sftp/SftpUtil.h"

#include <chrono>
#include <cstring>
#include <ctime>

namespace sc::sftpimpl {

namespace {

constexpr size_t kMaxStderr = 64 * 1024;
constexpr size_t kMaxControlLine = 4096;
constexpr size_t kMaxScriptOutput = 64 * 1024 * 1024;
const char* const kRcMarker = "__SC_RC=";
const char* const kMissing = "__SC_MISSING__";

std::string q(const std::string& s) { return scp::shellQuote(s); }

std::string firstLine(std::string s) {
    s = trim(s);
    const size_t nl = s.find('\n');
    if (nl != std::string::npos) s.resize(nl);
    return trim(s);
}

/**
 * One remote command on its own exec channel. Every libssh call runs on the connection thread in bounded steps, so
 * the terminal sharing the connection stays responsive.
 */
class ExecChannel {
public:
    explicit ExecChannel(std::weak_ptr<Transport> transport) : transport_(std::move(transport)), st_(std::make_shared<State>()) {}
    ~ExecChannel() { close(); }
    ExecChannel(const ExecChannel&) = delete;
    ExecChannel& operator=(const ExecChannel&) = delete;

    void start(const std::string& command) {
        call([st = st_, command](ssh_session session) {
            st->ch = ssh_channel_new(session);
            if (!st->ch) throw SftpError(std::string("Cannot open a channel: ") + ssh_get_error(session));
            if (ssh_channel_open_session(st->ch) != SSH_OK) {
                std::string msg = std::string("Cannot open a channel: ") + ssh_get_error(session);
                ssh_channel_free(st->ch);
                st->ch = nullptr;
                throw SftpError(msg);
            }
            if (ssh_channel_request_exec(st->ch, command.c_str()) != SSH_OK) {
                std::string msg = std::string("The server refused to run a command: ") + ssh_get_error(session);
                ssh_channel_free(st->ch);
                st->ch = nullptr;
                throw SftpError(msg);
            }
        });
    }

    /** Appends available stdout to buf (waits up to one step). Returns false once the remote side sent EOF. */
    bool readSome(std::string& buf) {
        auto chunk = std::make_shared<std::string>();
        const bool open = call([st = st_, chunk](ssh_session) {
            if (!st->ch) return false;
            const auto start = Clock::now();
            char tmp[32768];
            for (;;) {
                const int n = ssh_channel_read_nonblocking(st->ch, tmp, sizeof tmp, 0);
                if (n == SSH_ERROR) throw SftpError("The channel failed while reading");
                if (n > 0) {
                    chunk->append(tmp, static_cast<size_t>(n));
                    if (chunk->size() >= kStepBytes) return true;
                    continue;
                }
                drainStderr(*st);
                if (ssh_channel_is_eof(st->ch) || ssh_channel_is_closed(st->ch)) return false;
                if (!chunk->empty() || Clock::now() - start >= kStepBudget) return true;
                const int rc = ssh_channel_poll_timeout(st->ch, kPollMs, 0);
                if (rc == SSH_ERROR) throw SftpError("The channel failed while reading");
            }
        });
        buf += *chunk;
        return open || !chunk->empty();
    }

    /** Sends all of data (window-limited steps); progress(n) after each step. */
    void write(const std::shared_ptr<const std::string>& data, const ProgressFn& progress = {}) {
        size_t pos = 0;
        while (pos < data->size()) {
            const size_t next = call([st = st_, data, pos](ssh_session) {
                if (!st->ch) throw SftpError("The channel is closed");
                const auto start = Clock::now();
                size_t p = pos;
                while (p < data->size()) {
                    if (ssh_channel_is_closed(st->ch) || ssh_channel_is_eof(st->ch)) {
                        drainStderr(*st);
                        const std::string why = firstLine(st->err);
                        throw SftpError("The server closed the channel" + (why.empty() ? std::string() : ": " + why));
                    }
                    const uint32_t window = ssh_channel_window_size(st->ch);
                    if (window == 0) {
                        if (Clock::now() - start >= kStepBudget) break;
                        if (ssh_channel_poll_timeout(st->ch, kPollMs, 0) == SSH_ERROR) {
                            throw SftpError("The channel failed while writing");
                        }
                        continue;
                    }
                    const uint32_t len = static_cast<uint32_t>(std::min<size_t>({data->size() - p, window, 32768}));
                    const int n = ssh_channel_write(st->ch, data->data() + p, len);
                    if (n == SSH_ERROR) throw SftpError("The channel failed while writing");
                    p += static_cast<size_t>(n);
                    if (Clock::now() - start >= kStepBudget) break;
                }
                return p;
            });
            if (progress) progress(next - pos);
            pos = next;
        }
    }

    void write(const std::string& data) { write(std::make_shared<const std::string>(data)); }

    void sendEof() {
        call([st = st_](ssh_session) {
            if (st->ch && !ssh_channel_is_closed(st->ch)) ssh_channel_send_eof(st->ch);
        });
    }

    /** Reads stdout until the remote side sends EOF (appending to out, at most maxOut bytes kept). */
    void readToEnd(std::string& out, size_t maxOut) {
        std::string buf;
        for (;;) {
            const bool more = readSome(buf);
            if (out.size() < maxOut) out.append(buf, 0, std::min(buf.size(), maxOut - out.size()));
            buf.clear();
            if (!more) break;
        }
    }

    std::string stderrText() const { return st_->err; }

    /** Closes and frees the channel (best effort, never throws). */
    void close() noexcept {
        if (!st_->ch || lost_) return;
        try {
            call([st = st_](ssh_session) {
                if (!st->ch) return;
                if (!ssh_channel_is_closed(st->ch)) ssh_channel_close(st->ch);
                ssh_channel_free(st->ch);
                st->ch = nullptr;
            });
        } catch (...) {
        }
    }

    bool lost() const { return lost_; }

private:
    struct State {
        ssh_channel ch = nullptr;  // SSH thread only
        std::string err;
    };

    static void drainStderr(State& st) {
        char tmp[4096];
        for (;;) {
            const int n = ssh_channel_read_nonblocking(st.ch, tmp, sizeof tmp, 1);
            if (n <= 0) break;
            if (st.err.size() < kMaxStderr) st.err.append(tmp, static_cast<size_t>(n));
        }
    }

    template <class F>
    auto call(F fn) -> std::invoke_result_t<F&, ssh_session> {
        try {
            return runOnSsh(transport_, std::move(fn));
        } catch (const ConnectionLost&) {
            lost_ = true;  // the channel died with the session: never touch it again
            throw;
        }
    }

    std::weak_ptr<Transport> transport_;
    std::shared_ptr<State> st_;
    bool lost_ = false;
};

/** Buffered reader for the scp protocol's control lines, status bytes and data. */
class ScpStream {
public:
    explicit ScpStream(ExecChannel& ch) : ch_(ch) {}

    /** Ensures at least one byte is buffered; throws when the stream ended. */
    void fill() {
        while (pos_ >= buf_.size()) {
            buf_.clear();
            pos_ = 0;
            if (!ch_.readSome(buf_) && buf_.empty()) {
                const std::string why = firstLine(ch_.stderrText());
                throw SftpError("SCP ended unexpectedly" + (why.empty() ? std::string() : ": " + why));
            }
        }
    }

    char byte() {
        fill();
        return buf_[pos_++];
    }

    std::string line() {
        std::string out;
        for (;;) {
            const char c = byte();
            if (c == '\n') return out;
            out += c;
            if (out.size() > kMaxControlLine) throw SftpError("SCP sent an overlong line");
        }
    }

    /** Status byte: 0 = OK, 1 / 2 = warning / error followed by a message line. */
    void expectOk(const std::string& what) {
        const char c = byte();
        if (c == 0) return;
        if (c == 1 || c == 2) throw SftpError(what + ": " + trim(line()));
        throw SftpError(what + ": unexpected SCP reply");
    }

    /** Up to max buffered / newly read bytes. */
    std::string take(size_t max) {
        fill();
        const size_t n = std::min(max, buf_.size() - pos_);
        std::string out = buf_.substr(pos_, n);
        pos_ += n;
        return out;
    }

private:
    ExecChannel& ch_;
    std::string buf_;
    size_t pos_ = 0;
};

/* ------------------------------------------------------------------------ */
/* ScpFs                                                                     */
/* ------------------------------------------------------------------------ */

class ScpFsImpl final : public RemoteFs, public std::enable_shared_from_this<ScpFsImpl> {
public:
    explicit ScpFsImpl(std::weak_ptr<Transport> transport) : transport_(std::move(transport)) {}

    struct Result {
        int rc = -1;
        std::string out;
        std::string err;
    };

    /** Runs a POSIX sh script (through `sh -c`, whatever the login shell is) and returns its output and status. */
    Result runScript(const std::string& script) {
        if (lost_) throw ConnectionLost();
        if (closed_) throw SftpError("File session is closed");
        ExecChannel ch(transport_);
        try {
            // One line: csh / tcsh login shells reject newlines inside the quoted script.
            ch.start("sh -c " + q(script + "; echo \"" + kRcMarker + "$?\""));
            ch.sendEof();
            Result r;
            ch.readToEnd(r.out, kMaxScriptOutput);
            r.err = ch.stderrText();
            ch.close();
            const size_t at = r.out.rfind(kRcMarker);
            if (at != std::string::npos) {
                r.rc = std::atoi(r.out.c_str() + at + std::strlen(kRcMarker));
                r.out.resize(at);
            }
            return r;
        } catch (const ConnectionLost&) {
            lost_ = true;
            throw;
        }
    }

    /** Runs a script that must succeed; the error names `what` plus the first stderr line. */
    std::string check(const std::string& script, const std::string& what) {
        Result r = runScript(script);
        if (r.rc != 0) {
            std::string why = firstLine(r.err);
            if (why.empty()) why = firstLine(r.out);
            if (why.empty()) why = "the command failed";
            // "rm: cannot remove '/x': Permission denied" -> keep only the reason
            const size_t colon = why.rfind(": ");
            if (colon != std::string::npos && colon + 2 < why.size()) why = why.substr(colon + 2);
            throw SftpError(what + ": " + why);
        }
        return r.out;
    }

    void init() {
        const std::string probe =
            "export LC_ALL=C; "
            "if command -v scp >/dev/null 2>&1; then echo scp=yes; else echo scp=no; fi; "
            "echo \"tz=$(date +%z)\"; "
            "echo \"now=$(date +%s)\"; "
            "if ls -la --full-time -d / >/dev/null 2>&1; then echo 'ls=--full-time'; "
            "elif ls -laT -d / >/dev/null 2>&1; then echo 'ls=-T'; "
            "elif ls -lae -d / >/dev/null 2>&1; then echo 'ls=-e'; "
            "else echo 'ls='; fi; "
            "echo \"home=$(pwd)\"";
        Result r = runScript(probe);
        if (r.rc != 0 && r.out.find("scp=") == std::string::npos) {
            const std::string why = firstLine(r.err);
            throw SftpError("The server cannot run shell commands" + (why.empty() ? std::string() : ": " + why));
        }
        bool haveScp = false;
        for (const auto& raw : split(r.out, '\n')) {
            const std::string line = trim(raw);
            if (line == "scp=yes") haveScp = true;
            else if (startsWith(line, "tz=")) tz_ = scp::parseTzOffset(line.substr(3));
            else if (startsWith(line, "ls=")) lsFlags_ = line.substr(3);
            else if (startsWith(line, "home=")) home_ = line.substr(5);
        }
        if (!haveScp) throw SftpError("The server has no scp command");
        if (home_.empty() || home_[0] != '/') home_ = "/";
        preciseTimes_ = !lsFlags_.empty();
    }

    std::string lsCommand(const std::string& extra) const {
        return "ls -la" + extra + (lsFlags_.empty() ? std::string() : " " + lsFlags_);
    }

    SftpEntry toEntry(const std::string& dir, const scp::LsEntry& e) const {
        SftpEntry out;
        out.name = e.name;
        out.path = sftputil::joinRemote(dir, e.name);
        out.type = e.type == 'd' ? "dir" : e.type == 'l' ? "link" : e.type == '-' ? "file" : "other";
        out.size = static_cast<double>(e.size);
        out.mtime = e.mtime * 1000;
        out.mode = e.mode;
        out.permissions = sftputil::permissionsString(e.mode);
        return out;
    }

    static int64_t nowUtc() { return static_cast<int64_t>(std::time(nullptr)); }

    /* ---- RemoteFs */

    std::string protocol() const override { return "scp"; }
    RemoteCapabilities capabilities() const override { return RemoteCapabilities{false, preciseTimes_}; }
    bool lost() const override { return lost_; }
    std::string home() override { return home_; }

    std::string realpath(const std::string& path) override {
        const std::string p = path.empty() || path == "." ? home_ : path;
        const std::string out = check("cd " + q(p) + " && pwd", "Cannot open folder " + quotePath(p));
        const std::string resolved = trim(out);
        return resolved.empty() ? p : resolved;
    }

    std::vector<RemoteEntry> list(const std::string& rawPath, bool statLinks, const std::atomic<bool>* cancel) override {
        const std::string dir = rawPath.empty() || rawPath == "." ? home_ : rawPath;
        const std::string out =
            check("export LC_ALL=C; cd " + q(dir) + " && " + lsCommand("") + " .", "Cannot list folder " + quotePath(dir));
        std::vector<RemoteEntry> entries;
        std::vector<size_t> links;
        const int64_t now = nowUtc();
        for (const auto& line : split(out, '\n')) {
            auto e = scp::parseLsLine(line, tz_, now);
            if (!e || e->name == "." || e->name == "..") continue;
            RemoteEntry re{toEntry(dir, *e), e->size};
            if (e->type == 'l') {
                re.targetSize = 0;
                links.push_back(entries.size());
            }
            entries.push_back(std::move(re));
        }
        if (statLinks && !links.empty()) resolveLinks(dir, entries, links, cancel);
        return entries;
    }

    /** Batched `ls -ladL` for symlinks: target type (folder?) and size. */
    void resolveLinks(const std::string& dir, std::vector<RemoteEntry>& entries, const std::vector<size_t>& links,
                      const std::atomic<bool>* cancel) {
        constexpr size_t kBatch = 200;
        const int64_t now = nowUtc();
        for (size_t start = 0; start < links.size(); start += kBatch) {
            if (cancel && *cancel) throw Cancelled();
            const size_t end = std::min(links.size(), start + kBatch);
            std::string names;
            for (size_t i = start; i < end; ++i) names += " " + q(entries[links[i]].entry.name);
            const std::string script = "export LC_ALL=C; cd " + q(dir) + " || exit 1; for f in" + names + "; do " +
                                       lsCommand("dL") + " -- \"$f\" 2>/dev/null || echo '?'; done";
            Result r = runScript(script);
            const auto lines = split(r.out, '\n', true);
            for (size_t i = start, li = 0; i < end && li < lines.size(); ++i, ++li) {
                auto& re = entries[links[i]];
                auto e = scp::parseLsLine(lines[li], tz_, now);
                if (!e) continue;  // dangling
                re.entry.linkTargetIsDir = e->type == 'd';
                re.targetSize = e->type == '-' ? e->size : 0;
            }
        }
    }

    std::optional<SftpEntry> stat(const std::string& path, bool follow) override {
        const std::string exists = follow ? "[ -e " + q(path) + " ]" : "[ -e " + q(path) + " ] || [ -L " + q(path) + " ]";
        const std::string out = check("export LC_ALL=C; if " + exists + "; then " + lsCommand(follow ? "dL" : "d") + " -- " +
                                          q(path) + "; else echo " + kMissing + "; fi",
                                      "Cannot access " + quotePath(path));
        if (out.find(kMissing) != std::string::npos) return std::nullopt;
        for (const auto& line : split(out, '\n')) {
            if (auto e = scp::parseLsLine(line, tz_, nowUtc())) {
                SftpEntry s = toEntry(sftputil::remoteParent(path), *e);
                s.name = sftputil::remoteBaseName(path);
                s.path = path;
                return s;
            }
        }
        throw SftpError("Cannot access " + quotePath(path) + ": unexpected `ls` output");
    }

    void mkdir(const std::string& path, uint32_t mode) override {
        char m[8];
        std::snprintf(m, sizeof m, "%o", mode & 07777);
        check(std::string("mkdir ") + (mode == 0755 ? "" : std::string("-m ") + m + " ") + "-- " + q(path),
              "Cannot create folder " + quotePath(path));
    }

    void rename(const std::string& from, const std::string& to) override {
        check("if [ -e " + q(to) + " ] || [ -L " + q(to) + " ]; then echo 'mv: target: File exists' >&2; false; else mv -- " +
                  q(from) + " " + q(to) + "; fi",
              "Cannot rename " + quotePath(from) + " to " + quotePath(to));
    }

    void unlink(const std::string& path) override {
        check("if [ -d " + q(path) + " ] && [ ! -L " + q(path) + " ]; then echo 'rm: Is a directory' >&2; false; else rm -f -- " +
                  q(path) + "; fi",
              "Cannot delete " + quotePath(path));
    }

    void rmdir(const std::string& path) override {
        check("rmdir -- " + q(path), "Cannot delete folder " + quotePath(path));
    }

    void removeTree(const std::string& path, const std::atomic<bool>*) override {
        if (path.empty() || path == "/") throw SftpError("Refusing to delete " + quotePath(path));
        check("rm -rf -- " + q(path), "Cannot delete " + quotePath(path));
    }

    void chmod(const std::string& path, uint32_t mode) override {
        char m[8];
        std::snprintf(m, sizeof m, "%o", mode & 07777);
        check(std::string("chmod ") + m + " -- " + q(path), "Cannot change permissions of " + quotePath(path));
    }

    void setMtime(const std::string& path, int64_t mtime) override {
        if (mtime <= 0) return;
        // touch -t takes the server's local time.
        const int64_t local = mtime + tz_;
        const std::time_t lt = static_cast<std::time_t>(local);
        std::tm tm{};
        gmtime_s(&tm, &lt);
        char stamp[32];
        std::strftime(stamp, sizeof stamp, "%Y%m%d%H%M.%S", &tm);
        runScript("touch -m -d @" + std::to_string(mtime) + " -- " + q(path) + " 2>/dev/null || touch -m -t " + stamp +
                  " -- " + q(path));
    }

    std::unique_ptr<RemoteReader> openRead(const std::string& path, uint64_t offset) override;
    std::unique_ptr<RemoteWriter> openWrite(const std::string& path, const WriteSpec& spec) override;

    void close() override { closed_ = true; }

    std::weak_ptr<Transport> transport() const { return transport_; }
    void markLost() { lost_ = true; }

private:
    std::weak_ptr<Transport> transport_;
    std::atomic<bool> lost_{false};
    std::atomic<bool> closed_{false};
    std::string home_ = "/";
    std::string lsFlags_;
    int tz_ = 0;
    bool preciseTimes_ = false;
};

/** `scp -f`: the server is the source. */
class ScpReader final : public RemoteReader {
public:
    ScpReader(std::shared_ptr<ScpFsImpl> fs, const std::string& path) : fs_(std::move(fs)), ch_(fs_->transport()), in_(ch_) {
        try {
            ch_.start("scp -p -f " + q(path));
            ch_.write(std::string(1, '\0'));
            for (;;) {
                const char c = in_.byte();
                if (c == 1 || c == 2) throw SftpError("Cannot download " + quotePath(path) + ": " + trim(in_.line()));
                std::string error;
                auto rec = scp::parseRecord(std::string(1, c) + in_.line(), error);
                if (!rec) throw SftpError("Cannot download " + quotePath(path) + ": " + error);
                if (rec->type == 'T') {
                    mtime_ = rec->mtime;
                    ch_.write(std::string(1, '\0'));
                    continue;
                }
                if (rec->type != 'C') throw SftpError("Cannot download " + quotePath(path) + ": not a regular file");
                size_ = rec->size;
                remaining_ = rec->size;
                ch_.write(std::string(1, '\0'));
                break;
            }
        } catch (const ConnectionLost&) {
            fs_->markLost();
            throw;
        }
    }

    uint64_t size() const override { return size_; }
    int64_t mtime() const override { return mtime_; }

    std::shared_ptr<std::string> read(bool& done) override {
        auto out = std::make_shared<std::string>();
        if (remaining_ > 0) {
            *out = in_.take(static_cast<size_t>(std::min<uint64_t>(remaining_, kStepBytes)));
            remaining_ -= out->size();
        }
        if (remaining_ == 0 && !finished_) {
            in_.expectOk("Download failed");
            ch_.write(std::string(1, '\0'));
            ch_.sendEof();
            finished_ = true;
        }
        done = remaining_ == 0;
        return out;
    }

    void close() override { ch_.close(); }
    void abort() noexcept override { ch_.close(); }

private:
    std::shared_ptr<ScpFsImpl> fs_;
    ExecChannel ch_;
    ScpStream in_;
    uint64_t size_ = 0;
    uint64_t remaining_ = 0;
    int64_t mtime_ = 0;
    bool finished_ = false;
};

/** `scp -t`: the server is the sink. */
class ScpWriter final : public RemoteWriter {
public:
    ScpWriter(std::shared_ptr<ScpFsImpl> fs, const std::string& path, const WriteSpec& spec)
        : fs_(std::move(fs)), ch_(fs_->transport()), in_(ch_), path_(path), remaining_(spec.size) {
        try {
            ch_.start(std::string("scp") + (spec.mtime > 0 ? " -p" : "") + " -t " + q(path));
            in_.expectOk("Cannot upload to " + quotePath(path));
            if (spec.mtime > 0) {
                ch_.write(scp::timeRecord(spec.mtime, spec.mtime));
                in_.expectOk("Cannot upload to " + quotePath(path));
            }
            ch_.write(scp::fileRecord(spec.mode, spec.size, sftputil::remoteBaseName(path)));
            in_.expectOk("Cannot upload to " + quotePath(path));
        } catch (const ConnectionLost&) {
            fs_->markLost();
            throw;
        }
    }

    void write(const std::shared_ptr<std::string>& data, const ProgressFn& progress) override {
        if (data->size() > remaining_) throw SftpError("'" + path_ + "': the local file grew while uploading");
        ch_.write(data, progress);
        remaining_ -= data->size();
    }

    void finish(int64_t, const ProgressFn&) override {
        if (remaining_ != 0) throw SftpError("'" + path_ + "': the local file shrank while uploading");
        ch_.write(std::string(1, '\0'));
        in_.expectOk("Upload of " + quotePath(path_) + " failed");
        ch_.sendEof();
        ch_.close();
    }

    void abort() noexcept override { ch_.close(); }

private:
    std::shared_ptr<ScpFsImpl> fs_;
    ExecChannel ch_;
    ScpStream in_;
    std::string path_;
    uint64_t remaining_ = 0;
};

std::unique_ptr<RemoteReader> ScpFsImpl::openRead(const std::string& path, uint64_t offset) {
    if (offset > 0) throw SftpError("SCP cannot resume downloads");
    return std::make_unique<ScpReader>(shared_from_this(), path);
}

std::unique_ptr<RemoteWriter> ScpFsImpl::openWrite(const std::string& path, const WriteSpec& spec) {
    if (spec.offset > 0) throw SftpError("SCP cannot resume uploads");
    return std::make_unique<ScpWriter>(shared_from_this(), path, spec);
}

}  // namespace

std::shared_ptr<RemoteFs> openScpFs(const std::weak_ptr<Transport>& transport) {
    auto fs = std::make_shared<ScpFsImpl>(transport);
    fs->init();
    return fs;
}

}  // namespace sc::sftpimpl
