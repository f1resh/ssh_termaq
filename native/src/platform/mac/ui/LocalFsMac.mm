// Local files on macOS: the file manager's left pane (Home, Desktop, Documents, Downloads and the mounted volumes;
// listings from lstat), its operations - the Trash and copy / move through NSFileManager, with Finder's question about
// items that already exist - and folder watching with FSEvents.
#import <AppKit/AppKit.h>
#include <CoreServices/CoreServices.h>

#include "platform/ui/LocalFs.h"

#include "platform/ui/Shell.h"
#include "platform/sys/Fs.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <strings.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>

namespace st::ui::localfs {

namespace {

NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }
NSURL* fileUrl(const std::string& path) { return [NSURL fileURLWithPath:ns(path)]; }
std::string str(NSString* s) { return s ? std::string(s.UTF8String) : std::string(); }
std::string errnoText(int err) { return std::strerror(err); }
std::string errorText(NSError* e) { return e ? str(e.localizedDescription) : "unknown error"; }

/** EPERM on a folder is how macOS's privacy protection (Desktop, Documents, Downloads, volumes) refuses access. */
std::string accessText(int err) {
    if (err != EPERM) return errnoText(err);
    return errnoText(err) + " (macOS may need your permission: System Settings > Privacy & Security > Files and Folders)";
}

std::string trimmed(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string baseName(const std::string& path) {
    if (path == "/") return path;
    const size_t i = path.find_last_of('/');
    return i == std::string::npos ? path : path.substr(i + 1);
}

std::string parentOf(const std::string& path) {
    const size_t i = path.find_last_of('/');
    return i == std::string::npos ? std::string() : i == 0 ? "/" : path.substr(0, i);
}

int64_t toMs(const timespec& t) { return static_cast<int64_t>(t.tv_sec) * 1000 + t.tv_nsec / 1000000; }

/** An entry from lstat; a symbolic link describes its target (`target`; null when it is broken) and is marked. */
LocalEntry fromStat(const std::string& path, const std::string& name, const struct stat& ls, const struct stat* target) {
    LocalEntry e;
    e.name = name;
    e.path = path;
    e.link = S_ISLNK(ls.st_mode);
    const struct stat& st = e.link && target ? *target : ls;
    e.type = S_ISDIR(st.st_mode) ? "dir" : "file";
    e.size = e.type == "dir" ? 0.0 : static_cast<double>(st.st_size);
    e.mtime = toMs(st.st_mtimespec);
    e.hidden = (!name.empty() && name[0] == '.') || (ls.st_flags & UF_HIDDEN);
    e.readonly = (st.st_flags & (UF_IMMUTABLE | SF_IMMUTABLE)) || !(st.st_mode & S_IWUSR);  // locked, or not writable
    return e;
}

bool exists(const std::string& path) {
    struct stat st{};
    return ::lstat(path.c_str(), &st) == 0 || errno != ENOENT;
}

/** The path as the file system spells it (letter case, symbolic links resolved); the input when it cannot. */
std::string canonical(const std::string& path) {
    const int fd = ::open(path.c_str(), O_EVTONLY | O_CLOEXEC);
    if (fd < 0) return path;
    char buf[MAXPATHLEN] = {};
    const bool ok = ::fcntl(fd, F_GETPATH, buf) == 0;
    ::close(fd);
    return ok ? std::string(buf) : path;
}

/** path is dir or below it (canonical paths). */
bool isWithin(const std::string& path, const std::string& dir) {
    if (dir == "/") return true;
    return path == dir || (path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 && path[dir.size()] == '/');
}

/** A free name in dir: "name 2.txt", "name 3.txt" (keepBoth) or "name copy.txt", "name copy 2.txt" - Finder's. */
std::string freeName(const std::string& dir, const std::string& name, bool isDir, bool copySuffix) {
    std::string stem = name, ext;
    const size_t dot = name.find_last_of('.');
    if (!isDir && dot != std::string::npos && dot > 0) {
        stem = name.substr(0, dot);
        ext = name.substr(dot);
    }
    for (int n = copySuffix ? 1 : 2;; ++n) {
        std::string candidate = stem;
        if (copySuffix) candidate += " copy";
        if (n > 1) candidate += " " + std::to_string(n);
        candidate += ext;
        if (!exists(joinPath(dir, candidate))) return candidate;
    }
}

/* ------------------------------------------------------------------------ */
/* Volumes                                                                   */
/* ------------------------------------------------------------------------ */

/** One volume's details, read on its own thread: a dead network mount can block. */
struct VolumeQuery {
    std::string path;
    bool done = false;
    std::string name;
    bool browsable = true, local = true, removable = false;
    double freeBytes = -1, totalBytes = -1;
};

void queryVolume(VolumeQuery& out) {
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:ns(out.path) isDirectory:YES];
        NSDictionary<NSURLResourceKey, id>* v = [url resourceValuesForKeys:@[
            NSURLVolumeLocalizedNameKey, NSURLVolumeIsBrowsableKey, NSURLVolumeIsLocalKey, NSURLVolumeIsRemovableKey,
            NSURLVolumeIsEjectableKey, NSURLVolumeAvailableCapacityForImportantUsageKey,
            NSURLVolumeAvailableCapacityKey, NSURLVolumeTotalCapacityKey
        ] error:nil];
        out.name = str(v[NSURLVolumeLocalizedNameKey]);
        if (NSNumber* n = v[NSURLVolumeIsBrowsableKey]) out.browsable = n.boolValue;
        if (NSNumber* n = v[NSURLVolumeIsLocalKey]) out.local = n.boolValue;
        out.removable = [v[NSURLVolumeIsRemovableKey] boolValue] || [v[NSURLVolumeIsEjectableKey] boolValue];
        // "Important usage" counts purgeable space as free, as Finder does.
        NSNumber* avail = v[NSURLVolumeAvailableCapacityForImportantUsageKey];
        if (!avail || avail.longLongValue <= 0) avail = v[NSURLVolumeAvailableCapacityKey];
        if (avail) out.freeBytes = avail.doubleValue;
        if (NSNumber* total = v[NSURLVolumeTotalCapacityKey]) out.totalBytes = total.doubleValue;
    }
}

std::vector<LocalEntry> listVolumes() {
    std::vector<std::shared_ptr<VolumeQuery>> volumes;
    @autoreleasepool {
        NSArray<NSURL*>* urls = [NSFileManager.defaultManager mountedVolumeURLsIncludingResourceValuesForKeys:@[]
                                                                      options:NSVolumeEnumerationSkipHiddenVolumes];
        for (NSURL* url in urls) {
            auto q = std::make_shared<VolumeQuery>();
            q->path = url.fileSystemRepresentation;
            volumes.push_back(std::move(q));
        }
    }
    auto m = std::make_shared<std::mutex>();
    auto cv = std::make_shared<std::condition_variable>();
    for (auto& q : volumes) {
        std::thread([q, m, cv] {
            VolumeQuery result;
            result.path = q->path;
            queryVolume(result);
            std::lock_guard lk(*m);
            result.done = true;
            *q = std::move(result);
            cv->notify_all();
        }).detach();
    }
    {
        std::unique_lock lk(*m);
        cv->wait_for(lk, std::chrono::milliseconds(1500), [&] {
            for (auto& q : volumes) {
                if (!q->done) return false;
            }
            return true;
        });
    }
    std::vector<LocalEntry> out;
    std::lock_guard lk(*m);
    for (auto& q : volumes) {
        if (q->done && !q->browsable) continue;
        LocalEntry e;
        e.type = "drive";
        e.path = q->path;
        e.name = !q->name.empty() ? q->name : q->path == "/" ? "Macintosh HD" : baseName(q->path);
        e.label = e.name;
        e.icon = !q->done || !q->local ? "network" : q->removable ? "removable" : "drive";  // still blocked: likely remote
        if (q->freeBytes >= 0 && q->totalBytes > 0) {
            e.freeBytes = q->freeBytes;
            e.totalBytes = q->totalBytes;
            e.size = e.totalBytes;
        }
        out.push_back(std::move(e));
    }
    return out;
}

/* ------------------------------------------------------------------------ */
/* Existing items                                                            */
/* ------------------------------------------------------------------------ */

enum class Conflict { Replace, KeepBoth, Stop };

/** Finder's question about an item that already exists in the target folder (a sheet on the window when there is
 *  one), with "Apply to All" while more items follow. From a background thread it waits for the answer. */
class ConflictQuestion {
public:
    explicit ConflictQuestion(const ShellOpOptions& opts) : owner_(opts.owner) {}

    Conflict ask(const std::string& name, bool move, size_t remaining) {
        if (all_) return *all_;
        const bool offerAll = remaining > 1;
        NSString* message = [NSString stringWithFormat:@"An item named “%@” already exists in this location.", ns(name)];
        NSString* info = move ? @"Do you want to replace it with the one you’re moving?"
                              : @"Do you want to replace it with the one you’re copying?";
        void* owner = owner_;
        auto answer = std::make_shared<std::promise<std::pair<Conflict, bool>>>();
        std::future<std::pair<Conflict, bool>> result = answer->get_future();
        void (^show)(void) = ^{
            NSAlert* alert = [[NSAlert alloc] init];
            alert.alertStyle = NSAlertStyleWarning;
            alert.messageText = message;
            alert.informativeText = info;
            [alert addButtonWithTitle:@"Replace"];
            [alert addButtonWithTitle:@"Stop"].keyEquivalent = @"\033";
            [alert addButtonWithTitle:@"Keep Both"];
            if (offerAll) {
                alert.showsSuppressionButton = YES;
                alert.suppressionButton.title = @"Apply to All";
            }
            void (^done)(NSModalResponse) = ^(NSModalResponse r) {
                const Conflict c = r == NSAlertFirstButtonReturn  ? Conflict::Replace
                                   : r == NSAlertThirdButtonReturn ? Conflict::KeepBoth
                                                                   : Conflict::Stop;
                answer->set_value({c, offerAll && alert.suppressionButton.state == NSControlStateValueOn});
            };
            NSWindow* window = (__bridge NSWindow*)owner;
            if (window && window.visible) {
                [alert beginSheetModalForWindow:window completionHandler:done];
            } else {
                done([alert runModal]);
            }
        };
        if (pthread_main_np()) {
            show();
        } else {
            CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, show);
            CFRunLoopWakeUp(CFRunLoopGetMain());
        }
        const auto [choice, forAll] = result.get();
        if (forAll) all_ = choice;
        return choice;
    }

private:
    void* owner_;
    std::optional<Conflict> all_;
};

/** Moves (renames, or copies and deletes across volumes) or copies src to dst, which must not exist. */
void transfer(const std::string& src, const std::string& dst, bool move) {
    NSError* error = nil;
    NSFileManager* fm = NSFileManager.defaultManager;
    const BOOL ok = move ? [fm moveItemAtURL:fileUrl(src) toURL:fileUrl(dst) error:&error]
                         : [fm copyItemAtURL:fileUrl(src) toURL:fileUrl(dst) error:&error];
    if (!ok) throw std::runtime_error(errorText(error));
}

/** transfer() onto an existing item: to a hidden name next to it first, then swapped in, so that a failure leaves the
 *  existing item alone. */
void transferReplacing(const std::string& src, const std::string& dst, bool move) {
    std::random_device rd;
    const std::string tmp = joinPath(parentOf(dst), "." + baseName(dst) + ".sshtermaq-" + std::to_string(rd() % 1000000));
    transfer(src, tmp, move);
    if (::renamex_np(tmp.c_str(), dst.c_str(), RENAME_SWAP) == 0) {
        [NSFileManager.defaultManager removeItemAtURL:fileUrl(tmp) error:nil];  // now the replaced item
        return;
    }
    // File systems without RENAME_SWAP: rename() replaces a file, or an empty folder; anything else goes first.
    if (::rename(tmp.c_str(), dst.c_str()) == 0) return;
    NSError* error = nil;
    if ([NSFileManager.defaultManager removeItemAtURL:fileUrl(dst) error:&error] && ::rename(tmp.c_str(), dst.c_str()) == 0)
        return;
    const int err = errno;
    // Undo: the moved item goes back, a copy away.
    if (move) [NSFileManager.defaultManager moveItemAtURL:fileUrl(tmp) toURL:fileUrl(src) error:nil];
    else [NSFileManager.defaultManager removeItemAtURL:fileUrl(tmp) error:nil];
    throw std::runtime_error(error ? errorText(error) : "cannot replace '" + dst + "': " + errnoText(err));
}

}  // namespace

/* ------------------------------------------------------------------------ */
/* Places and listings                                                       */
/* ------------------------------------------------------------------------ */

std::vector<LocalEntry> listPlaces() {
    std::vector<LocalEntry> out;
    @autoreleasepool {
        NSFileManager* fm = NSFileManager.defaultManager;
        auto add = [&](NSString* path, const char* icon) {
            BOOL isDir = NO;
            if (!path || ![fm fileExistsAtPath:path isDirectory:&isDir] || !isDir) return;
            LocalEntry e;
            e.type = "place";
            e.path = path.fileSystemRepresentation;
            e.name = str([fm displayNameAtPath:path]);  // localized, as in Finder's sidebar
            e.label = e.name;
            e.icon = icon;
            out.push_back(std::move(e));
        };
        auto user = [](NSSearchPathDirectory d) {
            return NSSearchPathForDirectoriesInDomains(d, NSUserDomainMask, YES).firstObject;
        };
        add(user(NSDesktopDirectory), "desktop");
        add(user(NSDocumentDirectory), "documents");
        add(user(NSDownloadsDirectory), "downloads");
        add(NSHomeDirectory(), "home");
    }
    for (auto& v : listVolumes()) out.push_back(std::move(v));
    return out;
}

std::vector<LocalEntry> list(const std::string& rawPath) {
    const std::string dir = normalizePath(rawPath);
    if (dir.empty()) throw std::runtime_error("No folder given");
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("Cannot open folder '" + dir + "': " + accessText(errno));
    DIR* d = ::fdopendir(fd);
    if (!d) {
        const int err = errno;
        ::close(fd);
        throw std::runtime_error("Cannot open folder '" + dir + "': " + accessText(err));
    }
    std::vector<LocalEntry> out;
    errno = 0;
    while (dirent* de = ::readdir(d)) {
        const std::string name(de->d_name, de->d_namlen);
        if (name != "." && name != "..") {
            struct stat ls{}, target{};
            if (::fstatat(fd, de->d_name, &ls, AT_SYMLINK_NOFOLLOW) == 0) {  // else gone meanwhile
                const bool haveTarget = S_ISLNK(ls.st_mode) && ::fstatat(fd, de->d_name, &target, 0) == 0;
                out.push_back(fromStat(joinPath(dir, name), name, ls, haveTarget ? &target : nullptr));
            }
        }
        errno = 0;
    }
    const int err = errno;
    ::closedir(d);  // and fd
    if (err) throw std::runtime_error("Cannot list folder '" + dir + "': " + accessText(err));
    return out;
}

std::optional<LocalEntry> stat(const std::string& rawPath) {
    const std::string path = normalizePath(rawPath);
    if (path.empty()) return std::nullopt;
    struct stat ls{}, target{};
    if (::lstat(path.c_str(), &ls) != 0) {
        if (errno == ENOENT || errno == ENOTDIR || errno == ENAMETOOLONG) return std::nullopt;
        throw std::runtime_error("Cannot access '" + path + "': " + accessText(errno));
    }
    const bool haveTarget = S_ISLNK(ls.st_mode) && ::stat(path.c_str(), &target) == 0;
    return fromStat(path, baseName(path), ls, haveTarget ? &target : nullptr);
}

std::string documentsFolder() {
    // Home rather than Documents: listing Documents asks for the user's permission the first time.
    @autoreleasepool {
        return NSHomeDirectory().fileSystemRepresentation;
    }
}

/* ------------------------------------------------------------------------ */
/* Operations                                                                */
/* ------------------------------------------------------------------------ */

void makeDir(const std::string& rawPath) {
    const std::string path = normalizePath(rawPath);
    if (::mkdir(path.c_str(), 0777) != 0)
        throw std::runtime_error("Cannot create folder '" + path + "': " + accessText(errno));
}

void rename(const std::string& rawFrom, const std::string& rawTo) {
    const std::string from = normalizePath(rawFrom), to = normalizePath(rawTo);
    if (::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0) return;
    int err = errno;
    struct stat a{}, b{};
    const bool targetExists = ::lstat(to.c_str(), &b) == 0;
    if (err == EEXIST && ::lstat(from.c_str(), &a) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino &&
        ::strcasecmp(from.c_str(), to.c_str()) == 0) {
        // Only the letter case changes, on a case-insensitive volume: the "existing" target is the item itself.
        if (::rename(from.c_str(), to.c_str()) == 0) return;
        err = errno;
    } else if (err == ENOTSUP) {  // file systems without RENAME_EXCL (some network volumes)
        if (targetExists) err = EEXIST;
        else if (::rename(from.c_str(), to.c_str()) == 0) return;
        else err = errno;
    }
    throw std::runtime_error("Cannot rename '" + from + "' to '" + to + "': " + accessText(err));
}

void remove(const std::vector<std::string>& paths, bool toTrash, const ShellOpOptions&) {
    @autoreleasepool {
        NSFileManager* fm = NSFileManager.defaultManager;
        for (const auto& raw : paths) {
            const std::string p = normalizePath(raw);
            NSError* error = nil;
            const BOOL ok = toTrash ? [fm trashItemAtURL:fileUrl(p) resultingItemURL:nil error:&error]
                                    : [fm removeItemAtURL:fileUrl(p) error:&error];
            if (!ok) {
                throw std::runtime_error((toTrash ? "Cannot move '" + p + "' to the Trash: " : "Cannot delete '" + p + "': ") +
                                         errorText(error));
            }
        }
    }
}

void copy(const std::vector<std::string>& paths, const std::string& rawTargetDir, bool move, const ShellOpOptions& opts) {
    if (paths.empty()) return;
    const std::string targetDir = normalizePath(rawTargetDir);
    const std::string what = move ? "Move" : "Copy";
    struct stat td{};
    if (::stat(targetDir.c_str(), &td) != 0) throw std::runtime_error(what + " failed: '" + targetDir + "': " + accessText(errno));
    if (!S_ISDIR(td.st_mode)) throw std::runtime_error(what + " failed: '" + targetDir + "' is not a folder");
    const std::string realTarget = canonical(targetDir);
    ConflictQuestion question(opts);
    for (size_t i = 0; i < paths.size(); ++i) {
        @autoreleasepool {
            const std::string src = normalizePath(paths[i]);
            const std::string name = baseName(src);
            struct stat ss{}, ds{};
            if (::lstat(src.c_str(), &ss) != 0) throw std::runtime_error(what + " failed: '" + src + "': " + accessText(errno));
            const bool isDir = S_ISDIR(ss.st_mode);
            const std::string realSrc = S_ISLNK(ss.st_mode) ? src : canonical(src);
            if (isDir && isWithin(realTarget, realSrc))
                throw std::runtime_error(what + " failed: '" + src + "' cannot go into itself");
            std::string dst = joinPath(targetDir, name);
            bool replace = false;
            if (::lstat(dst.c_str(), &ds) == 0) {
                if (ds.st_dev == ss.st_dev && ds.st_ino == ss.st_ino) {
                    if (move) continue;  // already there
                    dst = joinPath(targetDir, freeName(targetDir, name, isDir, true));  // Finder's "name copy"
                } else {
                    const Conflict c = opts.showUi ? question.ask(name, move, paths.size() - i) : Conflict::Replace;
                    if (c == Conflict::Stop) return;  // cancelled, as in the shell's dialog: no error
                    if (c == Conflict::KeepBoth) {
                        dst = joinPath(targetDir, freeName(targetDir, name, isDir, false));
                    } else {
                        if (isWithin(realSrc, canonical(dst)))
                            throw std::runtime_error(what + " failed: '" + dst + "' contains the item that replaces it");
                        replace = true;
                    }
                }
            }
            try {
                if (replace) transferReplacing(src, dst, move);
                else transfer(src, dst, move);
            } catch (const std::exception& e) {
                throw std::runtime_error(what + " failed: " + e.what());
            }
        }
    }
}

void open(const std::string& path) { shell::openFile(sys::toPath(normalizePath(path))); }

void reveal(const std::string& path) { shell::showInFolder(sys::toPath(normalizePath(path))); }

std::string normalizePath(const std::string& in) {
    std::string p = trimmed(in);
    if (p == "~" || p.rfind("~/", 0) == 0) {
        @autoreleasepool {
            p = std::string(NSHomeDirectory().fileSystemRepresentation) + p.substr(1);
        }
    }
    std::string out;
    out.reserve(p.size());
    for (char c : p) {
        if (c == '/' && !out.empty() && out.back() == '/') continue;  // "a//b" -> "a/b"
        out += c;
    }
    while (out.size() > 1 && out.back() == '/') out.pop_back();  // keeps the root "/"
    return out;
}

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    return dir.back() == '/' ? dir + name : dir + '/' + name;
}

/* ------------------------------------------------------------------------ */
/* DirWatcher                                                                */
/* ------------------------------------------------------------------------ */

// FSEvents reports folders whose contents changed (recursively); the watcher keeps those about its own folder (or one
// above it: rescans, unmounts) and calls onChange once nothing new has come for 250 ms.
struct DirWatcher::Impl {
    std::string dir;  // canonical: FSEvents reports real paths
    std::function<void()> onChange;
    dispatch_queue_t queue = nil;
    dispatch_source_t quiet = nil;
    FSEventStreamRef stream = nullptr;
    bool stopped = false;  // on queue

    void changed() {
        if (stopped) return;
        dispatch_source_set_timer(quiet, dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC), DISPATCH_TIME_FOREVER,
                                  20 * NSEC_PER_MSEC);
    }

    void fire() {
        dispatch_source_set_timer(quiet, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
        if (stopped) return;
        try {
            onChange();
        } catch (...) {
        }
    }

    static void callback(ConstFSEventStreamRef, void* info, size_t count, void* eventPaths,
                         const FSEventStreamEventFlags flags[], const FSEventStreamEventId[]) {
        auto* self = static_cast<Impl*>(info);
        auto** paths = static_cast<char**>(eventPaths);
        constexpr FSEventStreamEventFlags kDropped = kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagKernelDropped;
        constexpr FSEventStreamEventFlags kAbove =
            kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUnmount | kFSEventStreamEventFlagRootChanged;
        for (size_t i = 0; i < count; ++i) {
            std::string p = paths[i];
            while (p.size() > 1 && p.back() == '/') p.pop_back();
            if ((flags[i] & kDropped) || p == self->dir || ((flags[i] & kAbove) && isWithin(self->dir, p))) {
                self->changed();
                return;
            }
        }
    }
};

DirWatcher::DirWatcher(const std::string& path, std::function<void()> onChange) : impl_(std::make_unique<Impl>()) {
    const std::string p = normalizePath(path);
    const int fd = ::open(p.c_str(), O_EVTONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("Cannot watch '" + p + "': " + accessText(errno));
    ::close(fd);
    impl_->dir = canonical(p);
    impl_->onChange = std::move(onChange);
    impl_->queue = dispatch_queue_create("st.localfs.watch", DISPATCH_QUEUE_SERIAL);
    impl_->quiet = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, impl_->queue);
    Impl* impl = impl_.get();
    dispatch_source_set_event_handler(impl_->quiet, ^{
        impl->fire();
    });
    dispatch_source_set_timer(impl_->quiet, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
    dispatch_resume(impl_->quiet);

    FSEventStreamContext context{0, impl, nullptr, nullptr, nullptr};
    CFStringRef cfPath = CFStringCreateWithFileSystemRepresentation(nullptr, impl_->dir.c_str());
    CFArrayRef watched = CFArrayCreate(nullptr, reinterpret_cast<const void**>(&cfPath), 1, &kCFTypeArrayCallBacks);
    impl_->stream = FSEventStreamCreate(nullptr, &Impl::callback, &context, watched, kFSEventStreamEventIdSinceNow, 0.05,
                                        kFSEventStreamCreateFlagWatchRoot | kFSEventStreamCreateFlagNoDefer);
    CFRelease(watched);
    CFRelease(cfPath);
    if (impl_->stream) {
        FSEventStreamSetDispatchQueue(impl_->stream, impl_->queue);
        if (FSEventStreamStart(impl_->stream)) return;
        FSEventStreamInvalidate(impl_->stream);
        FSEventStreamRelease(impl_->stream);
        impl_->stream = nullptr;
    }
    dispatch_source_cancel(impl_->quiet);
    throw std::runtime_error("Cannot watch '" + p + "': the file system events service refused it");
}

DirWatcher::~DirWatcher() {
    if (impl_->stream) {
        FSEventStreamStop(impl_->stream);
        FSEventStreamInvalidate(impl_->stream);
    }
    // After this nothing of the watcher runs on the queue any more (a callback in progress has finished).
    Impl* impl = impl_.get();
    dispatch_sync(impl_->queue, ^{
        impl->stopped = true;
        dispatch_source_cancel(impl->quiet);
    });
    if (impl_->stream) FSEventStreamRelease(impl_->stream);
}

}  // namespace st::ui::localfs
