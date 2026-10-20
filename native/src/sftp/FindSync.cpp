#include "sftp/FindSync.h"

#include "core/Strings.h"
#include "platform/sys/Fs.h"
#include "sftp/SftpSupport.h"
#include "sftp/SftpUtil.h"

#include <cctype>
#include <chrono>
#include <deque>
#include <set>

namespace st::sftpimpl {

namespace fs = std::filesystem;

namespace {

char fold(char c, bool caseSensitive) {
    return caseSensitive ? c : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

/**
 * Character class at p[i] == '['. Returns false when it is not a valid class (no closing ']'): the caller treats '['
 * literally. Otherwise `matched` tells whether ch is in the class and `end` is the index after ']'.
 */
bool matchClass(std::string_view p, size_t i, char ch, bool cs, size_t& end, bool& matched) {
    size_t j = i + 1;
    bool negate = false;
    if (j < p.size() && (p[j] == '!' || p[j] == '^')) {
        negate = true;
        ++j;
    }
    const size_t first = j;
    bool hit = false;
    const char c = fold(ch, cs);
    while (j < p.size() && (p[j] != ']' || j == first)) {
        if (j + 2 < p.size() && p[j + 1] == '-' && p[j + 2] != ']') {
            const char lo = fold(p[j], cs), hi = fold(p[j + 2], cs);
            if (c >= lo && c <= hi) hit = true;
            j += 3;
        } else {
            if (fold(p[j], cs) == c) hit = true;
            ++j;
        }
    }
    if (j >= p.size()) return false;
    end = j + 1;
    matched = hit != negate;
    return true;
}

bool hidden(std::string_view name) { return !name.empty() && name[0] == '.'; }

std::string u8(const fs::path& p) { return sys::fromPath(p); }

/** True when an ancestor folder of rel ("a/b/c" -> "a", "a/b") is in the set. */
bool underAny(const std::string& rel, const std::set<std::string>& dirs) {
    if (dirs.empty()) return false;
    for (size_t pos = rel.find('/'); pos != std::string::npos; pos = rel.find('/', pos + 1)) {
        if (dirs.count(rel.substr(0, pos))) return true;
    }
    return false;
}

}  // namespace

/* ------------------------------------------------------------------------ */
/* Find                                                                      */
/* ------------------------------------------------------------------------ */

bool globMatch(std::string_view p, std::string_view s, bool cs) {
    size_t pi = 0, si = 0;
    size_t starP = std::string_view::npos, starS = 0;
    while (si < s.size()) {
        bool advanced = false;
        if (pi < p.size()) {
            const char c = p[pi];
            if (c == '*') {
                starP = pi++;
                starS = si;
                continue;
            }
            if (c == '?') {
                ++pi;
                ++si;
                continue;
            }
            size_t end = 0;
            bool matched = false;
            if (c == '[' && matchClass(p, pi, s[si], cs, end, matched)) {
                if (matched) {
                    pi = end;
                    ++si;
                    advanced = true;
                }
            } else if (fold(c, cs) == fold(s[si], cs)) {
                ++pi;
                ++si;
                advanced = true;
            }
        }
        if (advanced) continue;
        if (starP == std::string_view::npos) return false;
        pi = starP + 1;
        si = ++starS;
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

std::vector<std::string> splitMasks(std::string_view masks) {
    std::vector<std::string> out;
    for (const auto& part : split(masks, ';')) {
        std::string t = trim(part);
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

void runFind(RemoteFs& rfs, const FindRequest& req, const std::atomic<bool>& cancel,
             const std::function<void(FindEvent&)>& emit) {
    std::vector<std::string> masks = splitMasks(req.pattern);
    if (masks.empty()) masks.push_back("*");
    const size_t maxResults = req.maxResults > 0 ? static_cast<size_t>(req.maxResults) : 10000;
    const std::string root = req.root.empty() ? rfs.home() : req.root;

    FindEvent ev;
    size_t found = 0;
    auto lastEmit = std::chrono::steady_clock::now();
    std::deque<std::string> dirs{root};
    bool first = true;
    while (!dirs.empty() && !cancel && !ev.truncated) {
        const std::string dir = dirs.front();
        dirs.pop_front();
        std::vector<RemoteEntry> entries;
        try {
            entries = rfs.list(dir, false, &cancel);
        } catch (const Cancelled&) {
            break;
        } catch (const ConnectionLost&) {
            throw;
        } catch (const SftpError& e) {
            if (first) {
                ev.error = e.what();
                break;
            }
            continue;  // unreadable sub-folder
        }
        first = false;
        ++ev.scannedDirs;
        ev.currentDir = dir;
        for (const auto& re : entries) {
            const SftpEntry& e = re.entry;
            if (!req.includeHidden && hidden(e.name)) continue;
            bool match = false;
            for (const auto& m : masks) {
                if (globMatch(m, e.name, req.caseSensitive)) {
                    match = true;
                    break;
                }
            }
            if (match) {
                ev.results.push_back(e);
                if (++found >= maxResults) {
                    ev.truncated = true;
                    break;
                }
            }
            if (e.type == "dir") dirs.push_back(e.path);
        }
        const auto now = std::chrono::steady_clock::now();
        if (ev.results.size() >= 200 || now - lastEmit >= std::chrono::milliseconds(300)) {
            emit(ev);
            ev.results.clear();
            lastEmit = now;
        }
    }
    ev.done = true;
    emit(ev);
}

/* ------------------------------------------------------------------------ */
/* Synchronize                                                               */
/* ------------------------------------------------------------------------ */

SyncFilter::SyncFilter(const SyncRequest& req) : includeHidden(req.includeHidden) {
    for (auto& m : splitMasks(req.exclude)) {
        if (m.back() == '/' || m.back() == '\\') {
            m.pop_back();
            if (!m.empty()) dirMasks.push_back(m);
        } else {
            fileMasks.push_back(m);
        }
    }
}

bool SyncFilter::accepts(std::string_view name, bool isDir) const {
    if (!includeHidden && hidden(name)) return false;
    for (const auto& m : fileMasks)
        if (globMatch(m, name, false)) return false;
    if (isDir) {
        for (const auto& m : dirMasks)
            if (globMatch(m, name, false)) return false;
    }
    return true;
}

std::vector<SyncItem> compareTrees(const Tree& local, const Tree& remote, const SyncRequest& req, int tol) {
    std::vector<SyncItem> out;
    const bool up = req.direction == "upload", down = req.direction == "download", both = req.direction == "both";
    std::set<std::string> handled;  // folders copied / deleted as a whole, or left alone (type clash)
    auto sideOf = [](const TreeEntry& e) { return SyncSide{static_cast<double>(e.size), e.mtime * 1000}; };
    auto add = [&](const std::string& rel, bool isDir, const char* action, const char* reason, const TreeEntry* l,
                   const TreeEntry* r) {
        SyncItem item;
        item.relPath = rel;
        item.isDir = isDir;
        item.action = action;
        item.reason = reason;
        if (l) item.local = sideOf(*l);
        if (r) item.remote = sideOf(*r);
        out.push_back(std::move(item));
        if (isDir) handled.insert(rel);
    };

    auto li = local.begin();
    auto ri = remote.begin();
    while (li != local.end() || ri != remote.end()) {
        const TreeEntry* l = nullptr;
        const TreeEntry* r = nullptr;
        std::string rel;
        if (ri == remote.end() || (li != local.end() && li->first < ri->first)) {
            rel = li->first;
            l = &li->second;
            ++li;
        } else if (li == local.end() || ri->first < li->first) {
            rel = ri->first;
            r = &ri->second;
            ++ri;
        } else {
            rel = li->first;
            l = &li->second;
            r = &ri->second;
            ++li;
            ++ri;
        }
        if (underAny(rel, handled)) continue;

        if (l && r) {
            if (l->dir && r->dir) continue;  // folders merge; their contents are compared one by one
            if (l->dir != r->dir) {
                handled.insert(rel);  // a file where a folder is: left alone
                continue;
            }
            const int64_t diff = l->mtime - r->mtime;
            const bool localNewer = diff > tol, remoteNewer = -diff > tol;
            const bool timeDiffers = req.compareTime && (localNewer || remoteNewer);
            const bool sizeDiffers = req.compareSize && l->size != r->size;
            if (!timeDiffers && !sizeDiffers) continue;
            if (both) {
                if (!req.compareTime || !timeDiffers) continue;  // same age, different size: cannot tell which wins
                add(rel, false, localNewer ? "upload" : "download", "newer", l, r);
            } else if (up) {
                add(rel, false, "upload", timeDiffers ? (localNewer ? "newer" : "older") : "size", l, r);
            } else if (down) {
                add(rel, false, "download", timeDiffers ? (remoteNewer ? "newer" : "older") : "size", l, r);
            }
            continue;
        }
        if (l) {
            if (up || both)
                add(rel, l->dir, "upload", "new", l, nullptr);
            else if (down && req.deleteExtra)
                add(rel, l->dir, "deleteLocal", "extra", l, nullptr);
            continue;
        }
        if (down || both)
            add(rel, r->dir, "download", "new", nullptr, r);
        else if (up && req.deleteExtra)
            add(rel, r->dir, "deleteRemote", "extra", nullptr, r);
    }
    return out;
}

Tree walkLocal(const fs::path& root, const SyncFilter& filter, const std::atomic<bool>& cancel) {
    Tree tree;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) throw SftpError("'" + u8(root) + "' is not a folder");
    fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end;
    if (ec) throw SftpError("Cannot read folder '" + u8(root) + "': " + ec.message());
    for (; it != end; it.increment(ec)) {
        if (ec) throw SftpError("Cannot read folder '" + u8(it->path().parent_path()) + "': " + ec.message());
        if (cancel) throw Cancelled();
        const fs::path& p = it->path();
        const auto info = sys::fileInfo(p);
        if (!info) continue;
        const bool isDir = info->isDir;
        const bool isLink = info->isLink;
        const std::string name = u8(p.filename());
        if (!filter.accepts(name, isDir) || (isDir && isLink)) {
            if (isDir) it.disable_recursion_pending();  // excluded folders and folder links: skip their contents
            continue;
        }
        TreeEntry e;
        e.dir = isDir;
        if (!isDir) {
            e.size = info->size;
            e.mtime = info->mtime;
        }
        tree[sys::fromGenericPath(p.lexically_relative(root))] = e;
    }
    return tree;
}

Tree walkRemote(RemoteFs& rfs, const std::string& root, const SyncFilter& filter, const std::atomic<bool>& cancel) {
    Tree tree;
    std::deque<std::pair<std::string, std::string>> dirs{{root, ""}};
    while (!dirs.empty()) {
        if (cancel) throw Cancelled();
        auto [dir, rel] = dirs.front();
        dirs.pop_front();
        for (const auto& [e, targetSize] : rfs.list(dir, true, &cancel)) {
            const bool isDir = e.type == "dir";
            if (!filter.accepts(e.name, isDir)) continue;
            const std::string childRel = rel.empty() ? e.name : rel + "/" + e.name;
            if (isDir) {
                tree[childRel] = TreeEntry{true, 0, 0};
                dirs.emplace_back(e.path, childRel);
            } else if (e.type == "file") {
                tree[childRel] = TreeEntry{false, static_cast<uint64_t>(e.size), e.mtime / 1000};
            } else if (e.type == "link" && !e.linkTargetIsDir && targetSize > 0) {
                tree[childRel] = TreeEntry{false, targetSize, e.mtime / 1000};
            }
        }
    }
    return tree;
}

}  // namespace st::sftpimpl
