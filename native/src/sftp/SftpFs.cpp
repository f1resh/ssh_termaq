#include "sftp/SftpFs.h"

#include "sftp/SftpSupport.h"
#include "sftp/SftpUtil.h"

namespace sc::sftpimpl {

namespace {

class SftpFsImpl final : public RemoteFs, public std::enable_shared_from_this<SftpFsImpl> {
public:
    explicit SftpFsImpl(std::weak_ptr<Transport> transport) : transport_(std::move(transport)) {}

    void init() {
        auto self = shared_from_this();
        home_ = runOnSsh(transport_, [self](ssh_session session) {
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
            self->sftp_ = sftp;
            SshCharPtr home{sftp_canonicalize_path(sftp, ".")};
            return std::string(home ? home.get() : "/");
        });
    }

    /** fn(sftp_session) on the connection thread. */
    template <class F>
    auto run(F fn) -> std::invoke_result_t<F&, sftp_session> {
        if (lost_) throw ConnectionLost();
        if (closed_) throw SftpError("SFTP session is closed");
        auto self = shared_from_this();
        try {
            return runOnSsh(transport_, [self, fn](ssh_session) mutable {
                if (!self->sftp_) throw SftpError("SFTP session is closed");
                return fn(self->sftp_);
            });
        } catch (const ConnectionLost&) {
            lost_ = true;
            throw;
        }
    }

    /** Discards in-flight requests and closes the handle (error / cancel paths). Never throws. */
    void abortFile(const std::shared_ptr<RemoteFile>& rf) noexcept {
        try {
            bool drained = false;
            for (int i = 0; i < 200 && !drained; ++i) {
                drained = run([rf](sftp_session sftp) {
                    if (!rf->isOpen()) return true;
                    if (!rf->drainStep(sftp)) return false;
                    rf->close(sftp, false);
                    return true;
                });
            }
            if (!drained) rf->abandon();
        } catch (...) {
            rf->abandon();
        }
    }

    /* ---- RemoteFs */

    std::string protocol() const override { return "sftp"; }
    RemoteCapabilities capabilities() const override { return RemoteCapabilities{true, true}; }
    bool lost() const override { return lost_; }
    std::string home() override { return home_; }

    std::string realpath(const std::string& path) override {
        return run([path](sftp_session sftp) {
            SshCharPtr p{sftp_canonicalize_path(sftp, path.empty() ? "." : path.c_str())};
            if (!p) throwSftp(sftp, "Cannot resolve " + quotePath(path));
            return std::string(p.get());
        });
    }

    std::vector<RemoteEntry> list(const std::string& rawPath, bool statLinks, const std::atomic<bool>* cancel) override {
        const std::string path = rawPath.empty() ? "." : rawPath;
        struct State {
            sftp_dir dir = nullptr;
            bool eof = false;
            size_t linkIndex = 0;
            std::vector<RemoteEntry> entries;
        };
        auto st = std::make_shared<State>();
        run([st, path](sftp_session sftp) {
            st->dir = sftp_opendir(sftp, path.c_str());
            if (!st->dir) throwSftp(sftp, "Cannot open folder " + quotePath(path));
        });
        auto closeDir = [this, st] {
            try {
                run([st](sftp_session) {
                    if (st->dir) sftp_closedir(st->dir);
                    st->dir = nullptr;
                });
            } catch (const std::exception&) {
            }
        };
        try {
            while (!st->eof) {
                if (cancel && *cancel) throw Cancelled();
                run([st, path](sftp_session sftp) {
                    const auto start = Clock::now();
                    do {
                        AttrPtr a{sftp_readdir(sftp, st->dir)};
                        if (!a) {
                            if (!sftp_dir_eof(st->dir)) throwSftp(sftp, "Cannot list folder " + quotePath(path));
                            st->eof = true;
                            sftp_closedir(st->dir);
                            st->dir = nullptr;
                            break;
                        }
                        const std::string name = a->name ? a->name : "";
                        if (name.empty() || name == "." || name == "..") continue;
                        st->entries.push_back(RemoteEntry{toEntry(path, a.get()), a->size});
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
                run([st](sftp_session sftp) {
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

    std::optional<SftpEntry> stat(const std::string& path, bool follow) override {
        return run([path, follow](sftp_session sftp) -> std::optional<SftpEntry> {
            AttrPtr a{follow ? sftp_stat(sftp, path.c_str()) : sftp_lstat(sftp, path.c_str())};
            if (!a) {
                const int code = sftp_get_error(sftp);
                if (code == SSH_FX_NO_SUCH_FILE || code == SSH_FX_NO_SUCH_PATH) return std::nullopt;
                throwSftp(sftp, "Cannot access " + quotePath(path));
            }
            SftpEntry e = toEntry(sftputil::remoteParent(path), a.get());
            e.name = sftputil::remoteBaseName(path);
            e.path = path;
            return e;
        });
    }

    void mkdir(const std::string& path, uint32_t mode) override {
        run([path, mode](sftp_session sftp) {
            if (sftp_mkdir(sftp, path.c_str(), static_cast<mode_t>(mode)) != SSH_OK) {
                throwSftp(sftp, "Cannot create folder " + quotePath(path));
            }
        });
    }

    void rename(const std::string& from, const std::string& to) override {
        run([from, to](sftp_session sftp) {
            if (sftp_rename(sftp, from.c_str(), to.c_str()) != SSH_OK) {
                throwSftp(sftp, "Cannot rename " + quotePath(from) + " to " + quotePath(to));
            }
        });
    }

    void unlink(const std::string& path) override {
        run([path](sftp_session sftp) {
            if (sftp_unlink(sftp, path.c_str()) != SSH_OK) throwSftp(sftp, "Cannot delete " + quotePath(path));
        });
    }

    void rmdir(const std::string& path) override {
        run([path](sftp_session sftp) {
            if (sftp_rmdir(sftp, path.c_str()) != SSH_OK) {
                std::string msg = sftpErrorText(sftp, "Cannot delete folder " + quotePath(path));
                if (sftp_get_error(sftp) == SSH_FX_FAILURE) msg += " (is it empty?)";
                throw SftpError(msg);
            }
        });
    }

    /** Deletes files in bounded batches (one connection-thread step removes many files). */
    void unlinkAll(const std::vector<std::string>& files) {
        auto index = std::make_shared<size_t>(0);
        auto list = std::make_shared<std::vector<std::string>>(files);
        while (*index < list->size()) {
            run([index, list](sftp_session sftp) {
                const auto start = Clock::now();
                do {
                    const std::string& f = (*list)[*index];
                    if (sftp_unlink(sftp, f.c_str()) != SSH_OK) throwSftp(sftp, "Cannot delete " + quotePath(f));
                    ++*index;
                } while (*index < list->size() && Clock::now() - start < kStepBudget);
            });
        }
    }

    void removeTree(const std::string& path, const std::atomic<bool>* cancel) override {
        auto st = stat(path, false);
        if (!st) throw SftpError("Cannot delete " + quotePath(path) + ": No such file or folder");
        if (st->type != "dir") {
            unlink(path);
            return;
        }
        std::vector<std::string> files;
        for (const auto& e : list(path, false, cancel)) {
            if (cancel && *cancel) throw Cancelled();
            if (e.entry.type == "dir")
                removeTree(e.entry.path, cancel);
            else
                files.push_back(e.entry.path);
        }
        unlinkAll(files);
        rmdir(path);
    }

    void chmod(const std::string& path, uint32_t mode) override {
        run([path, mode](sftp_session sftp) {
            if (sftp_chmod(sftp, path.c_str(), static_cast<mode_t>(mode & 07777)) != SSH_OK) {
                throwSftp(sftp, "Cannot change permissions of " + quotePath(path));
            }
        });
    }

    void setMtime(const std::string& path, int64_t mtime) override {
        if (mtime <= 0) return;
        run([path, mtime](sftp_session sftp) {
            timeval times[2];
            times[0].tv_sec = static_cast<long>(mtime);
            times[0].tv_usec = 0;
            times[1] = times[0];
            sftp_utimes(sftp, path.c_str(), times);  // best effort
        });
    }

    std::unique_ptr<RemoteReader> openRead(const std::string& path, uint64_t offset) override;
    std::unique_ptr<RemoteWriter> openWrite(const std::string& path, const WriteSpec& spec) override;

    void close() override {
        if (closed_.exchange(true)) return;
        if (lost_) return;
        auto self = shared_from_this();
        try {
            runOnSsh(transport_, [self](ssh_session) {
                if (self->sftp_) {
                    sftp_free(self->sftp_);
                    self->sftp_ = nullptr;
                }
            });
        } catch (const std::exception&) {
            // Connection already gone: the channel died with it.
        }
    }

private:
    std::weak_ptr<Transport> transport_;
    sftp_session sftp_ = nullptr;  // SSH thread only
    std::atomic<bool> lost_{false};
    std::atomic<bool> closed_{false};
    std::string home_;
};

class SftpReader final : public RemoteReader {
public:
    SftpReader(std::shared_ptr<SftpFsImpl> fs, const std::string& path, uint64_t offset)
        : fs_(std::move(fs)), rf_(std::make_shared<RemoteFile>()) {
        fs_->run([rf = rf_, path, offset](sftp_session sftp) { rf->openRead(sftp, path, offset); });
    }
    ~SftpReader() override {
        if (rf_->isOpen()) fs_->abortFile(rf_);
    }

    uint64_t size() const override { return rf_->size(); }
    int64_t mtime() const override { return rf_->mtime(); }

    std::shared_ptr<std::string> read(bool& done) override {
        auto r = fs_->run([rf = rf_](sftp_session sftp) { return rf->readStep(sftp); });
        done = r.done;
        return r.data;
    }
    void close() override {
        fs_->run([rf = rf_](sftp_session sftp) { rf->close(sftp, false); });
    }
    void abort() noexcept override { fs_->abortFile(rf_); }

private:
    std::shared_ptr<SftpFsImpl> fs_;
    std::shared_ptr<RemoteFile> rf_;
};

class SftpWriter final : public RemoteWriter {
public:
    SftpWriter(std::shared_ptr<SftpFsImpl> fs, const std::string& path, const WriteSpec& spec)
        : fs_(std::move(fs)), rf_(std::make_shared<RemoteFile>()), path_(path) {
        fs_->run([rf = rf_, path, spec](sftp_session sftp) { rf->openWrite(sftp, path, spec.mode, spec.offset); });
    }
    ~SftpWriter() override {
        if (rf_->isOpen()) fs_->abortFile(rf_);
    }

    void write(const std::shared_ptr<std::string>& data, const ProgressFn& progress) override {
        size_t pos = 0;
        while (pos < data->size()) {
            auto r = fs_->run([rf = rf_, data, pos](sftp_session sftp) { return rf->writeStep(sftp, *data, pos); });
            pos = r.pos;
            progress(r.acked);
        }
    }
    void finish(int64_t mtime, const ProgressFn& progress) override {
        for (;;) {
            auto [allAcked, acked] = fs_->run([rf = rf_](sftp_session sftp) { return rf->flushStep(sftp); });
            progress(acked);
            if (allAcked) break;
        }
        fs_->run([rf = rf_, path = path_, mtime](sftp_session sftp) {
            rf->close(sftp, true);
            if (mtime > 0) {
                timeval times[2];
                times[0].tv_sec = static_cast<long>(mtime);
                times[0].tv_usec = 0;
                times[1] = times[0];
                sftp_utimes(sftp, path.c_str(), times);  // best effort (preserve mtime)
            }
        });
    }
    void abort() noexcept override { fs_->abortFile(rf_); }

private:
    std::shared_ptr<SftpFsImpl> fs_;
    std::shared_ptr<RemoteFile> rf_;
    std::string path_;
};

std::unique_ptr<RemoteReader> SftpFsImpl::openRead(const std::string& path, uint64_t offset) {
    return std::make_unique<SftpReader>(shared_from_this(), path, offset);
}

std::unique_ptr<RemoteWriter> SftpFsImpl::openWrite(const std::string& path, const WriteSpec& spec) {
    return std::make_unique<SftpWriter>(shared_from_this(), path, spec);
}

}  // namespace

std::shared_ptr<RemoteFs> openSftpFs(const std::weak_ptr<Transport>& transport) {
    auto fs = std::make_shared<SftpFsImpl>(transport);
    fs->init();
    return fs;
}

}  // namespace sc::sftpimpl
