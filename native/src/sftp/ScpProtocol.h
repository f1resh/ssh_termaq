#pragma once
// Pure helpers of the SCP backend (no libssh state; unit-tested in native/tests/sftp):
//  - SCP control records ("C0644 12 name", "D0755 0 dir", "E", "T<mtime> 0 <atime> 0")
//  - POSIX shell quoting
//  - `ls -l` output parsing (GNU --full-time, BSD -T, BusyBox -e and plain formats)
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sc::scp {

struct Record {
    char type = 0;  // 'C' file, 'D' folder, 'E' end of folder, 'T' times
    uint32_t mode = 0;
    uint64_t size = 0;
    std::string name;
    int64_t mtime = 0;
    int64_t atime = 0;
};

/** Parses one control line without its trailing '\n'. nullopt + error for malformed lines. */
std::optional<Record> parseRecord(std::string_view line, std::string& error);
/** "C0644 <size> <name>\n" */
std::string fileRecord(uint32_t mode, uint64_t size, std::string_view name);
/** "T<mtime> 0 <atime> 0\n" */
std::string timeRecord(int64_t mtime, int64_t atime);

/** 'it'\''s' style single quoting for POSIX sh. */
std::string shellQuote(std::string_view s);

/* ---- ls -l parsing --------------------------------------------------------- */

struct LsEntry {
    std::string name;
    std::string linkTarget;
    /** '-' file, 'd' folder, 'l' symlink, 'c' 'b' 'p' 's' special. */
    char type = '-';
    /** Full st_mode (type bits + permissions incl. setuid/setgid/sticky). */
    uint32_t mode = 0;
    uint64_t size = 0;
    /** Unix seconds (UTC). */
    int64_t mtime = 0;
    /** The line carried seconds (GNU --full-time, BSD -T, BusyBox -e); plain `ls -l` only has minutes or days. */
    bool preciseTime = false;
    std::string owner;
    std::string group;
};

/**
 * Parses one line of `LC_ALL=C ls -l` output. nullopt for "total N" and lines that cannot be understood.
 * tzOffsetSeconds: the server's UTC offset, used for formats without a zone; nowUtc: Unix time used to infer the
 * year of "Mon dd hh:mm" dates.
 */
std::optional<LsEntry> parseLsLine(std::string_view line, int tzOffsetSeconds, int64_t nowUtc);

/** st_mode from "drwxr-sr-t" (trailing ACL / SELinux markers are ignored). */
uint32_t modeFromPermissions(std::string_view perms);

/** "+0130" / "-0500" (as printed by `date +%z`) -> seconds; 0 when unparsable. */
int parseTzOffset(std::string_view s);

/** Unix seconds of a civil UTC date/time. */
int64_t civilToUnix(int year, int month, int day, int hour, int minute, int second);

}  // namespace sc::scp
