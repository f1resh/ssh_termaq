#pragma once
// Local side of "open" and "edit" for remote files: temp folders for downloaded copies and safe shell launching
// (downloaded executables are never run; files get the Mark-of-the-Web).
#include <filesystem>
#include <string>

namespace sc::sftpimpl {

/** %TEMP%\SSH Crawler */
std::filesystem::path tempRoot();
/** A new empty folder below tempRoot() (old ones are pruned once per process). */
std::filesystem::path newTempFolder();
/** File types that run code when "opened" (.exe, .bat, .ps1, .lnk, ...). */
bool isExecutableType(const std::filesystem::path& file);
/** Zone.Identifier stream (ZoneId=3) so SmartScreen / Office treat the file as downloaded from the Internet. */
void markOfTheWeb(const std::filesystem::path& file);
/** Opens with the associated application; executables are only shown in Explorer. Throws on failure. */
void shellOpen(const std::filesystem::path& file);
/**
 * Opens a file for editing: the given editor program, else the shell's "edit" verb, else the associated application
 * (non-executables), else Notepad. Throws on failure.
 */
void shellEdit(const std::filesystem::path& file, const std::string& editor);

}  // namespace sc::sftpimpl
