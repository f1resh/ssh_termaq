#include "sftp/SftpService.h"

#include "app/Dialogs.h"
#include "bridge/Bridge.h"
#include "core/Log.h"
#include "core/Strings.h"
#include "core/Threading.h"
#include "core/Uuid.h"
#include "dnd/DragDrop.h"
#include "session/SessionManager.h"
#include "sftp/FindSync.h"
#include "sftp/LocalOpen.h"
#include "sftp/RemoteDragSource.h"
#include "sftp/ScpFs.h"
#include "sftp/SftpFs.h"
#include "sftp/SftpSupport.h"
#include "sftp/SftpUtil.h"
#include "sftp/TransferEngine.h"

#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace st {

namespace fs = std::filesystem;
using namespace sftpimpl;

namespace {

constexpr auto kShutdownWait = std::chrono::seconds(5);

/** Required string argument. */
std::string argString(const Json& args, size_t i) {
    const Json& v = jsonArg(args, i);
    if (!v.is_string()) throw std::invalid_argument("argument " + std::to_string(i) + " must be a string");
    return v.get<std::string>();
}

std::vector<std::string> argStringList(const Json& args, size_t i, bool optional) {
    const Json& v = jsonArg(args, i);
    if (v.is_null() && optional) return {};
    if (!v.is_array()) throw std::invalid_argument("argument " + std::to_string(i) + " must be a string array");
    return argStrList(args, i);
}

bool argPresent(const Json& args, size_t i) { return !jsonArg(args, i).is_null(); }

template <class T>
T argObject(const Json& args, size_t i) {
    const Json& v = jsonArg(args, i);
    return v.is_object() ? v.get<T>() : T{};
}

template <class T>
std::vector<T> argArray(const Json& args, size_t i) {
    const Json& v = jsonArg(args, i);
    std::vector<T> out;
    if (v.is_array())
        for (const auto& e : v) out.push_back(e.get<T>());
    return out;
}

/** Last write time + size of a local file; false when it is gone. */
bool fileStamp(const fs::path& p, uint64_t& size, uint64_t& writeTime) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return false;
    size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    writeTime = (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) | fad.ftLastWriteTime.dwLowDateTime;
    return true;
}

/** False while another program holds the file without read sharing (an editor still saving). */
bool readableNow(const fs::path& p) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

/** Synchronize paths come back from the page: relative, '/'-separated, no "..". */
bool safeRelPath(const std::string& rel) {
    if (rel.empty() || rel[0] == '/' || rel.find('\\') != std::string::npos) return false;
    for (const auto& part : split(rel, '/')) {
        if (part.empty() || part == "." || part == "..") return false;
    }
    return true;
}

/** Remote name of a local path (last component, UTF-8). */
std::string localBaseName(const fs::path& p) {
    fs::path clean = p;
    if (!clean.has_filename()) clean = clean.parent_path();  // "C:\dir\" -> "C:\dir"
    std::string name = narrow(clean.filename().wstring());
    return name.empty() ? std::string("root") : name;
}

}  // namespace

/* ------------------------------------------------------------------------ */
/* State                                                                     */
/* ------------------------------------------------------------------------ */

/** One remote file session (SFTP channel or SCP) on an SSH connection. */
struct SftpSession {
    std::string id;
    std::string connId;
    std::shared_ptr<RemoteFs> fs;
};
using SessionPtr = std::shared_ptr<SftpSession>;

/** A remote file opened for editing: its temp copy is watched and uploaded after every save. */
struct EditEntry {
    std::string editId;
    std::string sessionId;
    std::string remotePath;
    std::string editor;
    fs::path localPath;
    // guarded by SftpCore::em
    int64_t remoteMtime = 0;  // server copy as last downloaded / uploaded (Unix s; 0 = unknown)
    uint64_t size = 0;
    uint64_t writeTime = 0;
    Clock::time_point changedAt{};
    bool dirty = false;
    bool uploading = false;
};
using EditPtr = std::shared_ptr<EditEntry>;

struct FindJob {
    std::string sessionId;
    std::atomic<bool> cancel{false};
};

struct SftpCore : std::enable_shared_from_this<SftpCore> {
    SftpCore(SessionManager& s, Bridge& b) : sessions(s), bridge(b) {
        EngineHooks hooks;
        hooks.progress = [this](const TransferProgress& p) { bridge.emit("sftp:onProgress", Json::array({Json(p)})); };
        hooks.conflict = [this](const TransferConflict& c) { bridge.emit("sftp:onConflict", Json::array({Json(c)})); };
        engine = std::make_unique<TransferEngine>(std::move(hooks));
    }

    SessionManager& sessions;
    Bridge& bridge;
    std::unique_ptr<TransferEngine> engine;
    std::atomic<bool> stopping{false};

    std::mutex m;
    std::unordered_map<std::string, SessionPtr> byId;
    std::unordered_map<std::string, std::shared_ptr<FindJob>> finds;  // guarded by m

    // Edits and their watcher thread.
    std::mutex em;
    std::condition_variable ecv;
    std::unordered_map<std::string, EditPtr> edits;
    std::thread watcher;
    bool watcherStop = false;
    std::function<void(const std::string&, const std::string&)> editLauncher;  // tests

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

    void checkRunning() const {
        if (stopping) throw SftpError("SSHTerm is shutting down");
    }

    SessionPtr session(const std::string& id) {
        std::lock_guard lk(m);
        auto it = byId.find(id);
        if (it == byId.end()) throw SftpError("Unknown or closed file session");
        return it->second;
    }

    /* --------------------------------------------------------------- sessions */

    Json open(const std::string& connId, const SftpOpenOptions& opts) {
        checkRunning();
        std::shared_ptr<Transport> t = sessions.transport(connId);
        if (!t) throw SftpError("Not connected");
        SshTransport* ssh = t->asSsh();
        if (!ssh) throw SftpError("File transfer is only available on SSH connections");
        if (!ssh->isConnected()) throw SftpError("Not connected");
        std::weak_ptr<Transport> weak = t;
        t.reset();  // never keep the connection alive from here

        std::shared_ptr<RemoteFs> rfs;
        std::string sftpError;
        if (opts.fileProtocol != FileProtocol::Scp) {
            try {
                rfs = openSftpFs(weak);
            } catch (const ConnectionLost&) {
                throw;
            } catch (const SftpError& e) {
                if (opts.fileProtocol == FileProtocol::Sftp) throw;
                sftpError = e.what();
            }
        }
        if (!rfs) {
            try {
                rfs = openScpFs(weak);
            } catch (const ConnectionLost&) {
                throw;
            } catch (const SftpError& e) {
                if (sftpError.empty()) throw;
                throw SftpError("The server supports neither SFTP nor SCP. " + sftpError + ". SCP: " + e.what());
            }
            if (!sftpError.empty()) applog::info("sftp: " + sftpError + "; using SCP on " + connId);
        }

        auto s = std::make_shared<SftpSession>();
        s->id = newUuid();
        s->connId = connId;
        s->fs = rfs;
        engine->addQueue(s->id, rfs, opts.parallelTransfers);
        {
            std::lock_guard lk(m);
            byId[s->id] = s;
        }
        applog::info("sftp: opened " + rfs->protocol() + " session on " + connId);
        return Json{{"sftpId", s->id}, {"home", rfs->home()}, {"protocol", rfs->protocol()}, {"capabilities", rfs->capabilities()}};
    }

    /** Cancels everything of `s` and releases its protocol resources. Blocking (background thread). */
    void teardown(const SessionPtr& s) {
        engine->removeQueue(s->id);
        s->fs->close();
    }

    void close(const std::string& id) {
        SessionPtr s;
        {
            std::lock_guard lk(m);
            auto it = byId.find(id);
            if (it == byId.end()) return;
            s = it->second;
            byId.erase(it);
            for (auto& [fid, job] : finds)
                if (job->sessionId == id) job->cancel = true;
        }
        stopEditsOf(id);
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
            for (auto& [fid, job] : finds) job->cancel = true;
        }
        {
            std::lock_guard lk(em);
            watcherStop = true;
            edits.clear();
        }
        ecv.notify_all();
        if (watcher.joinable()) watcher.join();
        for (auto& s : all) teardown(s);
        engine->stop();
        std::unique_lock lk(jobsMutex);
        jobsCv.wait_for(lk, kShutdownWait, [this] { return jobs == 0; });
    }

    /* ------------------------------------------------------------- operations */

    Json list(const std::string& id, const std::string& path) {
        checkRunning();
        auto s = session(id);
        Json out = Json::array();
        for (auto& e : s->fs->list(path.empty() ? "." : path, true)) out.push_back(e.entry);
        return out;
    }

    Json stat(const std::string& id, const std::string& path) {
        checkRunning();
        auto e = session(id)->fs->stat(path, true);
        return e ? Json(*e) : Json(nullptr);
    }

    Json realpath(const std::string& id, const std::string& path) {
        checkRunning();
        return session(id)->fs->realpath(path);
    }

    Json mkdir(const std::string& id, const std::string& path) {
        checkRunning();
        session(id)->fs->mkdir(path, 0755);
        return nullptr;
    }

    Json rename(const std::string& id, const std::string& from, const std::string& to) {
        checkRunning();
        session(id)->fs->rename(from, to);
        return nullptr;
    }

    Json chmod(const std::string& id, const std::string& path, uint32_t mode) {
        checkRunning();
        session(id)->fs->chmod(path, mode);
        return nullptr;
    }

    Json remove(const std::string& id, const std::string& path, bool recursive) {
        checkRunning();
        auto s = session(id);
        if (path.empty() || path == "/") throw SftpError("Refusing to delete " + quotePath(path));
        if (recursive) {
            s->fs->removeTree(path);
            return nullptr;
        }
        auto st = s->fs->stat(path, false);  // lstat: links are removed, never followed
        if (!st) throw SftpError("Cannot delete " + quotePath(path) + ": No such file or folder");
        if (st->type == "dir")
            s->fs->rmdir(path);
        else
            s->fs->unlink(path);
        return nullptr;
    }

    /* -------------------------------------------------------------- transfers */

    Json download(const std::string& id, const std::vector<std::string>& remotePaths, std::string localDir,
                  const TransferOptions& opts) {
        checkRunning();
        auto s = session(id);
        if (remotePaths.empty()) return Json::array();
        if (localDir.empty()) {
            FileDialogOptions d;
            d.title = remotePaths.size() == 1 ? "Download " + sftputil::remoteBaseName(remotePaths[0]) + " to"
                                              : "Download " + std::to_string(remotePaths.size()) + " items to";
            auto dir = dialogs::openDirectory(d);
            if (!dir) return Json::array();
            localDir = *dir;
        }
        const fs::path base = widen(localDir);
        auto group = TransferEngine::newGroup(opts);
        group->multiple = remotePaths.size() > 1;
        Json ids = Json::array();
        for (const auto& remote : remotePaths) {
            std::string name = sftputil::remoteBaseName(remote);
            if (name.empty() || name == "/" || name == "." || name == "..") name = "root";
            auto t = engine->create(s->id, TransferKind::Download, remote, base / sftputil::sanitizeLocalName(name), name,
                                    opts, group);
            engine->enqueue(t);
            ids.push_back(t->id);
        }
        return ids;
    }

    Json upload(const std::string& id, const std::string& remoteDir, std::vector<std::string> localPaths, bool pick,
                const TransferOptions& opts) {
        checkRunning();
        auto s = session(id);
        if (pick) {
            FileDialogOptions d;
            d.title = "Upload to " + remoteDir;
            localPaths = dialogs::openFiles(d);
        }
        auto group = TransferEngine::newGroup(opts);
        group->multiple = localPaths.size() > 1;
        Json ids = Json::array();
        for (const auto& local : localPaths) {
            const fs::path p = widen(local);
            const std::string name = localBaseName(p);
            auto t = engine->create(s->id, TransferKind::Upload, sftputil::joinRemote(remoteDir, name), p, name, opts, group);
            engine->enqueue(t);
            ids.push_back(t->id);
        }
        return ids;
    }

    /* ------------------------------------------------------------------- find */

    void startFind(const std::string& id, const FindRequest& req, Bridge::Responder reply) {
        checkRunning();
        auto s = session(id);
        auto job = std::make_shared<FindJob>();
        job->sessionId = s->id;
        const std::string findId = newUuid();
        {
            std::lock_guard lk(m);
            finds[findId] = job;
        }
        // Resolve first: the reply reaches the page before the first onFind event.
        reply.resolve(findId);
        auto self = shared_from_this();
        auto guard = std::make_shared<JobGuard>(*this);
        std::thread([self, s, job, findId, req, guard] {
            auto emit = [&](FindEvent& ev) {
                ev.findId = findId;
                ev.sftpId = s->id;
                self->bridge.emit("sftp:onFind", Json::array({Json(ev)}));
            };
            try {
                runFind(*s->fs, req, job->cancel, emit);
            } catch (const std::exception& e) {
                FindEvent ev;
                ev.done = true;
                ev.error = e.what();
                emit(ev);
            }
            std::lock_guard lk(self->m);
            self->finds.erase(findId);
        }).detach();
    }

    void cancelFind(const std::string& findId) {
        std::lock_guard lk(m);
        auto it = finds.find(findId);
        if (it != finds.end()) it->second->cancel = true;
    }

    /* ------------------------------------------------------------ synchronize */

    Json syncCompare(const std::string& id, const SyncRequest& req) {
        checkRunning();
        auto s = session(id);
        if (req.localDir.empty() || req.remoteDir.empty()) throw SftpError("Choose a local and a remote folder");
        const SyncFilter filter(req);
        const Tree local = walkLocal(widen(req.localDir), filter, stopping);
        const Tree remote = walkRemote(*s->fs, req.remoteDir, filter, stopping);
        const int tolerance = s->fs->capabilities().preciseTimes ? 2 : 60;
        Json out = Json::array();
        for (auto& item : compareTrees(local, remote, req, tolerance)) out.push_back(item);
        return out;
    }

    Json syncApply(const std::string& id, const SyncRequest& req, const std::vector<SyncItem>& items, TransferOptions opts) {
        checkRunning();
        auto s = session(id);
        opts.preserveTimestamps = true;  // the next comparison relies on them
        if (opts.conflict == "ask") opts.conflict = "overwrite";  // the preview already decided
        opts.move = false;
        auto group = TransferEngine::newGroup(opts);
        group->multiple = items.size() > 1;
        const fs::path localRoot = widen(req.localDir);
        Json ids = Json::array();
        for (const auto& item : items) {
            if (!safeRelPath(item.relPath)) throw SftpError("Invalid path in the synchronize list: " + item.relPath);
            fs::path local = localRoot;
            for (const auto& part : split(item.relPath, '/')) local /= sftputil::sanitizeLocalName(part);
            const std::string remote = sftputil::joinRemote(req.remoteDir, item.relPath);
            TransferKind kind;
            if (item.action == "upload") kind = TransferKind::Upload;
            else if (item.action == "download") kind = TransferKind::Download;
            else if (item.action == "deleteRemote") kind = TransferKind::DeleteRemote;
            else if (item.action == "deleteLocal") kind = TransferKind::DeleteLocal;
            else throw SftpError("Unknown synchronize action: " + item.action);
            auto t = engine->create(s->id, kind, remote, local, item.relPath, opts, group, "sync");
            engine->enqueue(t);
            ids.push_back(t->id);
        }
        return ids;
    }

    /* ------------------------------------------------------------------- edit */

    void emitEdit(const EditEntry& e, const std::string& state, const std::string& error) {
        EditEvent ev;
        ev.editId = e.editId;
        ev.sftpId = e.sessionId;
        ev.remotePath = e.remotePath;
        ev.localPath = narrow(e.localPath.wstring());
        ev.state = state;
        ev.error = error;
        bridge.emit("sftp:onEdit", Json::array({Json(ev)}));
    }

    void edit(const std::string& id, const std::string& remotePath, const EditOptions& opts, Bridge::Responder reply) {
        checkRunning();
        auto s = session(id);
        const fs::path dir = newTempFolder();
        const std::string name = sftputil::remoteBaseName(remotePath);
        const fs::path local = dir / sftputil::sanitizeLocalName(name);
        TransferOptions topts;
        topts.conflict = "overwrite";
        auto t = engine->create(s->id, TransferKind::Download, remotePath, local, name, topts, nullptr, "edit");
        auto self = shared_from_this();
        t->onFinished = [self, s, reply, local, dir, remotePath, editor = opts.editor](bool ok, const std::string& error) {
            if (!ok) {
                std::error_code ignored;
                fs::remove_all(dir, ignored);
                reply.reject(error);
                return;
            }
            ThreadPool::shared().submit([self, s, reply, local, remotePath, editor] {
                try {
                    auto e = std::make_shared<EditEntry>();
                    e->editId = newUuid();
                    e->sessionId = s->id;
                    e->remotePath = remotePath;
                    e->localPath = local;
                    e->editor = editor;
                    if (auto st = s->fs->stat(remotePath, true)) e->remoteMtime = st->mtime / 1000;
                    fileStamp(local, e->size, e->writeTime);
                    if (self->editLauncher)
                        self->editLauncher(narrow(local.wstring()), editor);
                    else
                        shellEdit(local, editor);
                    {
                        std::lock_guard lk(self->em);
                        if (self->watcherStop) throw SftpError("SSHTerm is shutting down");
                        self->edits[e->editId] = e;
                        if (!self->watcher.joinable()) self->watcher = std::thread([self] { self->watchLoop(); });
                    }
                    reply.resolve(e->editId);
                    self->emitEdit(*e, "opened", "");
                } catch (const std::exception& ex) {
                    reply.reject(ex.what());
                }
            });
        };
        engine->enqueue(t);
    }

    void stopEdit(const std::string& editId) {
        EditPtr e;
        {
            std::lock_guard lk(em);
            auto it = edits.find(editId);
            if (it == edits.end()) return;
            e = it->second;
            edits.erase(it);
        }
        emitEdit(*e, "closed", "");
    }

    void stopEditsOf(const std::string& sessionId) {
        std::vector<EditPtr> stopped;
        {
            std::lock_guard lk(em);
            for (auto it = edits.begin(); it != edits.end();) {
                if (it->second->sessionId == sessionId) {
                    stopped.push_back(it->second);
                    it = edits.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& e : stopped) emitEdit(*e, "closed", "");
    }

    void watchLoop() {
        std::unique_lock lk(em);
        while (!watcherStop) {
            ecv.wait_for(lk, std::chrono::milliseconds(500), [this] { return watcherStop; });
            if (watcherStop) break;
            std::vector<EditPtr> list;
            for (auto& [id, e] : edits) list.push_back(e);
            lk.unlock();
            for (auto& e : list) checkEdit(e);
            lk.lock();
        }
    }

    /** Uploads a saved copy once it stopped changing for a moment and the editor released it. */
    void checkEdit(const EditPtr& e) {
        uint64_t size = 0, writeTime = 0;
        if (!fileStamp(e->localPath, size, writeTime)) return;  // being replaced by an atomic save
        const auto now = Clock::now();
        {
            std::lock_guard lk(em);
            if (size != e->size || writeTime != e->writeTime) {
                e->size = size;
                e->writeTime = writeTime;
                e->changedAt = now;
                e->dirty = true;
                return;
            }
            if (!e->dirty || e->uploading || now - e->changedAt < std::chrono::milliseconds(700)) return;
        }
        if (!readableNow(e->localPath)) return;
        {
            std::lock_guard lk(em);
            if (!edits.count(e->editId)) return;  // stopped meanwhile
            e->dirty = false;
            e->uploading = true;
        }
        startEditUpload(e);
    }

    void startEditUpload(const EditPtr& e) {
        SessionPtr s;
        try {
            s = session(e->sessionId);
        } catch (const std::exception&) {
            std::lock_guard lk(em);
            e->uploading = false;
            return;
        }
        TransferOptions opts;
        opts.conflict = "overwrite";
        opts.preserveTimestamps = false;
        auto t = engine->create(s->id, TransferKind::Upload, e->remotePath, e->localPath,
                                sftputil::remoteBaseName(e->remotePath), opts, nullptr, "edit");
        {
            std::lock_guard lk(em);
            t->expectedRemoteMtime = e->remoteMtime;
        }
        std::weak_ptr<Transfer> weak = t;
        auto self = shared_from_this();
        t->onFinished = [self, e, weak](bool ok, const std::string& error) {
            int skipped = 0;
            int64_t newMtime = 0;
            if (auto tt = weak.lock()) {
                std::lock_guard lk(tt->pm);
                skipped = tt->progress.skipped;
                newMtime = tt->expectedRemoteMtime;
            }
            {
                std::lock_guard lk(self->em);
                e->uploading = false;
                if (ok && !skipped && newMtime > 0) e->remoteMtime = newMtime;
            }
            if (!ok)
                self->emitEdit(*e, "error", error);
            else if (skipped)
                self->emitEdit(*e, "error", "Not uploaded: the server copy was kept");
            else
                self->emitEdit(*e, "uploaded", "");
        };
        emitEdit(*e, "uploading", "");
        engine->enqueue(t);
    }

    void openRemote(const std::string& id, const std::string& remotePath, Bridge::Responder reply) {
        checkRunning();
        auto s = session(id);
        const fs::path dir = newTempFolder();
        const std::string name = sftputil::remoteBaseName(remotePath);
        const fs::path local = dir / sftputil::sanitizeLocalName(name);
        TransferOptions opts;
        opts.conflict = "overwrite";
        auto t = engine->create(s->id, TransferKind::Download, remotePath, local, name, opts, nullptr, "open");
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
        engine->enqueue(t);
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

    bridge.handle("sftp:open", async([core](const Json& a) {
                      return core->open(argString(a, 0), argObject<SftpOpenOptions>(a, 1));
                  }));
    bridge.handle("sftp:list", async([core](const Json& a) { return core->list(argString(a, 0), argString(a, 1)); }));
    bridge.handle("sftp:stat", async([core](const Json& a) { return core->stat(argString(a, 0), argString(a, 1)); }));
    bridge.handle("sftp:realpath",
                  async([core](const Json& a) { return core->realpath(argString(a, 0), argString(a, 1)); }));
    bridge.handle("sftp:mkdir", async([core](const Json& a) { return core->mkdir(argString(a, 0), argString(a, 1)); }));
    bridge.handle("sftp:rename", async([core](const Json& a) {
                      return core->rename(argString(a, 0), argString(a, 1), argString(a, 2));
                  }));
    bridge.handle("sftp:remove", async([core](const Json& a) {
                      return core->remove(argString(a, 0), argString(a, 1), argBool(a, 2, false));
                  }));
    bridge.handle("sftp:chmod", async([core](const Json& a) {
                      if (!jsonArg(a, 2).is_number()) throw std::invalid_argument("mode must be a number");
                      return core->chmod(argString(a, 0), argString(a, 1), a[2].get<uint32_t>());
                  }));
    bridge.handle("sftp:download", async([core](const Json& a) {
                      return core->download(argString(a, 0), argStringList(a, 1, false), argStr(a, 2),
                                            argObject<TransferOptions>(a, 3));
                  }));
    bridge.handle("sftp:upload", async([core](const Json& a) {
                      const bool pick = !argPresent(a, 2);
                      return core->upload(argString(a, 0), argString(a, 1), argStringList(a, 2, true), pick,
                                          argObject<TransferOptions>(a, 3));
                  }));
    bridge.on("sftp:cancelTransfer", [core](const Json& a) { core->engine->cancel(argString(a, 0)); });
    bridge.on("sftp:resolveConflict", [core](const Json& a) {
        core->engine->resolveConflict(argString(a, 0), argObject<ConflictResolution>(a, 1));
    });
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

    bridge.handle("sftp:find", [core](const Json& args, Bridge::Responder reply) {
        auto guard = std::make_shared<SftpCore::JobGuard>(*core);
        ThreadPool::shared().submit([core, args, reply, guard] {
            try {
                core->startFind(argString(args, 0), argObject<FindRequest>(args, 1), reply);
            } catch (const std::exception& e) {
                reply.reject(e.what());
            }
        });
    });
    bridge.on("sftp:cancelFind", [core](const Json& a) { core->cancelFind(argString(a, 0)); });
    bridge.handle("sftp:syncCompare", async([core](const Json& a) {
                      return core->syncCompare(argString(a, 0), argObject<SyncRequest>(a, 1));
                  }));
    bridge.handle("sftp:syncApply", async([core](const Json& a) {
                      return core->syncApply(argString(a, 0), argObject<SyncRequest>(a, 1), argArray<SyncItem>(a, 2),
                                             argObject<TransferOptions>(a, 3));
                  }));
    bridge.handle("sftp:edit", [core](const Json& args, Bridge::Responder reply) {
        auto guard = std::make_shared<SftpCore::JobGuard>(*core);
        ThreadPool::shared().submit([core, args, reply, guard] {
            try {
                core->edit(argString(args, 0), argString(args, 1), argObject<EditOptions>(args, 2), reply);
            } catch (const std::exception& e) {
                reply.reject(e.what());
            }
        });
    });
    bridge.on("sftp:stopEdit", [core](const Json& a) { core->stopEdit(argString(a, 0)); });
    bridge.handle("sftp:startDrag", [core](const Json& a, Bridge::Responder reply) {
        try {
            auto s = core->session(argString(a, 0));
            auto source = makeRemoteDragSource(s->fs, argStringList(a, 1, false));
            dnd::startVirtualDrag(core->bridge.dispatcher(), std::move(source),
                                  [reply](const DragResult& r) { reply.resolve(Json(r)); });
        } catch (const std::exception& e) {
            reply.reject(e.what());
        }
    });
}

std::shared_ptr<dnd::VirtualSource> SftpService::dragSourceForTests(const std::string& sftpId,
                                                                   const std::vector<std::string>& paths) {
    return makeRemoteDragSource(impl_->core->session(sftpId)->fs, paths);
}

void SftpService::setEditLauncherForTests(std::function<void(const std::string&, const std::string&)> launcher) {
    impl_->core->editLauncher = std::move(launcher);
}

void SftpService::closeAll() {
    if (!impl_ || impl_->closed) return;
    impl_->closed = true;
    impl_->core->closeAll();
}

}  // namespace st
