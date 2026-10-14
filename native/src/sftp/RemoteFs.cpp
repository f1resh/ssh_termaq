#include "sftp/RemoteFs.h"

namespace st::sftpimpl {

void RemoteFs::removeTree(const std::string& path, const std::atomic<bool>* cancel) {
    // lstat: a symlink to a folder is removed as a link, never followed.
    auto st = stat(path, false);
    if (!st) throw SftpError("Cannot delete " + quotePath(path) + ": No such file or folder");
    if (st->type != "dir") {
        unlink(path);
        return;
    }
    for (const auto& e : list(path, false, cancel)) {
        if (cancel && *cancel) throw Cancelled();
        if (e.entry.type == "dir")
            removeTree(e.entry.path, cancel);
        else
            unlink(e.entry.path);
    }
    rmdir(path);
}

}  // namespace st::sftpimpl
