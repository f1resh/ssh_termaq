#pragma once
// Process and UI-thread services for the composition root (app/App.cpp): crash logging, the UI thread's setup and
// event loop, one running instance per data folder, and message boxes for when there is no main window.
#include <string>

namespace st::ui {

/** First thing in the process: logs crashes the C++ runtime does not see (Windows: SEH exceptions; macOS: Objective-C
 *  exceptions). */
void installCrashLogging();

/** Makes the calling thread the UI thread for the object's lifetime (Windows: OLE, which drag & drop needs;
 *  macOS: NSApplication and its menus). */
class UiThread {
public:
    UiThread();
    ~UiThread();
    UiThread(const UiThread&) = delete;
    UiThread& operator=(const UiThread&) = delete;

private:
    bool initialized_ = false;
};

/** Runs the UI thread's event loop until the main window is gone; returns the process exit code. */
int runEventLoop();

/**
 * One running instance per user and `key` (a short hash of the data folder: portable copies and --data-dir profiles
 * run side by side). The first process to take it is the primary instance; later ones hand their command line to it
 * with forwardToPrimary() and exit.
 */
class InstanceLock {
public:
    explicit InstanceLock(const std::string& key);
    ~InstanceLock();
    InstanceLock(const InstanceLock&) = delete;
    InstanceLock& operator=(const InstanceLock&) = delete;

    bool primary() const { return primary_; }
    /**
     * Hands this process's command line and working directory to the primary instance's main window
     * (HostWindow::onSecondInstance), which comes to the foreground. False when it cannot be reached.
     */
    static bool forwardToPrimary();

private:
    void* handle_ = nullptr;  // Windows: the named mutex; macOS: the lock file and the listening socket
    bool primary_ = false;
};

enum class MessageKind { Info, Error };
/** A message box without an owner window (startup errors, --version). Blocks until it is closed. */
void showMessage(const std::string& text, MessageKind kind);

}  // namespace st::ui
