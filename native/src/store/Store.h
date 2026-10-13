#pragma once
// Persistent application data under the data directory:
//   sessions.json     { "sessions": [SessionConfig JSON...], "folders": ["a", "a/b"] }
//   settings.json     AppSettings JSON (UI-owned shape; host deep-merges patches)
//   state.json        { key: any } (UI workspace state, window placement, ...)
//   known_hosts.json  see KnownHosts
//   credentials.json  see Credentials (DPAPI-encrypted)
// All methods are thread-safe. Writes are atomic (write temp file + MoveFileEx REPLACE_EXISTING) and
// happen synchronously on the calling thread.
// Sessions are stored as JSON documents (the UI's exact shape, so fields unknown to C++ survive);
// typed SessionConfig views are parsed on demand. proxy.password is encrypted at rest (DPAPI) and
// transparently decrypted on read.
#include "core/Json.h"
#include "core/Types.h"
#include "store/Credentials.h"
#include "store/KnownHosts.h"

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace sc {

class Store {
public:
    explicit Store(std::filesystem::path dataDir);
    ~Store();

    /** Loads all files (missing/corrupt files => defaults; corrupt files are backed up as *.corrupt-<ts>). */
    void load();

    const std::filesystem::path& dataDir() const { return dir_; }

    /* ---- sessions ---- */
    Json listSessionsJson() const;
    std::vector<SessionConfig> listSessions() const;
    std::optional<SessionConfig> getSession(const std::string& id) const;
    std::optional<Json> getSessionJson(const std::string& id) const;
    /** Insert or update by id (generates id if empty). Sets updatedAt (and createdAt for new). Returns stored JSON. */
    Json saveSession(const Json& session);
    SessionConfig saveSession(const SessionConfig& session);
    void saveSessions(const Json& sessionsArray);
    void deleteSessions(const std::vector<std::string>& ids);
    /** Explicit folders + folders implied by sessions (and all their parents), sorted. */
    std::vector<std::string> folders() const;
    void createFolder(const std::string& path);
    void renameFolder(const std::string& from, const std::string& to);
    void deleteFolder(const std::string& path, bool deleteSessions);
    /** Updates lastUsedAt (does not fire onSessionsChanged). */
    void touchSession(const std::string& id);
    /** True when a session with this name exists in this folder (case-insensitive). */
    bool sessionNameExists(const std::string& folder, const std::string& name) const;
    /**
     * Saved session by name, compared case-insensitively (PuTTY "-load"). A name containing '/' may also match
     * "<folder>/<name>". When several sessions share the name, the one in the root folder (or the first) wins.
     */
    std::optional<SessionConfig> findSessionByName(const std::string& name) const;

    /* ---- settings ---- */
    Json settings() const;
    /** Deep merge (arrays replace). Returns the full new settings. */
    Json updateSettings(const Json& patch);
    /** settings.defaultSession parsed (or built-in defaults). */
    SessionConfig defaultSessionTemplate() const;
    /** settings.logging.directory or <dataDir>/logs. */
    std::filesystem::path logDirectory() const;

    /* ---- UI state ---- */
    Json getState(const std::string& key) const;
    void setState(const std::string& key, const Json& value);

    KnownHosts& knownHosts() { return knownHosts_; }
    Credentials& credentials() { return credentials_; }

    /** Normalizes a session document: merges it over the built-in defaults (unknown fields preserved), repairs
     *  wrongly typed known fields, normalizes the folder and generates an id when empty (if generateId). */
    static Json normalizeSessionJson(const Json& doc, bool generateId = true);

    /** Change notifications, invoked on the thread that made the change (after the lock is released). */
    std::function<void()> onSessionsChanged;
    std::function<void(const Json& settings)> onSettingsChanged;

private:
    void writeSessionsLocked() const;
    void writeSettingsLocked() const;
    void writeStateLocked() const;
    void notifySessionsChanged();

    std::filesystem::path dir_;
    mutable std::recursive_mutex mutex_;
    Json sessions_ = Json::array();
    Json folders_ = Json::array();
    Json settings_ = Json::object();
    Json state_ = Json::object();
    KnownHosts knownHosts_;
    Credentials credentials_;
};

/** Writes `content` to `path` atomically (temp file + rename). Throws std::runtime_error on failure. */
void writeFileAtomic(const std::filesystem::path& path, const std::string& content);
/** Reads a whole file; std::nullopt if missing/unreadable. */
std::optional<std::string> readFile(const std::filesystem::path& path);

}  // namespace sc
