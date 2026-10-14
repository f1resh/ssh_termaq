#include "sftp/RemoteDragSource.h"

#include "core/Log.h"
#include "core/Strings.h"
#include "sftp/SftpUtil.h"

#include <cstring>

namespace sc::sftpimpl {

namespace {

/** Paths inside a FILEDESCRIPTOR are limited to MAX_PATH - 1 characters. */
bool fits(const std::string& rel) { return widen(rel).size() < 259; }

class Reader final : public dnd::VirtualReader {
public:
    explicit Reader(std::unique_ptr<RemoteReader> r) : reader_(std::move(r)), buf_(std::make_shared<std::string>()) {}
    ~Reader() override {
        if (!finished_) reader_->abort();
    }

    size_t read(void* out, size_t length) override {
        while (pos_ >= buf_->size()) {
            if (finished_) return 0;
            bool done = false;
            buf_ = reader_->read(done);
            pos_ = 0;
            if (done) {
                finished_ = true;
                reader_->close();
            }
        }
        const size_t n = std::min(length, buf_->size() - pos_);
        std::memcpy(out, buf_->data() + pos_, n);
        pos_ += n;
        return n;
    }

private:
    std::unique_ptr<RemoteReader> reader_;
    std::shared_ptr<std::string> buf_;
    size_t pos_ = 0;
    bool finished_ = false;
};

class RemoteDragSource final : public dnd::VirtualSource {
public:
    RemoteDragSource(std::shared_ptr<RemoteFs> fs, std::vector<std::string> paths)
        : fs_(std::move(fs)), paths_(std::move(paths)) {}

    std::vector<dnd::VirtualItem> enumerate() override {
        std::vector<dnd::VirtualItem> items;
        remotes_.clear();
        for (const auto& p : paths_) {
            auto st = fs_->stat(p, true);  // a top-level link to a folder is offered as that folder
            if (!st) throw SftpError("Cannot access " + quotePath(p) + ": No such file or folder");
            const std::string name = narrow(sftputil::sanitizeLocalName(sftputil::remoteBaseName(p)));
            if (st->type == "dir")
                addDir(p, name, items);
            else if (st->type == "file")
                add(items, p, name, false, static_cast<uint64_t>(st->size), st->mtime / 1000);
        }
        return items;
    }

    std::unique_ptr<dnd::VirtualReader> open(size_t index) override {
        if (index >= remotes_.size()) throw SftpError("Invalid drag item");
        return std::make_unique<Reader>(fs_->openRead(remotes_[index]));
    }

private:
    void add(std::vector<dnd::VirtualItem>& items, const std::string& remote, const std::string& rel, bool dir,
             uint64_t size, int64_t mtime) {
        items.push_back(dnd::VirtualItem{rel, dir, size, mtime});
        remotes_.push_back(remote);
    }

    void addDir(const std::string& remote, const std::string& rel, std::vector<dnd::VirtualItem>& items) {
        if (!fits(rel)) {
            applog::warn("drag: path too long for Explorer, skipped: " + rel);
            return;
        }
        add(items, remote, rel, true, 0, 0);
        for (const auto& [e, targetSize] : fs_->list(remote, true)) {
            const std::string child = rel + "/" + narrow(sftputil::sanitizeLocalName(e.name));
            if (e.type == "dir") {
                addDir(e.path, child, items);
                continue;
            }
            uint64_t size = 0;
            if (e.type == "file")
                size = static_cast<uint64_t>(e.size);
            else if (e.type == "link" && !e.linkTargetIsDir && targetSize > 0)
                size = targetSize;
            else
                continue;  // folder links, special files, dangling links
            if (!fits(child)) {
                applog::warn("drag: path too long for Explorer, skipped: " + child);
                continue;
            }
            add(items, e.path, child, false, size, e.mtime / 1000);
        }
    }

    std::shared_ptr<RemoteFs> fs_;
    std::vector<std::string> paths_;
    std::vector<std::string> remotes_;  // parallel to the enumerated items
};

}  // namespace

std::shared_ptr<dnd::VirtualSource> makeRemoteDragSource(std::shared_ptr<RemoteFs> fs, std::vector<std::string> paths) {
    return std::make_shared<RemoteDragSource>(std::move(fs), std::move(paths));
}

}  // namespace sc::sftpimpl
