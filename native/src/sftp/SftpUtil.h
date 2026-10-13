#pragma once
// Pure helpers used by the SFTP service (no libssh / Win32 state; unit-tested in native/tests/sftp).
#include <cstdint>
#include <string>
#include <string_view>

namespace sc::sftputil {

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

/**
 * Remote file name -> safe Windows file name: characters invalid on NTFS (<>:"/\|?* and controls) become '_',
 * trailing dots/spaces are trimmed, reserved device names (CON, NUL, COM1, ...) get a leading '_'.
 */
std::wstring sanitizeLocalName(std::string_view utf8Name);

/** Human readable text for an SFTP status code (SSH_FX_*). */
std::string statusText(int code);

/** "1.5 MiB"-style size (used in log lines). */
std::string humanSize(uint64_t bytes);

}  // namespace sc::sftputil
