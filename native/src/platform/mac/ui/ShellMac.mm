// The OS shell on macOS: URLs to their default applications and Finder with an item selected (NSWorkspace), files to
// their applications and text editors through /usr/bin/open (Launch Services; it reports a failure synchronously) -
// a file no application opens to the one the user picks, as with Finder's "Choose Application…" - the alert sound.
#import <AppKit/AppKit.h>

#include "platform/ui/Shell.h"

#include "platform/sys/Fs.h"
#include "platform/ui/Dialogs.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

extern char** environ;

namespace st::ui::shell {

namespace {

/** Starts a program with its output going nowhere; its pid, or -1 with `error`. */
pid_t start(const std::vector<std::string>& args, std::string& error) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);  // none of our descriptors
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, args[0].c_str(), &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        error = std::strerror(rc);
        return -1;
    }
    return pid;
}

int waitFor(pid_t pid) {
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/** Runs `open` with `args` and waits for it: Launch Services has found the application, or it throws. */
void openOrThrow(std::vector<std::string> args, const std::filesystem::path& file) {
    args.insert(args.begin(), "/usr/bin/open");
    std::string error;
    const pid_t pid = start(args, error);
    if (pid < 0 || waitFor(pid) != 0)
        throw std::runtime_error("Unable to open " + sys::fromPath(file) + (error.empty() ? "" : ": " + error));
}

}  // namespace

void openExternal(const std::string& target) {
    NSString* t = [NSString stringWithUTF8String:target.c_str()];
    if (!t.length) return;
    NSURL* url = [t hasPrefix:@"/"] ? [NSURL fileURLWithPath:t] : [NSURL URLWithString:t];
    if (url) [NSWorkspace.sharedWorkspace openURL:url];
}

void showInFolder(const std::filesystem::path& path) {
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:sys::fromPath(path).c_str()] ?: @"/"];
    [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[ url ]];
}

void openFile(const std::filesystem::path& file) {
    const std::string path = sys::fromPath(file);
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()] ?: @"/"];
    if ([NSWorkspace.sharedWorkspace URLForApplicationToOpenURL:url]) return openOrThrow({path}, file);
    FileDialogOptions choose;
    choose.title = "Choose an application to open “" + sys::fromPath(file.filename()) + "”";
    choose.defaultPath = "/Applications";
    choose.filters = {{"Applications", {"app"}}};
    if (const auto app = dialogs::openFile(choose)) openOrThrow({"-a", *app, path}, file);
}

void editFile(const std::filesystem::path& file, const std::string& editor) {
    if (editor.empty()) return openOrThrow({"-t", sys::fromPath(file)}, file);  // the default text editor
    // An application bundle opens through Launch Services; any other program gets the file as its argument.
    const std::filesystem::path program = sys::toPath(editor);
    std::error_code ec;
    if (program.extension() == ".app" || std::filesystem::is_directory(program, ec))
        return openOrThrow({"-a", editor, sys::fromPath(file)}, file);
    std::string error;
    const pid_t pid = start({editor, sys::fromPath(file)}, error);
    if (pid < 0) throw std::runtime_error("Unable to start " + editor + ": " + error);
    std::thread([pid] { waitFor(pid); }).detach();
}

void beep() { NSBeep(); }

}  // namespace st::ui::shell
