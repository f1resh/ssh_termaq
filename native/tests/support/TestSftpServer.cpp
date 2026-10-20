// The test SFTP server's protocol; its file system calls are per OS (TestSftpFiles.h).
#include "TestSftpServer.h"

#include "TestSftpFiles.h"

#include "core/Strings.h"
#include "platform/sys/Errors.h"
#include "platform/sys/Fs.h"
#include "platform/sys/Time.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace st::test {

namespace {

// SFTP v3 (draft-ietf-secsh-filexfer-02) constants.
enum : uint8_t {
    FXP_INIT = 1, FXP_VERSION = 2, FXP_OPEN = 3, FXP_CLOSE = 4, FXP_READ = 5, FXP_WRITE = 6, FXP_LSTAT = 7,
    FXP_FSTAT = 8, FXP_SETSTAT = 9, FXP_FSETSTAT = 10, FXP_OPENDIR = 11, FXP_READDIR = 12, FXP_REMOVE = 13,
    FXP_MKDIR = 14, FXP_RMDIR = 15, FXP_REALPATH = 16, FXP_STAT = 17, FXP_RENAME = 18, FXP_READLINK = 19,
    FXP_SYMLINK = 20, FXP_STATUS = 101, FXP_HANDLE = 102, FXP_DATA = 103, FXP_NAME = 104, FXP_ATTRS = 105,
    FXP_EXTENDED = 200, FXP_EXTENDED_REPLY = 201,
};
enum : uint32_t {
    FX_OK = 0, FX_EOF = 1, FX_NO_SUCH_FILE = 2, FX_PERMISSION_DENIED = 3, FX_FAILURE = 4, FX_BAD_MESSAGE = 5,
    FX_OP_UNSUPPORTED = 8,
};
enum : uint32_t { ATTR_SIZE = 0x1, ATTR_UIDGID = 0x2, ATTR_PERMISSIONS = 0x4, ATTR_ACMODTIME = 0x8, ATTR_EXTENDED = 0x80000000u };
enum : uint32_t { PF_READ = 0x1, PF_WRITE = 0x2, PF_APPEND = 0x4, PF_CREAT = 0x8, PF_TRUNC = 0x10, PF_EXCL = 0x20 };

constexpr size_t kMaxPacket = 1024 * 1024;  // generous; OpenSSH's own limit is 256 KiB
constexpr uint32_t kMaxRead = 255 * 1024;    // like OpenSSH sftp-server (SFTP_MAX_READ_LENGTH)
constexpr size_t kNamesPerReaddir = 100;
constexpr size_t kMaxHandles = 512;

struct Attrs {
    uint32_t flags = 0;
    uint64_t size = 0;
    uint32_t uid = 0, gid = 0;
    uint32_t perms = 0;
    uint32_t atime = 0, mtime = 0;
};

class Reader {
public:
    explicit Reader(std::string_view d) : d_(d) {}
    bool u8(uint8_t& v) {
        if (d_.size() - pos_ < 1) return false;
        v = static_cast<uint8_t>(d_[pos_++]);
        return true;
    }
    bool u32(uint32_t& v) {
        if (d_.size() - pos_ < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) v = (v << 8) | static_cast<uint8_t>(d_[pos_++]);
        return true;
    }
    bool u64(uint64_t& v) {
        uint32_t hi = 0, lo = 0;
        if (!u32(hi) || !u32(lo)) return false;
        v = (static_cast<uint64_t>(hi) << 32) | lo;
        return true;
    }
    bool str(std::string& v) {
        uint32_t n = 0;
        if (!u32(n) || d_.size() - pos_ < n) return false;
        v.assign(d_.substr(pos_, n));
        pos_ += n;
        return true;
    }
    bool attrs(Attrs& a) {
        if (!u32(a.flags)) return false;
        if ((a.flags & ATTR_SIZE) && !u64(a.size)) return false;
        if ((a.flags & ATTR_UIDGID) && (!u32(a.uid) || !u32(a.gid))) return false;
        if ((a.flags & ATTR_PERMISSIONS) && !u32(a.perms)) return false;
        if ((a.flags & ATTR_ACMODTIME) && (!u32(a.atime) || !u32(a.mtime))) return false;
        if (a.flags & ATTR_EXTENDED) {
            uint32_t count = 0;
            if (!u32(count)) return false;
            std::string k, v;
            for (uint32_t i = 0; i < count; ++i)
                if (!str(k) || !str(v)) return false;
        }
        return true;
    }

private:
    std::string_view d_;
    size_t pos_ = 0;
};

class Writer {
public:
    explicit Writer(uint8_t type) {
        b_.assign(4, '\0');
        b_.push_back(static_cast<char>(type));
    }
    Writer& u32(uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) b_.push_back(static_cast<char>((v >> s) & 0xFF));
        return *this;
    }
    Writer& u64(uint64_t v) { return u32(static_cast<uint32_t>(v >> 32)).u32(static_cast<uint32_t>(v)); }
    Writer& str(std::string_view s) {
        u32(static_cast<uint32_t>(s.size()));
        b_.append(s);
        return *this;
    }
    Writer& attrs(const Attrs& a) {
        u32(a.flags & ~ATTR_EXTENDED);
        if (a.flags & ATTR_SIZE) u64(a.size);
        if (a.flags & ATTR_UIDGID) u32(a.uid).u32(a.gid);
        if (a.flags & ATTR_PERMISSIONS) u32(a.perms);
        if (a.flags & ATTR_ACMODTIME) u32(a.atime).u32(a.mtime);
        return *this;
    }
    void appendTo(std::string& out) {
        const auto len = static_cast<uint32_t>(b_.size() - 4);
        for (int i = 0; i < 4; ++i) b_[static_cast<size_t>(i)] = static_cast<char>((len >> (24 - 8 * i)) & 0xFF);
        out += b_;
    }

private:
    std::string b_;
};

Attrs makeAttrs(const sftpfiles::Stat& s) {
    Attrs a;
    a.flags = ATTR_SIZE | ATTR_UIDGID | ATTR_PERMISSIONS | ATTR_ACMODTIME;
    a.size = s.isDir ? 0 : s.size;
    a.uid = a.gid = 1000;
    if (s.isDir) a.perms = 0040755;
    else a.perms = s.readOnly ? 0100444 : 0100644;
    a.atime = s.atime;
    a.mtime = s.mtime;
    return a;
}

uint32_t mapError(sftpfiles::OsError e) {
    switch (sftpfiles::classify(e)) {
        case sftpfiles::ErrorKind::NoSuchFile: return FX_NO_SUCH_FILE;
        case sftpfiles::ErrorKind::PermissionDenied: return FX_PERMISSION_DENIED;
        default: return FX_FAILURE;
    }
}

const char* statusText(uint32_t code) {
    switch (code) {
        case FX_OK: return "Success";
        case FX_EOF: return "End of file";
        case FX_NO_SUCH_FILE: return "No such file";
        case FX_PERMISSION_DENIED: return "Permission denied";
        case FX_FAILURE: return "Failure";
        case FX_BAD_MESSAGE: return "Bad message";
        case FX_OP_UNSUPPORTED: return "Operation unsupported";
        default: return "Unknown error";
    }
}

bool isReservedDeviceName(std::string_view comp) {
    std::string base = st::toLower(comp.substr(0, comp.find('.')));
    static const char* const kNames[] = {"con", "prn", "aux", "nul"};
    for (const char* n : kNames)
        if (base == n) return true;
    if (base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '0' && base[3] <= '9') return true;
    return false;
}

/** Lexically normalises an SFTP path relative to "/" (the home directory is the root). Returns false for components
 *  that could escape the root or address something other than a plain file on Windows. */
bool normalizePath(std::string_view in, std::vector<std::string>& comps) {
    comps.clear();
    std::string cur;
    bool ok = true;
    auto flush = [&] {
        if (cur.empty() || cur == ".") {
            // skip
        } else if (cur == "..") {
            if (!comps.empty()) comps.pop_back();
        } else {
            if (cur.find(':') != std::string::npos || cur.find('\0') != std::string::npos || cur.back() == '.' ||
                cur.back() == ' ' || isReservedDeviceName(cur))
                ok = false;
            comps.push_back(cur);
        }
        cur.clear();
    };
    for (char c : in) {
        if (c == '/' || c == '\\') flush();
        else cur.push_back(c);
    }
    flush();
    return ok;
}

std::string virtualPath(const std::vector<std::string>& comps) {
    if (comps.empty()) return "/";
    std::string s;
    for (const auto& c : comps) s += "/" + c;
    return s;
}

std::string longName(const std::string& name, const Attrs& a) {
    char perms[11] = "----------";
    if ((a.perms & 0170000) == 0040000) perms[0] = 'd';
    static const char kRwx[] = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i)
        if (a.perms & (1u << (8 - i))) perms[1 + i] = kRwx[i];
    std::time_t t = static_cast<std::time_t>(a.mtime);
    std::tm tmv{};
    st::sys::localTime(t, tmv);
    char date[32] = {};
    std::strftime(date, sizeof(date), "%b %d %H:%M", &tmv);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s    1 %-8s %-8s %8llu %s ", perms, "test", "test",
                  static_cast<unsigned long long>(a.size), date);
    return buf + name;
}

}  // namespace

struct SftpServer::Handle {
    bool isDir = false;
    std::filesystem::path local;
    sftpfiles::File file;
    bool append = false;
    std::vector<std::pair<std::string, Attrs>> entries;  // directory snapshot (OPENDIR)
    size_t next = 0;
};

SftpServer::SftpServer(std::filesystem::path root) {
    std::error_code ec;
    root_ = std::filesystem::absolute(root, ec).lexically_normal();
    if (ec) root_ = std::move(root);
}

SftpServer::~SftpServer() = default;

size_t SftpServer::openHandles() const { return handles_.size(); }

bool SftpServer::feed(std::string_view data, std::string& out) {
    inbuf_.append(data);
    size_t off = 0;
    while (inbuf_.size() - off >= 4) {
        const auto* p = reinterpret_cast<const unsigned char*>(inbuf_.data() + off);
        const uint32_t len = (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
        if (len == 0 || len > kMaxPacket) return false;
        if (inbuf_.size() - off - 4 < len) break;
        process(std::string_view(inbuf_).substr(off + 4, len), out);
        off += 4 + static_cast<size_t>(len);
    }
    inbuf_.erase(0, off);
    return true;
}

void SftpServer::sendStatus(std::string& out, uint32_t id, uint32_t code, std::string_view message) {
    Writer w(FXP_STATUS);
    w.u32(id).u32(code).str(message.empty() ? std::string_view(statusText(code)) : message).str("en");
    w.appendTo(out);
}

void SftpServer::process(std::string_view packet, std::string& out) {
    Reader r(packet);
    uint8_t type = 0;
    r.u8(type);
    if (type == FXP_INIT) {
        uint32_t version = 0;
        r.u32(version);
        Writer w(FXP_VERSION);
        w.u32(3).str("posix-rename@openssh.com").str("1").str("fsync@openssh.com").str("1");
        w.appendTo(out);
        initialised_ = true;
        return;
    }
    uint32_t id = 0;
    if (!r.u32(id)) return;  // not even an id: nothing we can reply to
    if (!initialised_) return sendStatus(out, id, FX_FAILURE, "SFTP session not initialised");

    const auto bad = [&] { sendStatus(out, id, FX_BAD_MESSAGE); };
    const auto fail = [&](sftpfiles::OsError err) {
        const uint32_t code = mapError(err);
        sendStatus(out, id, code, code == FX_FAILURE ? st::sys::errorMessage(err) : std::string(statusText(code)));
    };
    // Resolves a client path to (virtual components, local path); replies PERMISSION_DENIED and returns false if it
    // is not acceptable.
    std::vector<std::string> comps;
    const auto resolve = [&](const std::string& p, std::filesystem::path& local) {
        if (!normalizePath(p, comps)) {
            sendStatus(out, id, FX_PERMISSION_DENIED, "Path not allowed");
            return false;
        }
        local = root_;
        for (const auto& c : comps) local /= st::sys::toPath(c);
        return true;
    };
    const auto findHandle = [&](const std::string& h, bool wantDir) -> Handle* {
        auto it = handles_.find(h);
        if (it == handles_.end() || it->second->isDir != wantDir) return nullptr;
        return it->second.get();
    };
    const auto addHandle = [&](std::unique_ptr<Handle> h) {
        const std::string key = std::to_string(nextHandle_++);
        handles_.emplace(key, std::move(h));
        Writer w(FXP_HANDLE);
        w.u32(id).str(key);
        w.appendTo(out);
    };
    const auto applyAttrs = [&](const std::filesystem::path& local, const Attrs& a) -> sftpfiles::OsError {
        if (a.flags & ATTR_SIZE) {
            if (auto err = sftpfiles::truncate(local, a.size)) return err;
        }
        if (a.flags & ATTR_ACMODTIME) {
            if (auto err = sftpfiles::setTimes(local, a.atime, a.mtime)) return err;
        }
        if (a.flags & ATTR_PERMISSIONS) {
            if (auto err = sftpfiles::setWritable(local, (a.perms & 0200) != 0)) return err;
        }
        return 0;
    };
    const auto rename = [&](const std::string& from, const std::string& to, bool replace) {
        std::filesystem::path lf, lt;
        if (!resolve(from, lf)) return;
        const bool fromRoot = comps.empty();
        if (!resolve(to, lt)) return;
        if (fromRoot || comps.empty()) return sendStatus(out, id, FX_PERMISSION_DENIED);
        if (auto err = sftpfiles::rename(lf, lt, replace)) return fail(err);
        sendStatus(out, id, FX_OK);
    };
    const auto isDir = [](const std::filesystem::path& local) {
        sftpfiles::Stat s;
        return sftpfiles::stat(local, s) == 0 && s.isDir;
    };

    switch (type) {
        case FXP_OPEN: {
            std::string path;
            uint32_t pflags = 0;
            Attrs a;
            if (!r.str(path) || !r.u32(pflags) || !r.attrs(a)) return bad();
            std::filesystem::path local;
            if (!resolve(path, local)) return;
            if (handles_.size() >= kMaxHandles) return sendStatus(out, id, FX_FAILURE, "Too many open handles");
            if (isDir(local)) return sendStatus(out, id, FX_FAILURE, "Is a directory");
            sftpfiles::OpenFlags flags;
            flags.write = (pflags & (PF_WRITE | PF_APPEND)) != 0;
            flags.read = (pflags & PF_READ) != 0 || !flags.write;
            flags.create = (pflags & PF_CREAT) != 0;
            flags.exclusive = (pflags & PF_EXCL) != 0;
            flags.truncate = (pflags & PF_TRUNC) != 0;
            auto entry = std::make_unique<Handle>();
            if (auto err = entry->file.open(local, flags)) return fail(err);
            entry->local = local;
            entry->append = (pflags & PF_APPEND) != 0;
            return addHandle(std::move(entry));
        }
        case FXP_CLOSE: {
            std::string h;
            if (!r.str(h)) return bad();
            if (handles_.erase(h) == 0) return sendStatus(out, id, FX_FAILURE, "Invalid handle");
            return sendStatus(out, id, FX_OK);
        }
        case FXP_READ: {
            std::string h;
            uint64_t offset = 0;
            uint32_t len = 0;
            if (!r.str(h) || !r.u64(offset) || !r.u32(len)) return bad();
            Handle* fh = findHandle(h, false);
            if (!fh) return sendStatus(out, id, FX_FAILURE, "Invalid handle");
            len = std::min(len, kMaxRead);
            std::string buf(len, '\0');
            uint32_t got = 0;
            if (auto err = fh->file.read(offset, buf.data(), len, got)) return fail(err);
            if (got == 0) return sendStatus(out, id, FX_EOF);
            buf.resize(got);
            Writer w(FXP_DATA);
            w.u32(id).str(buf);
            return w.appendTo(out);
        }
        case FXP_WRITE: {
            std::string h, data;
            uint64_t offset = 0;
            if (!r.str(h) || !r.u64(offset) || !r.str(data)) return bad();
            Handle* fh = findHandle(h, false);
            if (!fh) return sendStatus(out, id, FX_FAILURE, "Invalid handle");
            if (fh->append) {
                if (auto err = fh->file.size(offset)) return fail(err);
            }
            if (auto err = fh->file.write(offset, data.data(), data.size())) return fail(err);
            return sendStatus(out, id, FX_OK);
        }
        case FXP_LSTAT:
        case FXP_STAT: {
            std::string path;
            if (!r.str(path)) return bad();
            std::filesystem::path local;
            if (!resolve(path, local)) return;
            sftpfiles::Stat s;
            if (auto err = sftpfiles::stat(local, s)) return fail(err);
            Writer w(FXP_ATTRS);
            w.u32(id).attrs(makeAttrs(s));
            return w.appendTo(out);
        }
        case FXP_FSTAT: {
            std::string h;
            if (!r.str(h)) return bad();
            auto it = handles_.find(h);
            if (it == handles_.end()) return sendStatus(out, id, FX_FAILURE, "Invalid handle");
            sftpfiles::Stat s;
            if (auto err = it->second->isDir ? sftpfiles::stat(it->second->local, s) : it->second->file.stat(s))
                return fail(err);
            Writer w(FXP_ATTRS);
            w.u32(id).attrs(makeAttrs(s));
            return w.appendTo(out);
        }
        case FXP_SETSTAT: {
            std::string path;
            Attrs a;
            if (!r.str(path) || !r.attrs(a)) return bad();
            std::filesystem::path local;
            if (!resolve(path, local)) return;
            if (auto err = applyAttrs(local, a)) return fail(err);
            return sendStatus(out, id, FX_OK);
        }
        case FXP_FSETSTAT: {
            std::string h;
            Attrs a;
            if (!r.str(h) || !r.attrs(a)) return bad();
            auto it = handles_.find(h);
            if (it == handles_.end()) return sendStatus(out, id, FX_FAILURE, "Invalid handle");
            if (auto err = applyAttrs(it->second->local, a)) return fail(err);
            return sendStatus(out, id, FX_OK);
        }
        case FXP_OPENDIR: {
            std::string path;
            if (!r.str(path)) return bad();
            std::filesystem::path local;
            if (!resolve(path, local)) return;
            if (handles_.size() >= kMaxHandles) return sendStatus(out, id, FX_FAILURE, "Too many open handles");
            sftpfiles::Stat self;
            if (auto err = sftpfiles::stat(local, self)) return fail(err);
            if (!self.isDir) return sendStatus(out, id, FX_FAILURE, "Not a directory");
            auto entry = std::make_unique<Handle>();
            entry->isDir = true;
            entry->local = local;
            std::vector<std::pair<std::string, sftpfiles::Stat>> found;
            if (auto err = sftpfiles::list(local, found)) return fail(err);
            bool sawDot = false, sawDotDot = false;
            for (const auto& [name, s] : found) {
                sawDot |= name == ".";
                sawDotDot |= name == "..";
                entry->entries.emplace_back(name, makeAttrs(s));
            }
            if (!sawDot) entry->entries.insert(entry->entries.begin(), {".", makeAttrs(self)});
            if (!sawDotDot) entry->entries.insert(entry->entries.begin() + 1, {"..", makeAttrs(self)});
            return addHandle(std::move(entry));
        }
        case FXP_READDIR: {
            std::string h;
            if (!r.str(h)) return bad();
            Handle* dh = findHandle(h, true);
            if (!dh) return sendStatus(out, id, FX_FAILURE, "Invalid handle");
            if (dh->next >= dh->entries.size()) return sendStatus(out, id, FX_EOF);
            const size_t count = std::min(kNamesPerReaddir, dh->entries.size() - dh->next);
            Writer w(FXP_NAME);
            w.u32(id).u32(static_cast<uint32_t>(count));
            for (size_t i = 0; i < count; ++i) {
                const auto& [name, a] = dh->entries[dh->next++];
                w.str(name).str(longName(name, a)).attrs(a);
            }
            return w.appendTo(out);
        }
        case FXP_REMOVE: {
            std::string path;
            if (!r.str(path)) return bad();
            std::filesystem::path local;
            if (!resolve(path, local)) return;
            if (comps.empty()) return sendStatus(out, id, FX_PERMISSION_DENIED);
            if (isDir(local)) return sendStatus(out, id, FX_FAILURE, "Is a directory");
            if (auto err = sftpfiles::removeFile(local)) return fail(err);
            return sendStatus(out, id, FX_OK);
        }
        case FXP_MKDIR: {
            std::string path;
            Attrs a;
            if (!r.str(path) || !r.attrs(a)) return bad();
            std::filesystem::path local;
            if (!resolve(path, local)) return;
            if (auto err = sftpfiles::makeDir(local)) return fail(err);
            return sendStatus(out, id, FX_OK);
        }
        case FXP_RMDIR: {
            std::string path;
            if (!r.str(path)) return bad();
            std::filesystem::path local;
            if (!resolve(path, local)) return;
            if (comps.empty()) return sendStatus(out, id, FX_PERMISSION_DENIED);
            if (auto err = sftpfiles::removeDir(local)) return fail(err);
            return sendStatus(out, id, FX_OK);
        }
        case FXP_REALPATH: {
            std::string path;
            if (!r.str(path)) return bad();
            if (!normalizePath(path, comps)) return sendStatus(out, id, FX_PERMISSION_DENIED, "Path not allowed");
            const std::string virt = virtualPath(comps);
            Writer w(FXP_NAME);
            w.u32(id).u32(1).str(virt).str(virt).attrs(Attrs{});
            return w.appendTo(out);
        }
        case FXP_RENAME: {
            std::string from, to;
            if (!r.str(from) || !r.str(to)) return bad();
            return rename(from, to, false);
        }
        case FXP_READLINK:
        case FXP_SYMLINK:
            return sendStatus(out, id, FX_OP_UNSUPPORTED, "Symbolic links are not supported");
        case FXP_EXTENDED: {
            std::string name;
            if (!r.str(name)) return bad();
            if (name == "posix-rename@openssh.com") {
                std::string from, to;
                if (!r.str(from) || !r.str(to)) return bad();
                return rename(from, to, true);
            }
            if (name == "fsync@openssh.com") {
                std::string h;
                if (!r.str(h)) return bad();
                Handle* fh = findHandle(h, false);
                if (!fh) return sendStatus(out, id, FX_FAILURE, "Invalid handle");
                if (auto err = fh->file.flush()) return fail(err);
                return sendStatus(out, id, FX_OK);
            }
            return sendStatus(out, id, FX_OP_UNSUPPORTED);
        }
        default:
            return sendStatus(out, id, FX_OP_UNSUPPORTED);
    }
}

}  // namespace st::test
