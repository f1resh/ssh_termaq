#include "sftp/SftpUtil.h"

#include "core/Strings.h"
#include "platform/sys/Fs.h"

#include <array>
#include <cwctype>
#include <cstdio>

namespace st::sftputil {

namespace {

constexpr uint32_t kIfMt = 0170000;
constexpr uint32_t kIfSock = 0140000;
constexpr uint32_t kIfLnk = 0120000;
constexpr uint32_t kIfReg = 0100000;
constexpr uint32_t kIfBlk = 0060000;
constexpr uint32_t kIfDir = 0040000;
constexpr uint32_t kIfChr = 0020000;
constexpr uint32_t kIfIfo = 0010000;

// libssh SSH_FILEXFER_TYPE_* values (kept local so this file does not need libssh headers).
constexpr uint8_t kTypeRegular = 1;
constexpr uint8_t kTypeDirectory = 2;
constexpr uint8_t kTypeSymlink = 3;
constexpr uint8_t kTypeSpecial = 4;

char typeChar(uint32_t mode) {
    switch (mode & kIfMt) {
        case kIfDir: return 'd';
        case kIfLnk: return 'l';
        case kIfChr: return 'c';
        case kIfBlk: return 'b';
        case kIfIfo: return 'p';
        case kIfSock: return 's';
        default: return '-';
    }
}

}  // namespace

std::string permissionsString(uint32_t mode) {
    std::string s(10, '-');
    s[0] = typeChar(mode);
    static constexpr std::array<char, 3> rwx{'r', 'w', 'x'};
    for (int i = 0; i < 9; ++i) {
        if (mode & (0400u >> i)) s[static_cast<size_t>(i) + 1] = rwx[static_cast<size_t>(i % 3)];
    }
    // setuid / setgid / sticky replace the execute slot (lowercase when execute is also set).
    if (mode & 04000) s[3] = (mode & 0100) ? 's' : 'S';
    if (mode & 02000) s[6] = (mode & 0010) ? 's' : 'S';
    if (mode & 01000) s[9] = (mode & 0001) ? 't' : 'T';
    return s;
}

std::string entryType(uint8_t fileType, uint32_t mode) {
    switch (fileType) {
        case kTypeRegular: return "file";
        case kTypeDirectory: return "dir";
        case kTypeSymlink: return "link";
        case kTypeSpecial: return "other";
        default: break;
    }
    switch (mode & kIfMt) {
        case kIfReg: return "file";
        case kIfDir: return "dir";
        case kIfLnk: return "link";
        case 0: return "file";  // no type information at all: treat as a plain file
        default: return "other";
    }
}

std::string joinRemote(std::string_view dir, std::string_view name) {
    if (!name.empty() && name.front() == '/') return std::string(name);
    if (dir.empty()) return std::string(name);
    std::string out(dir);
    if (out.back() != '/') out.push_back('/');
    out.append(name);
    return out;
}

std::string remoteBaseName(std::string_view path) {
    while (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
    if (path == "/") return "/";
    const auto pos = path.rfind('/');
    return std::string(pos == std::string_view::npos ? path : path.substr(pos + 1));
}

std::string remoteParent(std::string_view path) {
    while (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
    const auto pos = path.rfind('/');
    if (pos == std::string_view::npos) return ".";
    if (pos == 0) return "/";
    return std::string(path.substr(0, pos));
}

std::filesystem::path sanitizeLocalName(std::string_view utf8Name) {
    return sys::toPath(sys::sanitizeFileName(utf8Name));
}

std::string statusText(int code) {
    switch (code) {
        case 0: return "OK";
        case 1: return "End of file";
        case 2: return "No such file or directory";
        case 3: return "Permission denied";
        case 4: return "Operation failed";
        case 5: return "Bad message";
        case 6: return "No connection";
        case 7: return "Connection lost";
        case 8: return "Operation not supported by the server";
        case 9: return "Invalid handle";
        case 10: return "No such path";
        case 11: return "File already exists";
        case 12: return "Write protected";
        case 13: return "No media";
        default: return "SFTP error " + std::to_string(code);
    }
}

std::string humanSize(uint64_t bytes) {
    static const char* const kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(bytes);
    size_t u = 0;
    while (v >= 1024.0 && u + 1 < std::size(kUnits)) {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    if (u == 0) {
        std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
    } else {
        std::snprintf(buf, sizeof buf, "%.1f %s", v, kUnits[u]);
    }
    return buf;
}

std::string numberedName(const std::string& name, int n) {
    const std::string suffix = " (" + std::to_string(n) + ")";
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return name + suffix;
    return name.substr(0, dot) + suffix + name.substr(dot);
}

std::string LocalNames::claim(const std::string& name) {
    if (caseSensitive_) return name;
    std::string candidate = name;
    for (int n = 2; taken_.count(toLower(candidate)); ++n) candidate = numberedName(name, n);
    taken_.insert(toLower(candidate));
    return candidate;
}

}  // namespace st::sftputil
