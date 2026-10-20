#pragma once
// Pure helpers used by the SFTP service (no libssh / Win32 state; unit-tested in native/tests/sftp).
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>

namespace st::sftputil {

/** "drwxr-xr-x" style string from a POSIX st_mode (type bits + permission bits incl. setuid/setgid/sticky). */
std::string permissionsString(uint32_t mode);

/** SftpEntry.type ('file' | 'dir' | 'link' | 'other') from libssh's SSH_FILEXFER_TYPE_* falling back to mode bits. */
std::string entryType(uint8_t fileType, uint32_t mode);

/** POSIX join: ("/", "a") -> "/a", ("/x/", "a") -> "/x/a", ("", "a") -> "a", ("/x", "/abs") -> "/abs". */
std::string joinRemote(std::string_view dir, std::string_view name);

/** Last component of a POSIX path ("/a/b/" -> "b", "/" -> "/", "" -> ""). */
std::string remoteBaseName(std::string_view path);

/** Parent of a POSIX path ("/a/b" -> "/a", "/a" -> "/", "a" -> ".", "/" -> "/"). */
std::string remoteParent(std::string_view path);

/** Remote file name -> local path component valid on this OS (sys::sanitizeFileName). */
std::filesystem::path sanitizeLocalName(std::string_view utf8Name);

/** "name (n).ext" (a dot file or a name without an extension: "name (n)"). */
std::string numberedName(const std::string& name, int n);

/** The names one local folder of a download takes. Where letter case does not count (macOS, Windows), a name that
 *  differs from an earlier one only in case gets a number ("Readme (2).txt"), so that both items are kept. */
class LocalNames {
public:
    explicit LocalNames(bool caseSensitive) : caseSensitive_(caseSensitive) {}
    /** `name`, or a free variant of it; taken from now on. */
    std::string claim(const std::string& name);

private:
    bool caseSensitive_;
    std::set<std::string> taken_;  // lower case
};

/** Human readable text for an SFTP status code (SSH_FX_*). */
std::string statusText(int code);

/** "1.5 MiB"-style size (used in log lines). */
std::string humanSize(uint64_t bytes);

}  // namespace st::sftputil
