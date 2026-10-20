#pragma once
// Local file system access for the file manager's left pane: places (Windows: known folders and drives; macOS: the
// same folders and the mounted volumes), directory listings, and file operations through the OS shell (Windows:
// IFileOperation with Recycle Bin, progress and conflict UI; macOS: NSFileManager with the Trash and Finder's question
// about existing items). Everything here is blocking and meant for background threads (Windows: shell operations run
// on their own STA thread). Paths use the OS's syntax (Windows: "C:\x", "\\server\share"; macOS: "/x", "~/x").
#include "core/Types.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace st::ui::localfs {

/** Known folders (Desktop, Documents, Downloads, Home) followed by the drives / volumes, with label and free space. */
std::vector<LocalEntry> listPlaces();
/** Entries of a folder (no "." / ".."). Throws std::runtime_error with a readable message. */
std::vector<LocalEntry> list(const std::string& path);
/** nullopt when nothing exists at path. */
std::optional<LocalEntry> stat(const std::string& path);
/** The local pane's first folder: Documents (falls back to the profile folder); macOS: Home, which needs no
 *  permission. */
std::string documentsFolder();

void makeDir(const std::string& path);
/** Renames / moves within a volume; fails when the target exists (not for a change of letter case only). */
void rename(const std::string& from, const std::string& to);

struct ShellOpOptions {
    /** Parent window of the shell's progress / conflict dialogs (native handle, e.g. the HWND); may be null. */
    void* owner = nullptr;
    /** false: no progress, confirmation or error dialogs (tests, background work). */
    bool showUi = true;
};
/** Deletes files and folders (recursively), to the Recycle Bin / Trash or permanently. */
void remove(const std::vector<std::string>& paths, bool toRecycleBin, const ShellOpOptions& opts = {});
/** Copies or moves items into targetDir. With showUi the shell asks about existing targets, else they are replaced
 *  (Windows merges folders; macOS replaces them as a whole, as Finder does). Stopped by the user: no error. */
void copy(const std::vector<std::string>& paths, const std::string& targetDir, bool move,
          const ShellOpOptions& opts = {});

/** Opens with the associated application ("Open with" when there is none). */
void open(const std::string& path);
/** Opens an Explorer / Finder window with the item selected. */
void reveal(const std::string& path);

/** "c:/x/" -> "C:\x", "C:" -> "C:\"; keeps roots ("C:\", "\\server\share\") with their trailing separator.
 *  macOS: "~/x//y/" -> "$HOME/x/y". */
std::string normalizePath(const std::string& path);
/** dir + separator + name (no doubled separators). */
std::string joinPath(const std::string& dir, const std::string& name);

/** Watches one folder (not recursive). onChange runs on the watcher thread, debounced (~250 ms quiet time). */
class DirWatcher {
public:
    /** Throws when the folder cannot be opened. */
    DirWatcher(const std::string& path, std::function<void()> onChange);
    ~DirWatcher();
    DirWatcher(const DirWatcher&) = delete;
    DirWatcher& operator=(const DirWatcher&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace st::ui::localfs
