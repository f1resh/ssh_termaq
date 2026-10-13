#include "sftp/SftpService.h"

#include "app/Dialogs.h"
#include "bridge/Bridge.h"
#include "core/Log.h"
#include "core/Strings.h"
#include "core/Threading.h"
#include "core/Uuid.h"
#include "session/SessionManager.h"
#include "sftp/SftpSupport.h"
#include "sftp/SftpUtil.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace sc {

namespace fs = std::filesystem;
using namespace sftpimpl;

namespace {

constexpr size_t kMaxConcurrentTransfers = 2;
constexpr auto kProgressInterval = std::chrono::milliseconds(100);
constexpr auto kShutdownWait = std::chrono::seconds(5);
const wchar_t* const kPartialSuffix = L".sc-partial";

std::string argString(const Json& args, size_t i) {
    if (!args.is_array() || i >= args.size() || !args[i].is_string()) {
        throw std::invalid_argument("argument " + std::to_string(i) + " must be a string");
    }
    return args[i].get<std::string>();
}

std::vector<std::string> argStringList(const Json& args, size_t i, bool optional) {
    std::vector<std::string> out;
    if (!args.is_array() || i >= args.size() || args[i].is_null()) {
        if (optional) return out;
        throw std::invalid_argument("argument " + std::to_string(i) + " must be a string array");
    }
    if (!args[i].is_array()) throw std::invalid_argument("argument " + std::to_string(i) + " must be a string array");
    for (const auto& v : args[i]) {
        if (v.is_string()) out.push_back(v.get<std::string>());
    }
    return out;
}

bool argPresent(const Json& args, size_t i) { return args.is_array() && i < args.size() && !args[i].is_null(); }

std::string sq(const std::string& path) { return "'" + path + "'"; }

/** Remote name of a local path (last component, UTF-8). */
std::string localBaseName(const fs::path& p) {
    fs::path clean = p;
    if (!clean.has_filename()) clean = clean.parent_path();  // "C:\dir\" -> "C:\dir"
    std::string name = narrow(clean.filename().wstring());
    return name.empty() ? std::string("root") : name;
}

/** Unique-ish random folder name for openRemote temp downloads. */
std::wstring randomFolderName() { return widen(newUuid().substr(0, 8)); }

}  // namespace

/* ------------------------------------------------------------------------ */
/* State                                                                     */
/* ------------------------------------------------------------------------ */

struct Transfer;

/** One SFTP channel on an SSH connection. `sftp` is touched only on the connection thread. */
struct SftpSession {
    std::string id;
    std::string connId;
    std::weak_ptr<Transport> transport;
    sftp_session sftp = nullptr;  // SSH thread only
    std::atomic<bool> closing{false};
    std::atomic<bool> lost{false};

    // Transfer queue (max kMaxConcurrentTransfers workers).
    std::mutex qm;
    std::condition_variable qcv;
    std::deque<std::shared_ptr<Transfer>> queue;
    std::vector<std::shared_ptr<Transfer>> running;
    std::vector<std::thread> workers;
    size_t idle = 0;
};
using SessionPtr = std::shared_ptr<SftpSession>;

/** One planned unit of a transfer (directory to create or file to copy). */
struct TransferItem {
    bool dir = false;
    std::string remote;
    fs::path local;
    uint64_t size = 0;
    int64_t mtime = 0;  // unix seconds (uploads; downloads take it from the open remote file)
};

struct Transfer {
    std::string id;
    SessionPtr session;
    bool upload = false;
    std::string remotePath;  // download: source; upload: destination (remoteDir/name)
    fs::path localPath;      // download: destination; upload: source
    std::atomic<bool> cancel{false};
    /** Called once when finished (worker thread). ok == state "done". */
    std::function<void(bool ok, const std::string& error)> onFinished;

    std::mutex pm;
    TransferProgress progress;
    Clock::time_point lastEmit{};
    bool finished = false;
};
using TransferPtr = std::shared_ptr<Transfer>;

struct SftpCore : std::enable_shared_from_this<SftpCore> {
    SftpCore(SessionManager& s, Bridge& b) : sessions(s), bridge(b) {}

    SessionManager& sessions;
    Bridge& bridge;
    std::atomic<bool> stopping{false};

    std::mutex m;
    std::unordered_map<std::string, SessionPtr> byId;
    std::unordered_map<std::string, TransferPtr> transfers;

    // Background jobs (bridge async handlers, session teardown) still running; waited for by closeAll().
    std::mutex jobsMutex;
    std::condition_variable jobsCv;
    int jobs = 0;

    struct JobGuard {
        SftpCore& core;
        explicit JobGuard(SftpCore& c) : core(c) {
            std::lock_guard lk(core.jobsMutex);
            ++core.jobs;
        }
        ~JobGuard() {
            std::lock_guard lk(core.jobsMutex);
            --core.jobs;
            core.jobsCv.notify_all();
        }
        JobGuard(const JobGuard&) = delete;
        JobGuard& operator=(const JobGuard&) = delete;
    };

    /* ---------------------------------------------------------------- helpers */

    void checkRunning() const {
        if (stopping) throw SftpError("SSH Crawler is shutting down");
    }

    SessionPtr session(const std::string& id) {
        std::lock_guard lk(m);
        auto it = byId.find(id);
        if (it == byId.end()) throw SftpError("Unknown or closed SFTP session");
        return it->second;
    }

    /** fn(sftp_session) on the connection thread of `s`. */
    template <class F>
    auto runSftp(const SessionPtr& s, F fn) -> std::invoke_result_t<F&, sftp_session> {
        if (s->lost) throw ConnectionLost();
        try {
            return runOnSsh(s->transport, [s, fn](ssh_session) mutable {
                if (!s->sftp) throw SftpError("SFTP session is closed");
                return fn(s->sftp);
            });
        } catch (const ConnectionLost&) {
            s->lost = true;
            throw;
        }
    }

    /* --------------------------------------------------------------- sessions */

    Json open(const std::string& connId) {
        checkRunning();
        std::shared_ptr<Transport> t = sessions.transport(connId);
        if (!t) throw SftpError("Not connected");
        SshTransport* ssh = t->asSsh();
        if (!ssh) throw SftpError("SFTP is only available on SSH connections");
        if (!ssh->isConnected()) throw SftpError("Not connected");

        auto s = std::make_shared<SftpSession>();
        s->id = newUuid();
        s->connId = connId;
        s->transport = t;
        t.reset();  // never keep the connection alive from here

        const std::string home = runOnSsh(s->transport, [s](ssh_session session) {
            sftp_session sftp = sftp_new(session);
            if (!sftp) {
                const char* e = ssh_get_error(session);
                throw SftpError(std::string("Cannot open an SFTP channel: ") + (e && *e ? e : "unknown error"));
            }
            if (sftp_init(sftp) != SSH_OK) {
                std::string msg = sftpErrorText(sftp, "SFTP subsystem failed to start");
                sftp_free(sftp);
                throw SftpError(msg);
            }
            s->sftp = sftp;
            SshCharPtr home{sftp_canonicalize_path(sftp, ".")};
            return std::string(home ? home.get() : "/");
        });

        {
            std::lock_guard lk(m);
            byId[s->id] = s;
        }
        applog::info("sftp: opened session on " + connId);
        return Json{{"sftpId", s->id}, {"home", home}};
    }

    /** Cancels everything of `s`, joins its workers and frees the libssh channel. Blocking (background thread). */
    void teardown(const SessionPtr& s) {
        s->closing = true;
        std::vector<TransferPtr> dropped;
        {
            std::lock_guard lk(s->qm);
            for (auto& t : s->queue) dropped.push_back(t);
            s->queue.clear();
            for (auto& t : s->running) t->cancel = true;
        }
        s->qcv.notify_all();
        for (auto& t : dropped) {
            t->cancel = true;
            finish(t, "cancelled", "");
        }
        std::vector<std::thread> workers;
        {
            std::lock_guard lk(s->qm);
            workers.swap(s->workers);
        }
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }
        if (!s->lost) {
            try {
                runOnSsh(s->transport, [s](ssh_session) {
                    if (s->sftp) {
                        sftp_free(s->sftp);
                        s->sftp = nullptr;
                    }
                });
            } catch (const std::exception&) {
                // Connection already gone: the channel died with it.
            }
        }
    }

    void close(const std::string& id) {
        SessionPtr s;
        {
            std::lock_guard lk(m);
            auto it = byId.find(id);
            if (it == byId.end()) return;
            s = it->second;
            byId.erase(it);
        }
        auto self = shared_from_this();
        auto guard = std::make_shared<JobGuard>(*this);
        ThreadPool::shared().submit([self, s, guard] { self->teardown(s); });
    }

    void closeAll() {
        stopping = true;
        std::vector<SessionPtr> all;
        {
            std::lock_guard lk(m);
            for (auto& [id, s] : byId) all.push_back(s);
            byId.clear();
        }
        for (auto& s : all) teardown(s);
        std::unique_lock lk(jobsMutex);
        jobsCv.wait_for(lk, kShutdownWait, [this] { return jobs == 0; });
    }

    /* ------------------------------------------------------------- operations */

    /** Lists a directory with bounded steps; resolves symlink targets (stat) when statLinks. */
    std::vector<std::pair<SftpEntry, uint64_t>> listDir(const SessionPtr& s, const std::string& path, bool statLinks,
                                                        const std::atomic<bool>* cancel = nullptr) {
        struct State {
            sftp_dir dir = nullptr;
            bool eof = false;
            size_t linkIndex = 0;
            // entry + size of what it points to (links: target size; others: own size)
            std::vector<std::pair<SftpEntry, uint64_t>> entries;
        };
        auto st = std::make_shared<State>();
        runSftp(s, [st, path](sftp_session sftp) {
            st->dir = sftp_opendir(sftp, path.c_str());
            if (!st->dir) throwSftp(sftp, "Cannot open folder " + sq(path));
        });
        auto closeDir = [this, s, st] {
            try {
                runSftp(s, [st](sftp_session) {
                    if (st->dir) sftp_closedir(st->dir);
                    st->dir = nullptr;
                });
            } catch (const std::exception&) {
            }
        };
        try {
            while (!st->eof) {
                if (cancel && *cancel) throw Cancelled();
                runSftp(s, [st, path](sftp_session sftp) {
                    const auto start = Clock::now();
                    do {
                        AttrPtr a{sftp_readdir(sftp, st->dir)};
                        if (!a) {
                            if (!sftp_dir_eof(st->dir)) throwSftp(sftp, "Cannot list folder " + sq(path));
                            st->eof = true;
                            sftp_closedir(st->dir);
                            st->dir = nullptr;
                            break;
                        }
                        const std::string name = a->name ? a->name : "";
                        if (name.empty() || name == "." || name == "..") continue;
                        st->entries.emplace_back(toEntry(path, a.get()), a->size);
                    } while (Clock::now() - start < kStepBudget);
                });
            }
        } catch (...) {
            closeDir();
            throw;
        }
        if (statLinks) {
            while (st->linkIndex < st->entries.size()) {
                if (cancel && *cancel) throw Cancelled();
                runSftp(s, [st](sftp_session sftp) {
                    const auto start = Clock::now();
                    for (; st->linkIndex < st->entries.size(); ++st->linkIndex) {
                        auto& [e, targetSize] = st->entries[st->linkIndex];
                        if (e.type != "link") continue;
                        if (Clock::now() - start >= kStepBudget) break;
                        if (AttrPtr t{sftp_stat(sftp, e.path.c_str())}) {
                            const std::string tt = sftputil::entryType(t->type, t->permissions);
                            e.linkTargetIsDir = tt == "dir";
                            targetSize = tt == "file" ? t->size : 0;
                        } else {
                            targetSize = 0;  // dangling link
                        }
                    }
                });
            }
        }
        return std::move(st->entries);
    }

    Json list(const std::string& id, const std::string& path) {
        checkRunning();
        auto s = session(id);
        Json out = Json::array();
        for (auto& [e, targetSize] : listDir(s, path.empty() ? "." : path, true)) out.push_back(e);
        return out;
    }

    Json realpath(const std::string& id, const std::string& path) {
        checkRunning();
        auto s = session(id);
        return runSftp(s, [path](sftp_session sftp) {
            SshCharPtr p{sftp_canonicalize_path(sftp, path.empty() ? "." : path.c_str())};
            if (!p) throwSftp(sftp, "Cannot resolve " + sq(path));
            return std::string(p.get());
        });
    }

    Json mkdir(const std::string& id, const std::string& path) {
        checkRunning();
        auto s = session(id);
        runSftp(s, [path](sftp_session sftp) {
            if (sftp_mkdir(sftp, path.c_str(), 0755) != SSH_OK) throwSftp(sftp, "Cannot create folder " + sq(path));
        });
        return nullptr;
    }

    Json rename(const std::string& id, const std::string& from, const std::string& to) {
        checkRunning();
        auto s = session(id);
        runSftp(s, [from, to](sftp_session sftp) {
            if (sftp_rename(sftp, from.c_str(), to.c_str()) != SSH_OK) {
                throwSftp(sftp, "Cannot rename " + sq(from) + " to " + sq(to));
            }
        });
        return nullptr;
    }

    Json chmod(const std::string& id, const std::string& path, uint32_t mode) {
        checkRunning();
        auto s = session(id);
        runSftp(s, [path, mode](sftp_session sftp) {
            if (sftp_chmod(sftp, path.c_str(), static_cast<mode_t>(mode & 07777)) != SSH_OK) {
                throwSftp(sftp, "Cannot change permissions of " + sq(path));
            }
        });
        return nullptr;
    }

    /** Deletes files in bounded steps. */
    void unlinkAll(const SessionPtr& s, const std::vector<std::string>& files) {
        auto index = std::make_shared<size_t>(0);
        auto list = std::make_shared<std::vector<std::string>>(files);
        while (*index < list->size()) {
            runSftp(s, [index, list](sftp_session sftp) {
                const auto start = Clock::now();
                do {
                    const std::string& f = (*list)[*index];
                    if (sftp_unlink(sftp, f.c_str()) != SSH_OK) throwSftp(sftp, "Cannot delete " + sq(f));
                    ++*index;
                } while (*index < list->size() && Clock::now() - start < kStepBudget);
            });
        }
    }

    void removeTree(const SessionPtr& s, const std::string& path, bool recursive) {
        checkRunning();
        // lstat: a symlink to a directory is removed as a link, never followed.
        const std::string type = runSftp(s, [path](sftp_session sftp) {
            AttrPtr a{sftp_lstat(sftp, path.c_str())};
            if (!a) throwSftp(sftp, "Cannot delete " + sq(path));
            return sftputil::entryType(a->type, a->permissions);
        });
        if (type != "dir") {
            unlinkAll(s, {path});
            return;
        }
        if (recursive) {
            std::vector<std::string> files;
            for (auto& [e, ignored] : listDir(s, path, false)) {
                if (e.type == "dir") {
                    removeTree(s, e.path, true);
                } else {
                    files.push_back(e.path);
                }
            }
            unlinkAll(s, files);
        }
        runSftp(s, [path, recursive](sftp_session sftp) {
            if (sftp_rmdir(sftp, path.c_str()) != SSH_OK) {
                std::string msg = sftpErrorText(sftp, "Cannot delete folder " + sq(path));
                if (!recursive && sftp_get_error(sftp) == SSH_FX_FAILURE) msg += " (is it empty?)";
                throw SftpError(msg);
            }
        });
    }

    Json remove(const std::string& id, const std::string& path, bool recursive) {
        auto s = session(id);
        if (path.empty() || path == "/") throw SftpError("Refusing to delete " + sq(path));
        removeTree(s, path, recursive);
        return nullptr;
    }

    /* -------------------------------------------------------------- transfers */

    void emitProgress(const TransferProgress& p) { bridge.emit("sftp:onProgress", Json::array({Json(p)})); }

    void setState(const TransferPtr& t, const std::string& state) {
        TransferProgress copy;
        {
            std::lock_guard lk(t->pm);
            if (t->finished) return;
            t->progress.state = state;
            t->lastEmit = Clock::now();
            copy = t->progress;
        }
        emitProgress(copy);
    }

    void setTotal(const TransferPtr& t, uint64_t total) {
        TransferProgress copy;
        {
            std::lock_guard lk(t->pm);
            t->progress.total = static_cast<double>(total);
            t->lastEmit = Clock::now();
            copy = t->progress;
        }
        emitProgress(copy);
    }

    void addProgress(const TransferPtr& t, uint64_t delta) {
        if (delta == 0) return;
        TransferProgress copy;
        {
            std::lock_guard lk(t->pm);
            t->progress.transferred += static_cast<double>(delta);
            if (t->progress.total < t->progress.transferred) t->progress.total = t->progress.transferred;
            const auto now = Clock::now();
            if (now - t->lastEmit < kProgressInterval) return;
            t->lastEmit = now;
            copy = t->progress;
        }
        emitProgress(copy);
    }

    /** Final state (exactly once). */
    void finish(const TransferPtr& t, const std::string& state, const std::string& error) {
        TransferProgress copy;
        std::function<void(bool, const std::string&)> onFinished;
        {
            std::lock_guard lk(t->pm);
            if (t->finished) return;
            t->finished = true;
            t->progress.state = state;
            t->progress.error = error;
            copy = t->progress;
            onFinished = std::move(t->onFinished);
            t->onFinished = nullptr;
            t->session.reset();  // break the session <-> transfer reference
        }
        emitProgress(copy);
        {
            std::lock_guard lk(m);
            transfers.erase(t->id);
        }
        if (onFinished) onFinished(state == "done", state == "cancelled" ? std::string("Cancelled") : error);
    }

    TransferPtr makeTransfer(const SessionPtr& s, bool upload, const std::string& remote, const fs::path& local,
                             const std::string& name) {
        auto t = std::make_shared<Transfer>();
        t->id = newUuid();
        t->session = s;
        t->upload = upload;
        t->remotePath = remote;
        t->localPath = local;
        t->progress.transferId = t->id;
        t->progress.sftpId = s->id;
        t->progress.direction = upload ? "upload" : "download";
        t->progress.name = name;
        t->progress.localPath = narrow(local.wstring());
        t->progress.remotePath = remote;
        t->progress.state = "queued";
        return t;
    }

    void enqueue(const SessionPtr& s, const TransferPtr& t) {
        {
            std::lock_guard lk(m);
            transfers[t->id] = t;
        }
        setState(t, "queued");
        std::lock_guard lk(s->qm);
        if (s->closing) {
            t->cancel = true;
            ThreadPool::shared().submit([self = shared_from_this(), t] { self->finish(t, "cancelled", ""); });
            return;
        }
        s->queue.push_back(t);
        if (s->idle == 0 && s->workers.size() < kMaxConcurrentTransfers) {
            s->workers.emplace_back([self = shared_from_this(), s] { self->workerLoop(s); });
        } else {
            s->qcv.notify_one();
        }
    }

    void workerLoop(const SessionPtr& s) {
        for (;;) {
            TransferPtr t;
            {
                std::unique_lock lk(s->qm);
                ++s->idle;
                s->qcv.wait(lk, [&] { return s->closing.load() || !s->queue.empty(); });
                --s->idle;
                if (s->queue.empty()) return;  // closing
                t = s->queue.front();
                s->queue.pop_front();
                s->running.push_back(t);
            }
            runTransfer(s, t);
            {
                std::lock_guard lk(s->qm);
                s->running.erase(std::remove(s->running.begin(), s->running.end(), t), s->running.end());
            }
        }
    }

    void cancelTransfer(const std::string& transferId) {
        TransferPtr t;
        {
            std::lock_guard lk(m);
            auto it = transfers.find(transferId);
            if (it == transfers.end()) return;
            t = it->second;
        }
        t->cancel = true;
        SessionPtr s;
        {
            std::lock_guard lk(t->pm);
            s = t->session;
        }
        if (!s) return;
        bool wasQueued = false;
        {
            std::lock_guard lk(s->qm);
            auto it = std::find(s->queue.begin(), s->queue.end(), t);
            if (it != s->queue.end()) {
                s->queue.erase(it);
                wasQueued = true;
            }
        }
        if (wasQueued) finish(t, "cancelled", "");  // running transfers notice the flag between steps
    }

    void checkCancel(const TransferPtr& t) const {
        if (t->cancel || stopping) throw Cancelled();
    }

    void runTransfer(const SessionPtr& s, const TransferPtr& t) {
        std::string state = "done";
        std::string error;
        try {
            checkCancel(t);
            setState(t, "running");
            if (t->upload) {
                runUpload(s, t);
            } else {
                runDownload(s, t);
            }
        } catch (const Cancelled&) {
            state = "cancelled";
        } catch (const std::exception& e) {
            state = t->cancel ? "cancelled" : "error";
            error = e.what();
        }
        if (state == "error") {
            applog::warn("sftp: transfer " + t->progress.name + " failed: " + error);
        }
        finish(t, state, error);
    }

    /* ---- download */

    void planDownloadDir(const SessionPtr& s, const TransferPtr& t, const std::string& remoteDir, const fs::path& localDir,
                         std::vector<TransferItem>& items, uint64_t& total) {
        items.push_back(TransferItem{true, remoteDir, localDir, 0, 0});
        for (auto& [e, targetSize] : listDir(s, remoteDir, true, &t->cancel)) {
            const fs::path local = localDir / sftputil::sanitizeLocalName(e.name);
            if (e.type == "dir") {
                planDownloadDir(s, t, e.path, local, items, total);
            } else if (e.type == "file" || (e.type == "link" && !e.linkTargetIsDir && targetSize > 0)) {
                items.push_back(TransferItem{false, e.path, local, targetSize, e.mtime / 1000});
                total += targetSize;
            } else if (e.type == "link" && !e.linkTargetIsDir) {
                // Link to an empty file, or dangling: try it (an open error is reported).
                items.push_back(TransferItem{false, e.path, local, 0, 0});
            }
            // Links to directories are not followed (loop protection); special files are skipped.
        }
    }

    void runDownload(const SessionPtr& s, const TransferPtr& t) {
        // stat (follows links): a top-level link to a directory is downloaded as that directory.
        const auto [type, size] = runSftp(s, [path = t->remotePath](sftp_session sftp) {
            AttrPtr a{sftp_stat(sftp, path.c_str())};
            if (!a) throwSftp(sftp, "Cannot access " + sq(path));
            return std::pair<std::string, uint64_t>(sftputil::entryType(a->type, a->permissions), a->size);
        });
        std::vector<TransferItem> items;
        uint64_t total = 0;
        if (type == "dir") {
            planDownloadDir(s, t, t->remotePath, t->localPath, items, total);
        } else if (type == "file") {
            total = size;
            items.push_back(TransferItem{false, t->remotePath, t->localPath, size, 0});
        } else {
            throw SftpError(sq(t->remotePath) + " is not a regular file or folder");
        }
        setTotal(t, total);
        for (const auto& item : items) {
            checkCancel(t);
            if (item.dir) {
                std::error_code ec;
                fs::create_directories(item.local, ec);
                if (ec) throw SftpError("Cannot create folder '" + narrow(item.local.wstring()) + "': " + ec.message());
            } else {
                downloadFile(s, t, item);
            }
        }
    }

    void downloadFile(const SessionPtr& s, const TransferPtr& t, const TransferItem& item) {
        std::error_code ec;
        if (item.local.has_parent_path()) fs::create_directories(item.local.parent_path(), ec);
        fs::path part = item.local;
        part += kPartialSuffix;
        UniqueHandle h{CreateFileW(part.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
        if (h.get() == INVALID_HANDLE_VALUE) {
            throw SftpError("Cannot create '" + narrow(item.local.wstring()) + "': " + lastErrorMessage());
        }
        auto rf = std::make_shared<RemoteFile>();
        bool committed = false;
        try {
            runSftp(s, [rf, path = item.remote](sftp_session sftp) { rf->openRead(sftp, path); });
            for (;;) {
                checkCancel(t);
                auto r = runSftp(s, [rf](sftp_session sftp) { return rf->readStep(sftp); });
                const std::string& data = *r.data;
                size_t off = 0;
                while (off < data.size()) {
                    DWORD n = 0;
                    const DWORD want = static_cast<DWORD>(std::min<size_t>(data.size() - off, 1u << 30));
                    if (!WriteFile(h.get(), data.data() + off, want, &n, nullptr)) {
                        throw SftpError("Cannot write '" + narrow(item.local.wstring()) + "': " + lastErrorMessage());
                    }
                    off += n;
                }
                addProgress(t, data.size());
                if (r.done) break;
            }
            runSftp(s, [rf](sftp_session sftp) { rf->close(sftp, false); });
            if (rf->mtime() > 0) {
                const FILETIME ft = unixToFileTime(rf->mtime());
                SetFileTime(h.get(), nullptr, &ft, &ft);
            }
            h.reset();
            if (!MoveFileExW(part.c_str(), item.local.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
                throw SftpError("Cannot replace '" + narrow(item.local.wstring()) + "': " + lastErrorMessage());
            }
            committed = true;
        } catch (...) {
            abortRemote(s, rf);
            if (!committed) {
                h.reset();
                DeleteFileW(part.c_str());  // partial file cleanup
            }
            throw;
        }
    }

    /** Best-effort: discard in-flight requests and close the remote handle (error/cancel paths). */
    void abortRemote(const SessionPtr& s, const std::shared_ptr<RemoteFile>& rf) {
        try {
            bool drained = false;
            for (int i = 0; i < 200 && !drained; ++i) {
                drained = runSftp(s, [rf](sftp_session sftp) {
                    if (!rf->isOpen()) return true;
                    if (!rf->drainStep(sftp)) return false;
                    rf->close(sftp, false);
                    return true;
                });
            }
        } catch (const std::exception&) {
            rf->abandon();
        }
    }

    /* ---- upload */

    static bool localInfo(const fs::path& p, uint64_t& size, int64_t& mtime, bool& isDir) {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return false;
        isDir = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        mtime = fileTimeToUnix(fad.ftLastWriteTime);
        return true;
    }

    void runUpload(const SessionPtr& s, const TransferPtr& t) {
        uint64_t size = 0;
        int64_t mtime = 0;
        bool isDir = false;
        if (!localInfo(t->localPath, size, mtime, isDir)) {
            throw SftpError("Cannot read '" + narrow(t->localPath.wstring()) + "': " + lastErrorMessage());
        }
        std::vector<TransferItem> items;
        uint64_t total = 0;
        if (isDir) {
            items.push_back(TransferItem{true, t->remotePath, t->localPath, 0, mtime});
            std::error_code ec;
            fs::recursive_directory_iterator it(t->localPath, fs::directory_options::skip_permission_denied, ec), end;
            if (ec) throw SftpError("Cannot read folder '" + narrow(t->localPath.wstring()) + "': " + ec.message());
            for (; it != end; it.increment(ec)) {
                if (ec) break;
                checkCancel(t);
                const fs::path& p = it->path();
                const std::string rel = narrow(fs::relative(p, t->localPath, ec).generic_wstring());
                if (ec || rel.empty()) continue;
                const std::string remote = sftputil::joinRemote(t->remotePath, rel);
                uint64_t sz = 0;
                int64_t mt = 0;
                bool d = false;
                if (!localInfo(p, sz, mt, d)) continue;
                if (d) {
                    items.push_back(TransferItem{true, remote, p, 0, mt});
                } else if (it->is_regular_file(ec)) {
                    items.push_back(TransferItem{false, remote, p, sz, mt});
                    total += sz;
                }
            }
        } else {
            items.push_back(TransferItem{false, t->remotePath, t->localPath, size, mtime});
            total = size;
        }
        setTotal(t, total);
        for (const auto& item : items) {
            checkCancel(t);
            if (item.dir) {
                ensureRemoteDir(s, item.remote);
            } else {
                uploadFile(s, t, item);
            }
        }
    }

    void ensureRemoteDir(const SessionPtr& s, const std::string& path) {
        runSftp(s, [path](sftp_session sftp) {
            if (sftp_mkdir(sftp, path.c_str(), 0755) == SSH_OK) return;
            const std::string err = sftpErrorText(sftp, "Cannot create folder " + sq(path));
            AttrPtr a{sftp_stat(sftp, path.c_str())};
            if (!a || sftputil::entryType(a->type, a->permissions) != "dir") throw SftpError(err);
        });
    }

    void uploadFile(const SessionPtr& s, const TransferPtr& t, const TransferItem& item) {
        UniqueHandle h{CreateFileW(item.local.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
        if (h.get() == INVALID_HANDLE_VALUE) {
            throw SftpError("Cannot open '" + narrow(item.local.wstring()) + "': " + lastErrorMessage());
        }
        auto rf = std::make_shared<RemoteFile>();
        try {
            runSftp(s, [rf, path = item.remote](sftp_session sftp) { rf->openWrite(sftp, path, 0644); });
            auto buf = std::make_shared<std::string>();
            bool eof = false;
            while (!eof) {
                checkCancel(t);
                buf->resize(kStepBytes);
                DWORD n = 0;
                if (!ReadFile(h.get(), buf->data(), static_cast<DWORD>(buf->size()), &n, nullptr)) {
                    throw SftpError("Cannot read '" + narrow(item.local.wstring()) + "': " + lastErrorMessage());
                }
                buf->resize(n);
                eof = n == 0;
                size_t pos = 0;
                while (pos < buf->size()) {
                    checkCancel(t);
                    auto r = runSftp(s, [rf, buf, pos](sftp_session sftp) { return rf->writeStep(sftp, *buf, pos); });
                    pos = r.pos;
                    addProgress(t, r.acked);
                }
            }
            for (bool done = false; !done;) {
                checkCancel(t);
                auto [allAcked, acked] = runSftp(s, [rf](sftp_session sftp) { return rf->flushStep(sftp); });
                addProgress(t, acked);
                done = allAcked;
            }
            runSftp(s, [rf, item](sftp_session sftp) {
                rf->close(sftp, true);
                if (item.mtime > 0) {
                    timeval times[2];
                    times[0].tv_sec = static_cast<long>(item.mtime);
                    times[0].tv_usec = 0;
                    times[1] = times[0];
                    sftp_utimes(sftp, item.remote.c_str(), times);  // best effort (preserve mtime)
                }
            });
        } catch (const Cancelled&) {
            abortRemote(s, rf);
            // Remove the incomplete remote file (best effort).
            try {
                runSftp(s, [path = item.remote](sftp_session sftp) { sftp_unlink(sftp, path.c_str()); });
            } catch (const std::exception&) {
            }
            throw;
        } catch (...) {
            abortRemote(s, rf);
            throw;
        }
    }

    /* ---- entry points */

    Json download(const std::string& id, const std::vector<std::string>& remotePaths, std::string localDir) {
        checkRunning();
        auto s = session(id);
        if (remotePaths.empty()) return Json::array();
        if (localDir.empty()) {
            FileDialogOptions opts;
            opts.title = remotePaths.size() == 1 ? "Download " + sftputil::remoteBaseName(remotePaths[0]) + " to"
                                                 : "Download " + std::to_string(remotePaths.size()) + " items to";
            auto dir = dialogs::openDirectory(opts);
            if (!dir) return Json::array();
            localDir = *dir;
        }
        const fs::path base = widen(localDir);
        Json ids = Json::array();
        for (const auto& remote : remotePaths) {
            std::string name = sftputil::remoteBaseName(remote);
            if (name.empty() || name == "/" || name == "." || name == "..") name = "root";
            auto t = makeTransfer(s, false, remote, base / sftputil::sanitizeLocalName(name), name);
            enqueue(s, t);
            ids.push_back(t->id);
        }
        return ids;
    }

    Json upload(const std::string& id, const std::string& remoteDir, std::vector<std::string> localPaths, bool pick) {
        checkRunning();
        auto s = session(id);
        if (pick) {
            FileDialogOptions opts;
            opts.title = "Upload to " + remoteDir;
            localPaths = dialogs::openFiles(opts);
        }
        Json ids = Json::array();
        for (const auto& local : localPaths) {
            const fs::path p = widen(local);
            const std::string name = localBaseName(p);
            auto t = makeTransfer(s, true, sftputil::joinRemote(remoteDir, name), p, name);
            enqueue(s, t);
            ids.push_back(t->id);
        }
        return ids;
    }

    static fs::path tempRoot() {
        wchar_t buf[MAX_PATH + 1];
        const DWORD n = GetTempPathW(MAX_PATH + 1, buf);
        fs::path root = n ? fs::path(std::wstring(buf, n)) : fs::temp_directory_path();
        return root / L"SSH Crawler";
    }

    /** Removes openRemote temp folders older than a week (best effort, once per process). */
    static void pruneTemp(const fs::path& root) {
        static std::once_flag once;
        std::call_once(once, [&root] {
            std::error_code ec;
            const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24 * 7);
            for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code ec2;
                if (it->is_directory(ec2) && it->last_write_time(ec2) < cutoff) fs::remove_all(it->path(), ec2);
            }
        });
    }

    /** File types that run code when "opened": never executed directly from a remote download. */
    static bool isExecutableType(const fs::path& file) {
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

    /** Mark-of-the-Web: lets SmartScreen / Office Protected View treat the file as downloaded from the Internet. */
    static void markOfTheWeb(const fs::path& file) {
        std::ofstream ads(file.wstring() + L":Zone.Identifier", std::ios::binary | std::ios::trunc);
        if (ads) ads << "[ZoneTransfer]\r\nZoneId=3\r\n";
    }

    static void shellOpen(const fs::path& file) {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        markOfTheWeb(file);
        if (isExecutableType(file)) {
            // Show the file in Explorer instead of running downloaded code.
            if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(file.c_str())) {
                SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                ILFree(pidl);
            }
            if (SUCCEEDED(hr)) CoUninitialize();
            return;
        }
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof sei;
        sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        sei.lpFile = file.c_str();
        sei.nShow = SW_SHOWNORMAL;
        BOOL ok = ShellExecuteExW(&sei);
        if (!ok && GetLastError() == ERROR_NO_ASSOCIATION) {
            sei.lpVerb = L"openas";  // no default application: let the user pick one
            ok = ShellExecuteExW(&sei);
        }
        const std::string err = ok ? std::string() : lastErrorMessage();
        if (SUCCEEDED(hr)) CoUninitialize();
        if (!ok) throw SftpError("Cannot open '" + narrow(file.filename().wstring()) + "': " + err);
    }

    void openRemote(const std::string& id, const std::string& remotePath, Bridge::Responder reply) {
        checkRunning();
        auto s = session(id);
        const fs::path root = tempRoot();
        pruneTemp(root);
        const fs::path dir = root / randomFolderName();
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) throw SftpError("Cannot create a temporary folder: " + ec.message());
        std::string name = sftputil::remoteBaseName(remotePath);
        const fs::path local = dir / sftputil::sanitizeLocalName(name);
        auto t = makeTransfer(s, false, remotePath, local, name);
        t->onFinished = [reply, local, dir](bool ok, const std::string& error) {
            if (!ok) {
                std::error_code ignored;
                fs::remove_all(dir, ignored);
                reply.reject(error);
                return;
            }
            ThreadPool::shared().submit([reply, local] {
                try {
                    shellOpen(local);
                    reply.resolve(nullptr);
                } catch (const std::exception& e) {
                    reply.reject(e.what());
                }
            });
        };
        enqueue(s, t);
    }
};

/* ------------------------------------------------------------------------ */
/* SftpService                                                               */
/* ------------------------------------------------------------------------ */

struct SftpService::Impl {
    std::shared_ptr<SftpCore> core;
    bool closed = false;
};

SftpService::SftpService(SessionManager& sessions, Bridge& bridge) : impl_(std::make_unique<Impl>()) {
    impl_->core = std::make_shared<SftpCore>(sessions, bridge);
}

SftpService::~SftpService() { closeAll(); }

void SftpService::registerHandlers() {
    auto core = impl_->core;
    Bridge& bridge = core->bridge;

    // Runs `fn` on the shared pool, tracked so closeAll() can wait for it.
    auto async = [core](std::function<Json(const Json&)> fn) {
        return [core, fn](const Json& args, Bridge::Responder reply) {
            auto guard = std::make_shared<SftpCore::JobGuard>(*core);
            ThreadPool::shared().submit([core, fn, args, reply, guard] {
                try {
                    reply.resolve(fn(args));
                } catch (const std::exception& e) {
                    reply.reject(e.what());
                }
            });
        };
    };

    bridge.handle("sftp:open", async([core](const Json& a) { return core->open(argString(a, 0)); }));
    bridge.handle("sftp:list", async([core](const Json& a) { return core->list(argString(a, 0), argString(a, 1)); }));
    bridge.handle("sftp:realpath",
                  async([core](const Json& a) { return core->realpath(argString(a, 0), argString(a, 1)); }));
    bridge.handle("sftp:mkdir", async([core](const Json& a) { return core->mkdir(argString(a, 0), argString(a, 1)); }));
    bridge.handle("sftp:rename", async([core](const Json& a) {
                      return core->rename(argString(a, 0), argString(a, 1), argString(a, 2));
                  }));
    bridge.handle("sftp:remove", async([core](const Json& a) {
                      const bool recursive = argPresent(a, 2) && a[2].is_boolean() && a[2].get<bool>();
                      return core->remove(argString(a, 0), argString(a, 1), recursive);
                  }));
    bridge.handle("sftp:chmod", async([core](const Json& a) {
                      if (!argPresent(a, 2) || !a[2].is_number()) throw std::invalid_argument("mode must be a number");
                      return core->chmod(argString(a, 0), argString(a, 1), a[2].get<uint32_t>());
                  }));
    bridge.handle("sftp:download", async([core](const Json& a) {
                      const std::string localDir = argPresent(a, 2) && a[2].is_string() ? a[2].get<std::string>() : "";
                      return core->download(argString(a, 0), argStringList(a, 1, false), localDir);
                  }));
    bridge.handle("sftp:upload", async([core](const Json& a) {
                      const bool pick = !argPresent(a, 2);
                      return core->upload(argString(a, 0), argString(a, 1), argStringList(a, 2, true), pick);
                  }));
    bridge.on("sftp:cancelTransfer", [core](const Json& a) { core->cancelTransfer(argString(a, 0)); });
    bridge.handle("sftp:openRemote", [core](const Json& args, Bridge::Responder reply) {
        auto guard = std::make_shared<SftpCore::JobGuard>(*core);
        ThreadPool::shared().submit([core, args, reply, guard] {
            try {
                core->openRemote(argString(args, 0), argString(args, 1), reply);
            } catch (const std::exception& e) {
                reply.reject(e.what());
            }
        });
    });
    bridge.on("sftp:close", [core](const Json& a) { core->close(argString(a, 0)); });
}

void SftpService::closeAll() {
    if (!impl_ || impl_->closed) return;
    impl_->closed = true;
    impl_->core->closeAll();
}

}  // namespace sc
