#pragma once
// Recursive name search on the server and local <-> remote folder comparison for "Synchronize".
// The matching and comparison rules are pure functions (unit-tested); the walkers do the I/O.
#include "sftp/RemoteFs.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace sc::sftpimpl {

/** Glob on one name: '*', '?', '[abc]', '[a-z]', '[!x]' / '[^x]'. ASCII case folding when !caseSensitive. */
bool globMatch(std::string_view pattern, std::string_view name, bool caseSensitive);
/** "*.log; *.txt" -> {"*.log", "*.txt"} (empty parts dropped). */
std::vector<std::string> splitMasks(std::string_view masks);

/**
 * Breadth-first search below req.root. emit() receives batches of new matches (and a final event with done=true);
 * it is called on the calling thread. Folder symlinks are never followed; unreadable sub-folders are skipped.
 */
void runFind(RemoteFs& rfs, const FindRequest& req, const std::atomic<bool>& cancel,
             const std::function<void(FindEvent&)>& emit);

/** One side of a synchronize comparison, keyed by '/'-separated path relative to the root. */
struct TreeEntry {
    bool dir = false;
    uint64_t size = 0;
    int64_t mtime = 0;  // Unix s
};
using Tree = std::map<std::string, TreeEntry>;

/** Exclude masks + hidden rule of a SyncRequest. */
struct SyncFilter {
    std::vector<std::string> fileMasks;  // match files and folders
    std::vector<std::string> dirMasks;   // "name/" masks: folders only
    bool includeHidden = true;

    explicit SyncFilter(const SyncRequest& req);
    /** False when `name` (one path component) is excluded. */
    bool accepts(std::string_view name, bool isDir) const;
};

/** Pure comparison. toleranceSeconds absorbs FAT / minute-precision timestamps. */
std::vector<SyncItem> compareTrees(const Tree& local, const Tree& remote, const SyncRequest& req, int toleranceSeconds);

/** Local folder tree (excluded / hidden entries and their contents are left out). */
Tree walkLocal(const std::filesystem::path& root, const SyncFilter& filter, const std::atomic<bool>& cancel);
/** Remote folder tree (links to files count as files; links to folders are not followed). */
Tree walkRemote(RemoteFs& rfs, const std::string& root, const SyncFilter& filter, const std::atomic<bool>& cancel);

}  // namespace sc::sftpimpl
