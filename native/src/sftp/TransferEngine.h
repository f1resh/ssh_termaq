#pragma once
// Transfer queue and execution on top of RemoteFs: one queue per remote session with parallel workers, recursive
// planning of folder transfers, conflict handling (ask / overwrite / newer / skip / resume / rename, "apply to all"),
// resume of interrupted transfers (.sc-partial files locally, offset writes remotely), move (delete the source after
// each copied file), deletes for synchronize, and throttled progress reporting.
#include "sftp/RemoteFs.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace sc::sftpimpl {

enum class ConflictDecision { Overwrite, Skip, Resume, Rename };

/**
 * Pure policy evaluation. action: 'overwrite' | 'newer' | 'skip' | 'resume' | 'rename' (not 'ask').
 * Times are Unix seconds (0 = unknown); preciseTimes=false widens the "newer" tolerance to a minute.
 */
ConflictDecision decideConflict(const std::string& action, int64_t sourceMtime, int64_t targetMtime, bool canResume,
                                bool preciseTimes);

/** "name (n).ext"; dotfiles keep their name as a whole (".bashrc" -> ".bashrc (1)"). */
std::string numberedName(const std::string& name, int n);

/** Shared by all transfers created by one request: "apply to all" answers and cancel-all. */
struct ConflictGroup {
    std::string policy = "ask";
    /** More than one file belongs to the request ("apply to all" makes sense). */
    std::atomic<bool> multiple{false};
    /** One question per request at a time. */
    std::timed_mutex askMutex;
    std::mutex m;
    std::optional<ConflictResolution> forAll;
    std::atomic<bool> cancelled{false};
};

enum class TransferKind { Download, Upload, DeleteRemote, DeleteLocal };

struct Transfer {
    std::string id;
    std::string sessionId;
    TransferKind kind = TransferKind::Download;
    /** Download: source; upload: destination (full path); deleteRemote: target. */
    std::string remotePath;
    /** Download: destination (full path); upload: source; deleteLocal: target. */
    std::filesystem::path localPath;
    TransferOptions options;
    std::shared_ptr<ConflictGroup> group;
    /** Edit uploads: the server copy should still have this mtime (Unix s); otherwise ask with reason "changed". */
    int64_t expectedRemoteMtime = 0;
    std::atomic<bool> cancel{false};
    /** Called once when finished (on a worker thread). ok == state "done". */
    std::function<void(bool ok, const std::string& error)> onFinished;

    std::mutex pm;
    TransferProgress progress;
    std::chrono::steady_clock::time_point lastEmit{};
    bool finished = false;
};
using TransferPtr = std::shared_ptr<Transfer>;

struct EngineHooks {
    std::function<void(const TransferProgress&)> progress;
    std::function<void(const TransferConflict&)> conflict;
};

class TransferEngine {
public:
    explicit TransferEngine(EngineHooks hooks);
    ~TransferEngine();
    TransferEngine(const TransferEngine&) = delete;
    TransferEngine& operator=(const TransferEngine&) = delete;

    /** Adds the queue of a remote session (parallel workers, clamped to 1..6). */
    void addQueue(const std::string& sessionId, std::shared_ptr<RemoteFs> fs, int parallel);
    /** Cancels everything of the queue and joins its workers (blocking; background threads only). */
    void removeQueue(const std::string& sessionId);

    static std::shared_ptr<ConflictGroup> newGroup(const TransferOptions& opts);

    /** New transfer in state "queued" (group null => a group of its own). Not started until enqueue(). */
    TransferPtr create(const std::string& sessionId, TransferKind kind, std::string remotePath,
                       std::filesystem::path localPath, std::string name, const TransferOptions& opts,
                       std::shared_ptr<ConflictGroup> group, std::string origin = "transfer");
    /** Queues it on its session (finishes as "cancelled" when the session is gone or closing). */
    void enqueue(const TransferPtr& t);

    void cancel(const std::string& transferId);
    void resolveConflict(const std::string& conflictId, const ConflictResolution& resolution);
    /** Shutdown: cancels everything and joins all workers. */
    void stop();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace sc::sftpimpl
