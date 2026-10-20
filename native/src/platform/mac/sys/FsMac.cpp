// Files on macOS: F_FULLFSYNC, Gatekeeper's quarantine attribute, and the file types that run code when opened.
#include "platform/sys/Fs.h"

#include "platform/detail/PosixFs.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

namespace fs = std::filesystem;

namespace st::sys {

namespace detail {

bool syncToDisk(int fd) {
    // fsync only reaches the drive's cache on macOS; F_FULLFSYNC asks the drive to write it out. Some file systems
    // (network, FAT) do not support it: fall back to fsync.
    if (::fcntl(fd, F_FULLFSYNC) == 0) return true;
    return ::fsync(fd) == 0;
}

}  // namespace detail

bool caseSensitiveFolder(const fs::path& dir) {
    // The download may create the folder: its nearest existing parent is on the same volume.
    fs::path p = dir;
    std::error_code ec;
    while (!p.empty() && !fs::exists(p, ec) && p.has_parent_path() && p.parent_path() != p) p = p.parent_path();
    return ::pathconf(p.c_str(), _PC_CASE_SENSITIVE) == 1;  // unknown (-1): as not, which only ever renames
}

bool isExecutableType(const fs::path& file) {
    // Opening these from Finder / NSWorkspace runs code (Terminal scripts, apps, installers, scripts for
    // AppleScript / Automator, Java archives) or follows a link.
    static const char* const kTypes[] = {".app",     ".command", ".tool",    ".sh",          ".bash",      ".zsh",
                                         ".csh",     ".ksh",     ".tcsh",    ".pkg",         ".mpkg",      ".terminal",
                                         ".workflow", ".action", ".scpt",    ".scptd",       ".applescript", ".jar",
                                         ".fileloc", ".inetloc", ".webloc",  ".prefpane",    ".osax",      ".plugin",
                                         ".bundle",  ".dylib",   ".kext",    ".mobileconfig", ".url"};
    std::string ext = file.extension().native();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* t : kTypes)
        if (ext == t) return true;
    // A file with an execute bit opens in Terminal and runs.
    struct stat st {};
    return ::stat(file.c_str(), &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
}

void markDownloaded(const fs::path& file) {
    // The attribute browsers set ("flags;hex time;agent;event id"): Gatekeeper checks the file when it is opened.
    char value[64];
    std::snprintf(value, sizeof value, "0081;%08lx;SSHTermaq;", static_cast<unsigned long>(std::time(nullptr)));
    ::setxattr(file.c_str(), "com.apple.quarantine", value, std::char_traits<char>::length(value), 0, 0);
}

}  // namespace st::sys
