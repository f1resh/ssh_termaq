#pragma once
// Private to platform/: file operations whose POSIX form differs per Unix (implemented in mac/sys/FsMac.cpp).
namespace st::sys::detail {

/** Flushes `fd`'s data to the disk itself, not only to the drive's cache (macOS: F_FULLFSYNC). False on failure. */
bool syncToDisk(int fd);

}  // namespace st::sys::detail
