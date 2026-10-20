#pragma once
// The OS shell: default handlers for URLs and files, the file manager, the system sound.
#include <filesystem>
#include <string>

namespace st::ui::shell {

/** Hands a URL (http, https, mailto) or a local path to its default handler; fire and forget, any thread. */
void openExternal(const std::string& target);
/** Opens a file manager window (Windows: Explorer) with `path` selected; best effort, any thread. */
void showInFolder(const std::filesystem::path& path);
/** Opens a file with its associated application ("Open with" when there is none). Throws std::runtime_error. */
void openFile(const std::filesystem::path& file);
/**
 * Opens a file for editing: the given editor program, else the OS's "edit" handler, else the associated application
 * (non-executables), else the system text editor (Windows: Notepad). Throws std::runtime_error.
 */
void editFile(const std::filesystem::path& file, const std::string& editor);
/** The system's default alert sound. */
void beep();

}  // namespace st::ui::shell
