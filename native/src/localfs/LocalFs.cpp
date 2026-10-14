#include "localfs/LocalFs.h"

#include "core/Strings.h"
#include "core/Win.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace sc::localfs {

using Microsoft::WRL::ComPtr;

namespace {

/** FILETIME (100 ns ticks since 1601) -> ms since the Unix epoch; 0 stays 0. */
int64_t fileTimeToMs(const FILETIME& ft) {
    const uint64_t ticks = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    if (ticks == 0) return 0;
    return static_cast<int64_t>(ticks / 10000) - 11644473600000LL;
}

std::string hrMessage(HRESULT hr) {
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32) return winErrorMessage(HRESULT_CODE(hr));
    return winErrorMessage(static_cast<unsigned long>(hr));
}

/** Win32 path for file APIs: long paths get the \\?\ prefix. */
std::wstring apiPath(const std::string& path) {
    std::wstring w = widen(path);
    if (w.size() < 240 || w.rfind(L"\\\\?\\", 0) == 0) return w;
    if (w.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + w.substr(2);
    return L"\\\\?\\" + w;
}

bool isLinkReparse(DWORD attrs, DWORD tag) {
    return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
           (tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT);
}

std::string baseName(const std::string& path) {
    std::string p = path;
    while (p.size() > 1 && p.back() == '\\') p.pop_back();
    const size_t i = p.find_last_of('\\');
    return i == std::string::npos ? p : p.substr(i + 1);
}

LocalEntry fromAttributes(const std::string& path, const std::string& name, DWORD attrs, DWORD sizeHigh, DWORD sizeLow,
                          const FILETIME& mtime, DWORD reparseTag) {
    LocalEntry e;
    e.name = name;
    e.path = path;
    e.type = (attrs & FILE_ATTRIBUTE_DIRECTORY) ? "dir" : "file";
    e.size = e.type == "dir" ? 0.0 : static_cast<double>((static_cast<uint64_t>(sizeHigh) << 32) | sizeLow);
    e.mtime = fileTimeToMs(mtime);
    e.hidden = (attrs & FILE_ATTRIBUTE_HIDDEN) != 0;
    e.system = (attrs & FILE_ATTRIBUTE_SYSTEM) != 0;
    e.readonly = (attrs & FILE_ATTRIBUTE_READONLY) != 0;
    e.link = isLinkReparse(attrs, reparseTag);
    return e;
}

/** Runs fn on a fresh STA thread: shell APIs need one, and pool threads may already be MTA. */
template <class F>
void runSta(F fn) {
    std::exception_ptr error;
    std::thread t([&] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        try {
            fn();
        } catch (...) {
            error = std::current_exception();
        }
        if (SUCCEEDED(hr)) CoUninitialize();
    });
    t.join();
    if (error) std::rethrow_exception(error);
}

ComPtr<IShellItem> shellItem(const std::string& path) {
    ComPtr<IShellItem> item;
    const HRESULT hr = SHCreateItemFromParsingName(widen(path).c_str(), nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr)) throw std::runtime_error("Cannot access '" + path + "': " + hrMessage(hr));
    return item;
}

/**
 * One IFileOperation over `paths`; add(op, item, dest) queues the operation for one item (dest = targetDir's shell
 * item, null when targetDir is empty). User cancellation is not an error.
 */
template <class Add>
void performShellOp(const std::vector<std::string>& paths, const std::string& targetDir, const ShellOpOptions& opts,
                    DWORD flags, const char* what, Add add) {
    runSta([&] {
        ComPtr<IShellItem> dest;
        if (!targetDir.empty()) dest = shellItem(targetDir);
        ComPtr<IFileOperation> op;
        HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op));
        if (FAILED(hr)) throw std::runtime_error(std::string(what) + " failed: " + hrMessage(hr));
        if (!opts.showUi) flags |= FOF_SILENT | FOF_NOERRORUI | FOF_NOCONFIRMATION;
        op->SetOperationFlags(flags);
        if (opts.owner) op->SetOwnerWindow(opts.owner);
        for (const auto& p : paths) {
            ComPtr<IShellItem> item = shellItem(normalizePath(p));
            hr = add(op.Get(), item.Get(), dest.Get());
            if (FAILED(hr)) throw std::runtime_error(std::string(what) + " failed for '" + p + "': " + hrMessage(hr));
        }
        hr = op->PerformOperations();
        BOOL aborted = FALSE;
        op->GetAnyOperationsAborted(&aborted);
        const bool cancelled = hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) || hr == COPYENGINE_E_USER_CANCELLED;
        if (FAILED(hr) && !cancelled) throw std::runtime_error(std::string(what) + " failed: " + hrMessage(hr));
        if (aborted && !opts.showUi) throw std::runtime_error(std::string(what) + " did not complete");
    });
}

std::string knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) && p) out = narrow(p);
    CoTaskMemFree(p);
    return out;
}

/** Localised shell display name of a folder ("Documents", "Документы", ...). */
std::string displayName(const std::string& path, const std::string& fallback) {
    SHFILEINFOW info{};
    if (SHGetFileInfoW(widen(path).c_str(), 0, &info, sizeof info, SHGFI_DISPLAYNAME) && info.szDisplayName[0]) {
        return narrow(info.szDisplayName);
    }
    return fallback;
}

struct DriveQuery {
    std::wstring root;
    std::wstring label;
    ULARGE_INTEGER free{}, total{};
    bool done = false;
    bool haveSpace = false;
};

}  // namespace

/* ------------------------------------------------------------------------ */
/* Paths                                                                     */
/* ------------------------------------------------------------------------ */

std::string normalizePath(const std::string& in) {
    std::string p = trim(in);
    if (p.rfind("\\\\?\\", 0) == 0) return p;
    for (auto& c : p) {
        if (c == '/') c = '\\';
    }
    std::string out;
    out.reserve(p.size());
    for (size_t i = 0; i < p.size(); ++i) {
        // Collapse "\\" runs, but keep the leading "\\" of UNC paths.
        if (p[i] == '\\' && !out.empty() && out.back() == '\\' && i > 1) continue;
        out += p[i];
    }
    if (out.size() >= 2 && out[1] == ':' && std::isalpha(static_cast<unsigned char>(out[0]))) {
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
        if (out.size() == 2) out += '\\';
    }
    const bool driveRoot = out.size() == 3 && out[1] == ':';
    while (!driveRoot && out.size() > 3 && out.back() == '\\') out.pop_back();
    return out;
}

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    return dir.back() == '\\' ? dir + name : dir + '\\' + name;
}

/* ------------------------------------------------------------------------ */
/* Listing                                                                   */
/* ------------------------------------------------------------------------ */

std::vector<LocalEntry> listPlaces() {
    std::vector<LocalEntry> out;
    struct Known {
        const KNOWNFOLDERID* id;
        const char* icon;
        const char* fallback;
    };
    const Known known[] = {{&FOLDERID_Desktop, "desktop", "Desktop"},
                           {&FOLDERID_Documents, "documents", "Documents"},
                           {&FOLDERID_Downloads, "downloads", "Downloads"},
                           {&FOLDERID_Profile, "home", "Home"}};
    for (const auto& k : known) {
        const std::string path = knownFolder(*k.id);
        if (path.empty() || GetFileAttributesW(widen(path).c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        LocalEntry e;
        e.type = "place";
        e.path = path;
        e.name = displayName(path, k.fallback);
        e.label = e.name;
        e.icon = k.icon;
        out.push_back(std::move(e));
    }

    // Volume label and free space can block (empty card readers, disconnected network drives): query every drive
    // on its own thread and give up on stragglers after a deadline.
    std::vector<std::shared_ptr<DriveQuery>> drives;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        auto q = std::make_shared<DriveQuery>();
        q->root = std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L":\\";
        drives.push_back(q);
    }
    auto m = std::make_shared<std::mutex>();
    auto cv = std::make_shared<std::condition_variable>();
    for (auto& q : drives) {
        std::thread([q, m, cv] {
            DWORD oldMode = 0;
            SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &oldMode);
            wchar_t label[MAX_PATH + 1] = {};
            std::wstring l;
            if (GetVolumeInformationW(q->root.c_str(), label, MAX_PATH + 1, nullptr, nullptr, nullptr, nullptr, 0)) {
                l = label;
            }
            ULARGE_INTEGER freeBytes{}, total{};
            const bool space = GetDiskFreeSpaceExW(q->root.c_str(), &freeBytes, &total, nullptr) != 0;
            std::lock_guard lk(*m);
            q->label = l;
            q->free = freeBytes;
            q->total = total;
            q->haveSpace = space;
            q->done = true;
            cv->notify_all();
        }).detach();
    }
    {
        std::unique_lock lk(*m);
        cv->wait_for(lk, std::chrono::milliseconds(1500), [&] {
            for (auto& q : drives) {
                if (!q->done) return false;
            }
            return true;
        });
    }
    std::lock_guard lk(*m);
    for (auto& q : drives) {
        const UINT type = GetDriveTypeW(q->root.c_str());
        if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN) continue;
        LocalEntry e;
        e.type = "drive";
        e.path = narrow(q->root);
        const char* fallback = "Local Disk";
        switch (type) {
            case DRIVE_REMOVABLE:
                e.icon = "removable";
                fallback = "USB Drive";
                break;
            case DRIVE_REMOTE:
                e.icon = "network";
                fallback = "Network Drive";
                break;
            case DRIVE_CDROM:
                e.icon = "cdrom";
                fallback = "CD Drive";
                break;
            default: e.icon = "drive"; break;
        }
        e.label = narrow(q->label);
        e.name = (e.label.empty() ? std::string(fallback) : e.label) + " (" + e.path.substr(0, 2) + ")";
        if (q->haveSpace) {
            e.freeBytes = static_cast<double>(q->free.QuadPart);
            e.totalBytes = static_cast<double>(q->total.QuadPart);
            e.size = e.totalBytes;
        }
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<LocalEntry> list(const std::string& rawPath) {
    const std::string dir = normalizePath(rawPath);
    if (dir.empty()) throw std::runtime_error("No folder given");
    std::wstring pattern = apiPath(dir);
    if (pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) return {};  // e.g. an empty drive root
        throw std::runtime_error("Cannot open folder '" + dir + "': " + winErrorMessage(err));
    }
    std::vector<LocalEntry> out;
    do {
        const std::wstring_view n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        const std::string name = narrow(n);
        out.push_back(fromAttributes(joinPath(dir, name), name, fd.dwFileAttributes, fd.nFileSizeHigh, fd.nFileSizeLow,
                                     fd.ftLastWriteTime, fd.dwReserved0));
    } while (FindNextFileW(h, &fd));
    const DWORD err = GetLastError();
    FindClose(h);
    if (err != ERROR_NO_MORE_FILES) throw std::runtime_error("Cannot list folder '" + dir + "': " + winErrorMessage(err));
    return out;
}

std::optional<LocalEntry> stat(const std::string& rawPath) {
    const std::string path = normalizePath(rawPath);
    if (path.empty()) return std::nullopt;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(apiPath(path).c_str(), GetFileExInfoStandard, &fad)) {
        const DWORD err = GetLastError();
        switch (err) {
            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
            case ERROR_INVALID_NAME:
            case ERROR_BAD_NETPATH:
            case ERROR_BAD_NET_NAME:
            case ERROR_NOT_READY:
            case ERROR_INVALID_DRIVE: return std::nullopt;
            default: throw std::runtime_error("Cannot access '" + path + "': " + winErrorMessage(err));
        }
    }
    DWORD tag = 0;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileExW(apiPath(path).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h != INVALID_HANDLE_VALUE) {
            tag = fd.dwReserved0;
            FindClose(h);
        }
    }
    const bool root = path.size() == 3 && path[1] == ':';
    return fromAttributes(path, root ? path.substr(0, 2) : baseName(path), fad.dwFileAttributes, fad.nFileSizeHigh,
                          fad.nFileSizeLow, fad.ftLastWriteTime, tag);
}

std::string documentsFolder() {
    std::string p = knownFolder(FOLDERID_Documents);
    if (p.empty()) p = knownFolder(FOLDERID_Profile);
    return p;
}

/* ------------------------------------------------------------------------ */
/* Operations                                                                */
/* ------------------------------------------------------------------------ */

void makeDir(const std::string& rawPath) {
    const std::string path = normalizePath(rawPath);
    if (!CreateDirectoryW(apiPath(path).c_str(), nullptr)) {
        throw std::runtime_error("Cannot create folder '" + path + "': " + lastErrorMessage());
    }
}

void rename(const std::string& rawFrom, const std::string& rawTo) {
    const std::string from = normalizePath(rawFrom), to = normalizePath(rawTo);
    if (!MoveFileExW(apiPath(from).c_str(), apiPath(to).c_str(), 0)) {
        throw std::runtime_error("Cannot rename '" + from + "' to '" + to + "': " + lastErrorMessage());
    }
}

void remove(const std::vector<std::string>& paths, bool toRecycleBin, const ShellOpOptions& opts) {
    if (paths.empty()) return;
    DWORD flags = FOF_NOCONFIRMATION | FOFX_EARLYFAILURE;  // the UI has already asked
    if (toRecycleBin) flags |= FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE;
    performShellOp(paths, std::string(), opts, flags, "Delete",
                   [](IFileOperation* op, IShellItem* item, IShellItem*) { return op->DeleteItem(item, nullptr); });
}

void copy(const std::vector<std::string>& paths, const std::string& targetDir, bool move, const ShellOpOptions& opts) {
    if (paths.empty()) return;
    performShellOp(paths, normalizePath(targetDir), opts, FOF_NOCONFIRMMKDIR | FOF_ALLOWUNDO, move ? "Move" : "Copy",
                   [move](IFileOperation* op, IShellItem* item, IShellItem* dest) {
                       return move ? op->MoveItem(item, dest, nullptr, nullptr) : op->CopyItem(item, dest, nullptr, nullptr);
                   });
}

void open(const std::string& rawPath) {
    const std::wstring path = widen(normalizePath(rawPath));
    runSta([&] {
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof sei;
        sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        sei.lpFile = path.c_str();
        sei.nShow = SW_SHOWNORMAL;
        BOOL ok = ShellExecuteExW(&sei);
        if (!ok && GetLastError() == ERROR_NO_ASSOCIATION) {
            sei.lpVerb = L"openas";  // no default application: let the user pick one
            ok = ShellExecuteExW(&sei);
        }
        if (!ok) throw std::runtime_error("Cannot open '" + narrow(path) + "': " + lastErrorMessage());
    });
}

void reveal(const std::string& rawPath) {
    const std::wstring path = widen(normalizePath(rawPath));
    runSta([&] {
        PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
        if (!pidl) throw std::runtime_error("Cannot show '" + narrow(path) + "' in Explorer");
        const HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
        if (FAILED(hr)) throw std::runtime_error("Cannot show '" + narrow(path) + "' in Explorer: " + hrMessage(hr));
    });
}

/* ------------------------------------------------------------------------ */
/* DirWatcher                                                                */
/* ------------------------------------------------------------------------ */

struct DirWatcher::Impl {
    HANDLE dir = INVALID_HANDLE_VALUE;
    HANDLE stop = nullptr;
    HANDLE ready = nullptr;
    std::function<void()> onChange;
    std::thread thread;

    void run() {
        constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                                  FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_ATTRIBUTES;
        alignas(DWORD) char buffer[16384];
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool armed = false;
        bool pending = false;
        auto fire = [this] {
            try {
                onChange();
            } catch (...) {
            }
        };
        for (;;) {
            if (!armed) {
                ResetEvent(ov.hEvent);
                if (!ReadDirectoryChangesW(dir, buffer, sizeof buffer, FALSE, kFilter, nullptr, &ov, nullptr)) {
                    if (pending) fire();
                    break;  // folder gone / handle invalid
                }
                armed = true;
                if (ready) SetEvent(ready);
            }
            HANDLE handles[2] = {ov.hEvent, stop};
            const DWORD w = WaitForMultipleObjects(2, handles, FALSE, pending ? 250 : INFINITE);
            if (w == WAIT_OBJECT_0) {
                DWORD bytes = 0;
                const BOOL ok = GetOverlappedResult(dir, &ov, &bytes, FALSE);
                armed = false;
                pending = true;  // also on overflow (bytes == 0) or errors: let the UI re-list
                if (!ok && GetLastError() == ERROR_OPERATION_ABORTED) break;
                continue;
            }
            if (w == WAIT_TIMEOUT) {
                pending = false;
                fire();
                continue;
            }
            break;  // stop requested (or wait failure)
        }
        if (armed) {
            CancelIoEx(dir, &ov);
            DWORD bytes = 0;
            GetOverlappedResult(dir, &ov, &bytes, TRUE);
        }
        CloseHandle(ov.hEvent);
    }
};

DirWatcher::DirWatcher(const std::string& path, std::function<void()> onChange) : impl_(std::make_unique<Impl>()) {
    const std::string p = normalizePath(path);
    impl_->dir = CreateFileW(apiPath(p).c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (impl_->dir == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot watch '" + p + "': " + lastErrorMessage());
    impl_->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl_->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl_->onChange = std::move(onChange);
    impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
    // Changes made right after construction must not be missed: wait until the first read is armed.
    WaitForSingleObject(impl_->ready, 2000);
}

DirWatcher::~DirWatcher() {
    if (impl_->stop) SetEvent(impl_->stop);
    if (impl_->thread.joinable()) impl_->thread.join();
    if (impl_->dir != INVALID_HANDLE_VALUE) CloseHandle(impl_->dir);
    if (impl_->stop) CloseHandle(impl_->stop);
    if (impl_->ready) CloseHandle(impl_->ready);
}

}  // namespace sc::localfs
