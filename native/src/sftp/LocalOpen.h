#pragma once
// Local side of "open" and "edit" for remote files: temp folders for downloaded copies and safe shell launching
// (downloaded executables are never run; files are marked as downloaded, sys::markDownloaded).
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace st::sftpimpl {

/** <temp folder>/SSHTermaq (%TEMP% on Windows, $TMPDIR on macOS); see chooseTempFolder. */
std::filesystem::path tempRoot();
/**
 * The temp folder tempRoot() uses: `systemTemp` (std::filesystem::temp_directory_path(), "" when it failed, e.g.
 * because %TEMP% names a folder that does not exist), else the first absolute path in TMPDIR, TMP or TEMP (`env`
 * reads a variable; the folder is created later, as Windows' GetTempPath allowed), else `home`. "" when none.
 */
std::filesystem::path chooseTempFolder(const std::filesystem::path& systemTemp,
                                       const std::function<std::optional<std::string>(const std::string&)>& env,
                                       const std::filesystem::path& home);
/** A new empty folder below tempRoot() (old ones are pruned once per process). */
std::filesystem::path newTempFolder();
/** Opens with the associated application; executables are only shown in the file manager. Throws on failure. */
void shellOpen(const std::filesystem::path& file);
/**
 * Opens a file for editing: the given editor program, else the shell's "edit" verb, else the associated application
 * (non-executables), else the system text editor (ui::shell::editFile). Throws on failure.
 */
void shellEdit(const std::filesystem::path& file, const std::string& editor);

}  // namespace st::sftpimpl
