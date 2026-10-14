#include "store/Store.h"

#include "core/Log.h"
#include "core/Strings.h"
#include "core/Uuid.h"
#include "store/JsonFile.h"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;

namespace st {

namespace {

constexpr const char* kEncPrefix = "enc:";

const Json& sessionDefaults() {
    static const Json defaults = Json(SessionConfig{});
    return defaults;
}

bool typeCompatible(const Json& value, const Json& def) {
    switch (def.type()) {
        case Json::value_t::object: return value.is_object();
        case Json::value_t::array: return value.is_array();
        case Json::value_t::string: return value.is_string();
        case Json::value_t::boolean: return value.is_boolean();
        case Json::value_t::number_integer:
        case Json::value_t::number_unsigned:
        case Json::value_t::number_float: return value.is_number();
        default: return true;
    }
}

/** Fills missing known fields from `defs` and replaces wrongly typed ones (recursively). Unknown fields stay. */
void repairObject(Json& doc, const Json& defs) {
    for (auto it = defs.begin(); it != defs.end(); ++it) {
        auto found = doc.find(it.key());
        if (found == doc.end()) {
            doc[it.key()] = it.value();
        } else if (!typeCompatible(*found, it.value())) {
            *found = it.value();
        } else if (it.value().is_object() && !it.value().empty()) {
            repairObject(*found, it.value());
        }
    }
}

/** Arrays of objects: drop non-objects, repair each element against the element defaults. */
void repairObjectArray(Json& arr, const Json& elementDefaults) {
    Json out = Json::array();
    for (auto& e : arr) {
        if (!e.is_object()) continue;
        repairObject(e, elementDefaults);
        out.push_back(std::move(e));
    }
    arr = std::move(out);
}

void repairStringArray(Json& arr) {
    Json out = Json::array();
    for (auto& e : arr)
        if (e.is_string()) out.push_back(std::move(e));
    arr = std::move(out);
}

/** proxy.password: plain text in memory, "enc:<base64 DPAPI>" on disk. */
void encryptProxyPassword(Json& session) {
    auto proxy = session.find("proxy");
    if (proxy == session.end() || !proxy->is_object()) return;
    auto pw = proxy->find("password");
    if (pw == proxy->end() || !pw->is_string()) return;
    const std::string& plain = pw->get_ref<const std::string&>();
    if (plain.empty()) return;
    *pw = std::string(kEncPrefix) + Credentials::protect(plain);
}

void decryptProxyPassword(Json& session) {
    auto proxy = session.find("proxy");
    if (proxy == session.end() || !proxy->is_object()) return;
    auto pw = proxy->find("password");
    if (pw == proxy->end() || !pw->is_string()) return;
    const std::string& stored = pw->get_ref<const std::string&>();
    if (!startsWith(stored, kEncPrefix)) return;  // legacy / hand-edited plain text: re-encrypted on next save
    auto plain = Credentials::unprotect(stored.substr(4));
    if (!plain) applog::warn("Store: a proxy password could not be decrypted (different Windows user?) and was cleared");
    *pw = plain.value_or(std::string());
}

std::string folderOf(const Json& session) {
    auto it = session.find("folder");
    return (it != session.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

std::string idOf(const Json& session) {
    auto it = session.find("id");
    return (it != session.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

bool inFolder(const std::string& folder, const std::string& root) {
    return folder == root || startsWith(folder, root + "/");
}

/** Re-parent: replace prefix `from` (a folder or any descendant) with `to`. */
std::string reparent(const std::string& folder, const std::string& from, const std::string& to) {
    if (folder == from) return to;
    if (startsWith(folder, from + "/")) return to.empty() ? folder.substr(from.size() + 1) : to + folder.substr(from.size());
    return folder;
}

std::string parentFolder(const std::string& folder) {
    auto pos = folder.rfind('/');
    return pos == std::string::npos ? std::string() : folder.substr(0, pos);
}

void addWithParents(std::set<std::string>& out, const std::string& folder) {
    std::string f = normalizeFolder(folder);
    while (!f.empty()) {
        out.insert(f);
        f = parentFolder(f);
    }
}

Json uniqueFolders(const Json& folders) {
    Json out = Json::array();
    std::set<std::string> seen;
    for (const auto& f : folders) {
        if (!f.is_string()) continue;
        std::string n = normalizeFolder(f.get<std::string>());
        if (!n.empty() && seen.insert(n).second) out.push_back(n);
    }
    return out;
}

}  // namespace

/* ------------------------------------------------------------------------ */

Store::Store(std::filesystem::path dataDir)
    : dir_(std::move(dataDir)), knownHosts_(dir_ / "known_hosts.json"), credentials_(dir_ / "credentials.json") {}

Store::~Store() = default;

Json Store::normalizeSessionJson(const Json& in, bool generateId) {
    Json doc = in.is_object() ? in : Json::object();
    repairObject(doc, sessionDefaults());
    repairObjectArray(doc["ssh"]["tunnels"], Json(PortForward{}));
    repairObjectArray(doc["connection"]["environment"], Json(EnvVar{}));
    for (const char* k : {"kex", "ciphers", "hostKeyAlgorithms", "macs"}) repairStringArray(doc["ssh"][k]);
    doc["folder"] = normalizeFolder(doc["folder"].get<std::string>());
    if (generateId && doc["id"].get<std::string>().empty()) doc["id"] = newUuid();
    return doc;
}

void Store::load() {
    fs::path sessionsFile = dir_ / "sessions.json";
    fs::path settingsFile = dir_ / "settings.json";
    fs::path stateFile = dir_ / "state.json";

    Json sessions = Json::array(), folders = Json::array(), settings = Json::object(), state = Json::object();

    Json root;
    if (storeio::loadJson(sessionsFile, root) == storeio::LoadStatus::Ok) {
        const Json* list = nullptr;
        if (root.is_object()) {
            if (root.contains("sessions") && root["sessions"].is_array()) list = &root["sessions"];
            if (root.contains("folders") && root["folders"].is_array()) folders = uniqueFolders(root["folders"]);
        } else if (root.is_array()) {
            list = &root;  // bare array of sessions (hand-written / older format)
        } else {
            storeio::backupCorrupt(sessionsFile, "unexpected document shape");
        }
        if (list) {
            std::set<std::string> ids;
            for (const auto& item : *list) {
                if (!item.is_object()) continue;
                Json doc = normalizeSessionJson(item);
                decryptProxyPassword(doc);
                // Duplicate ids would make updates ambiguous: give the later copy a fresh id.
                if (!ids.insert(idOf(doc)).second) {
                    doc["id"] = newUuid();
                    ids.insert(idOf(doc));
                }
                sessions.push_back(std::move(doc));
            }
        }
    }

    root = nullptr;
    if (storeio::loadJson(settingsFile, root) == storeio::LoadStatus::Ok) {
        if (root.is_object()) {
            settings = std::move(root);
            if (settings.contains("defaultSession") && settings["defaultSession"].is_object())
                decryptProxyPassword(settings["defaultSession"]);
        } else {
            storeio::backupCorrupt(settingsFile, "root is not an object");
        }
    }

    root = nullptr;
    if (storeio::loadJson(stateFile, root) == storeio::LoadStatus::Ok) {
        if (root.is_object())
            state = std::move(root);
        else
            storeio::backupCorrupt(stateFile, "root is not an object");
    }

    const size_t sessionCount = sessions.size(), folderCount = folders.size();
    {
        std::lock_guard lock(mutex_);
        sessions_ = std::move(sessions);
        folders_ = std::move(folders);
        settings_ = std::move(settings);
        state_ = std::move(state);
    }
    knownHosts_.load();
    credentials_.load();
    applog::info("Store: loaded " + std::to_string(sessionCount) + " sessions, " + std::to_string(folderCount) + " folders, " +
                 std::to_string(knownHosts_.list().size()) + " known host keys");
}

/* ---- persistence -------------------------------------------------------- */

void Store::writeSessionsLocked() const {
    Json list = Json::array();
    for (const auto& s : sessions_) {
        Json disk = s;
        encryptProxyPassword(disk);
        list.push_back(std::move(disk));
    }
    storeio::saveJson(dir_ / "sessions.json", Json{{"version", 1}, {"sessions", std::move(list)}, {"folders", folders_}});
}

void Store::writeSettingsLocked() const {
    Json disk = settings_;
    if (disk.contains("defaultSession") && disk["defaultSession"].is_object()) encryptProxyPassword(disk["defaultSession"]);
    storeio::saveJson(dir_ / "settings.json", disk);
}

void Store::writeStateLocked() const { storeio::saveJson(dir_ / "state.json", state_); }

void Store::notifySessionsChanged() {
    if (onSessionsChanged) onSessionsChanged();
}

/* ---- sessions ----------------------------------------------------------- */

Json Store::listSessionsJson() const {
    std::lock_guard lock(mutex_);
    return sessions_;
}

std::vector<SessionConfig> Store::listSessions() const {
    Json docs = listSessionsJson();
    std::vector<SessionConfig> out;
    out.reserve(docs.size());
    for (const auto& d : docs) {
        try {
            out.push_back(sessionFromJson(d));
        } catch (const std::exception& e) {
            applog::warn(std::string("Store: skipping unparsable session: ") + e.what());
        }
    }
    return out;
}

std::optional<Json> Store::getSessionJson(const std::string& id) const {
    std::lock_guard lock(mutex_);
    for (const auto& s : sessions_)
        if (idOf(s) == id) return std::optional<Json>(std::in_place, s);
    return std::nullopt;
}

std::optional<SessionConfig> Store::getSession(const std::string& id) const {
    auto doc = getSessionJson(id);
    if (!doc) return std::nullopt;
    try {
        return sessionFromJson(*doc);
    } catch (const std::exception& e) {
        applog::warn(std::string("Store: unparsable session: ") + e.what());
        return std::nullopt;
    }
}

Json Store::saveSession(const Json& session) {
    if (!session.is_object()) throw std::invalid_argument("Session must be a JSON object");
    Json doc = normalizeSessionJson(session);
    const int64_t now = nowMs();
    {
        std::lock_guard lock(mutex_);
        Json previous = sessions_;
        const std::string id = idOf(doc);
        auto it = std::find_if(sessions_.begin(), sessions_.end(), [&](const Json& s) { return idOf(s) == id; });
        if (doc["createdAt"].get<int64_t>() == 0)
            doc["createdAt"] = (it != sessions_.end() && (*it)["createdAt"].get<int64_t>() != 0) ? (*it)["createdAt"].get<int64_t>()
                                                                                                  : now;
        doc["updatedAt"] = now;
        if (it != sessions_.end())
            *it = doc;
        else
            sessions_.push_back(doc);
        try {
            writeSessionsLocked();
        } catch (...) {
            sessions_ = std::move(previous);
            throw;
        }
    }
    notifySessionsChanged();
    return doc;
}

SessionConfig Store::saveSession(const SessionConfig& session) {
    // Typed save: overlay the typed fields onto the stored document so fields unknown to C++ survive.
    Json typed = session;
    Json doc = Json::object();
    if (!session.id.empty())
        if (auto existing = getSessionJson(session.id)) doc = std::move(*existing);
    jsonDeepMerge(doc, typed);
    doc["appearance"] = session.appearance.is_object() ? session.appearance : Json::object();  // UI-owned: replace
    return sessionFromJson(saveSession(doc));
}

void Store::saveSessions(const Json& sessionsArray) {
    if (!sessionsArray.is_array()) throw std::invalid_argument("Expected an array of sessions");
    const int64_t now = nowMs();
    {
        std::lock_guard lock(mutex_);
        Json previous = sessions_;
        for (const auto& item : sessionsArray) {
            if (!item.is_object()) continue;
            Json doc = normalizeSessionJson(item);
            const std::string id = idOf(doc);
            auto it = std::find_if(sessions_.begin(), sessions_.end(), [&](const Json& s) { return idOf(s) == id; });
            if (doc["createdAt"].get<int64_t>() == 0)
                doc["createdAt"] = (it != sessions_.end() && (*it)["createdAt"].get<int64_t>() != 0)
                                       ? (*it)["createdAt"].get<int64_t>()
                                       : now;
            if (doc["updatedAt"].get<int64_t>() == 0) doc["updatedAt"] = now;
            if (it != sessions_.end())
                *it = std::move(doc);
            else
                sessions_.push_back(std::move(doc));
        }
        try {
            writeSessionsLocked();
        } catch (...) {
            sessions_ = std::move(previous);
            throw;
        }
    }
    notifySessionsChanged();
}

void Store::deleteSessions(const std::vector<std::string>& ids) {
    std::vector<std::string> removed;
    {
        std::lock_guard lock(mutex_);
        Json previous = sessions_;
        Json kept = Json::array();
        for (auto& s : sessions_) {
            const std::string id = idOf(s);
            if (std::find(ids.begin(), ids.end(), id) != ids.end())
                removed.push_back(id);
            else
                kept.push_back(std::move(s));
        }
        if (removed.empty()) {
            sessions_ = std::move(previous);
            return;
        }
        sessions_ = std::move(kept);
        try {
            writeSessionsLocked();
        } catch (...) {
            sessions_ = std::move(previous);
            throw;
        }
    }
    // Saved passwords of deleted sessions are no longer reachable: drop them.
    for (const auto& id : removed) {
        try {
            credentials_.remove(Credentials::passwordKey(id));
        } catch (const std::exception& e) {
            applog::warn(std::string("Store: could not remove saved password of a deleted session: ") + e.what());
        }
    }
    notifySessionsChanged();
}

std::vector<std::string> Store::folders() const {
    std::set<std::string> all;
    {
        std::lock_guard lock(mutex_);
        for (const auto& f : folders_)
            if (f.is_string()) addWithParents(all, f.get<std::string>());
        for (const auto& s : sessions_) addWithParents(all, folderOf(s));
    }
    std::vector<std::string> out(all.begin(), all.end());
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        std::string la = toLower(a), lb = toLower(b);
        return la != lb ? la < lb : a < b;
    });
    return out;
}

void Store::createFolder(const std::string& path) {
    const std::string folder = normalizeFolder(path);
    if (folder.empty()) throw std::invalid_argument("Folder name is empty");
    {
        std::lock_guard lock(mutex_);
        for (const auto& f : folders_)
            if (f.is_string() && f.get<std::string>() == folder) return;
        Json previous = folders_;
        folders_.push_back(folder);
        try {
            writeSessionsLocked();
        } catch (...) {
            folders_ = std::move(previous);
            throw;
        }
    }
    notifySessionsChanged();
}

void Store::renameFolder(const std::string& fromPath, const std::string& toPath) {
    const std::string from = normalizeFolder(fromPath);
    const std::string to = normalizeFolder(toPath);
    if (from.empty()) throw std::invalid_argument("Cannot rename the root folder");
    if (to.empty()) throw std::invalid_argument("New folder name is empty");
    if (from == to) return;
    {
        std::lock_guard lock(mutex_);
        Json prevSessions = sessions_, prevFolders = folders_;
        bool explicitFrom = false;
        Json folders = Json::array();
        for (const auto& f : folders_) {
            if (!f.is_string()) continue;
            const std::string name = f.get<std::string>();
            if (name == from) explicitFrom = true;
            folders.push_back(reparent(name, from, to));
        }
        if (explicitFrom) folders.push_back(to);
        folders_ = uniqueFolders(folders);
        for (auto& s : sessions_) {
            std::string folder = folderOf(s);
            if (inFolder(folder, from)) s["folder"] = reparent(folder, from, to);
        }
        try {
            writeSessionsLocked();
        } catch (...) {
            sessions_ = std::move(prevSessions);
            folders_ = std::move(prevFolders);
            throw;
        }
    }
    notifySessionsChanged();
}

void Store::deleteFolder(const std::string& path, bool deleteSessions) {
    const std::string folder = normalizeFolder(path);
    if (folder.empty()) throw std::invalid_argument("Cannot delete the root folder");
    const std::string parent = parentFolder(folder);
    std::vector<std::string> removed;
    {
        std::lock_guard lock(mutex_);
        Json prevSessions = sessions_, prevFolders = folders_;
        Json folders = Json::array();
        for (const auto& f : folders_) {
            if (!f.is_string()) continue;
            const std::string name = f.get<std::string>();
            if (name == folder) continue;
            if (inFolder(name, folder)) {
                // Keeping contents: sub-folders move up one level (into the deleted folder's parent).
                if (!deleteSessions) folders.push_back(reparent(name, folder, parent));
                continue;
            }
            folders.push_back(name);
        }
        folders_ = uniqueFolders(folders);
        Json kept = Json::array();
        for (auto& s : sessions_) {
            const std::string f = folderOf(s);
            if (!inFolder(f, folder)) {
                kept.push_back(std::move(s));
            } else if (deleteSessions) {
                removed.push_back(idOf(s));
            } else {
                s["folder"] = reparent(f, folder, parent);
                kept.push_back(std::move(s));
            }
        }
        sessions_ = std::move(kept);
        try {
            writeSessionsLocked();
        } catch (...) {
            sessions_ = std::move(prevSessions);
            folders_ = std::move(prevFolders);
            throw;
        }
    }
    for (const auto& id : removed) {
        try {
            credentials_.remove(Credentials::passwordKey(id));
        } catch (const std::exception& e) {
            applog::warn(std::string("Store: could not remove saved password of a deleted session: ") + e.what());
        }
    }
    notifySessionsChanged();
}

void Store::touchSession(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (auto& s : sessions_) {
        if (idOf(s) != id) continue;
        Json previous = s["lastUsedAt"];
        s["lastUsedAt"] = nowMs();
        try {
            writeSessionsLocked();
        } catch (const std::exception& e) {
            s["lastUsedAt"] = previous;
            applog::warn(std::string("Store: could not record session use: ") + e.what());
        }
        return;
    }
}

bool Store::sessionNameExists(const std::string& folder, const std::string& name) const {
    const std::string f = normalizeFolder(folder);
    std::lock_guard lock(mutex_);
    for (const auto& s : sessions_) {
        auto n = s.find("name");
        if (n != s.end() && n->is_string() && folderOf(s) == f && iequals(n->get_ref<const std::string&>(), name)) return true;
    }
    return false;
}

std::optional<SessionConfig> Store::findSessionByName(const std::string& rawName) const {
    const std::string name = trim(rawName);
    if (name.empty()) return std::nullopt;
    std::optional<Json> best;
    bool bestInRoot = false;
    {
        std::lock_guard lock(mutex_);
        for (const auto& s : sessions_) {
            auto n = s.find("name");
            if (n == s.end() || !n->is_string()) continue;
            const std::string& sname = n->get_ref<const std::string&>();
            const std::string folder = folderOf(s);
            const bool byName = iequals(sname, name);
            const bool byPath = !folder.empty() && iequals(folder + "/" + sname, normalizeFolder(name));
            if (!byName && !byPath) continue;
            const bool inRoot = folder.empty() || byPath;
            if (!best || (inRoot && !bestInRoot)) {
                best = s;
                bestInRoot = inRoot;
            }
        }
    }
    if (!best) return std::nullopt;
    try {
        return sessionFromJson(*best);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/* ---- settings ----------------------------------------------------------- */

Json Store::settings() const {
    std::lock_guard lock(mutex_);
    return settings_;
}

Json Store::updateSettings(const Json& patch) {
    if (!patch.is_object()) throw std::invalid_argument("Settings patch must be a JSON object");
    Json result;
    {
        std::lock_guard lock(mutex_);
        Json previous = settings_;
        jsonDeepMerge(settings_, patch);
        if (settings_.contains("defaultSession") && settings_["defaultSession"].is_object()) {
            Json ds = normalizeSessionJson(settings_["defaultSession"], /*generateId=*/false);
            if (ds["id"].get<std::string>().empty()) ds["id"] = "default";
            settings_["defaultSession"] = std::move(ds);
        }
        try {
            writeSettingsLocked();
        } catch (...) {
            settings_ = std::move(previous);
            throw;
        }
        result = settings_;
    }
    if (onSettingsChanged) onSettingsChanged(result);
    return result;
}

SessionConfig Store::defaultSessionTemplate() const {
    Json doc = Json::object();
    {
        std::lock_guard lock(mutex_);
        auto it = settings_.find("defaultSession");
        if (it != settings_.end() && it->is_object()) doc = *it;
    }
    try {
        SessionConfig s = sessionFromJson(normalizeSessionJson(doc, /*generateId=*/false));
        if (s.id.empty()) s.id = "default";
        return s;
    } catch (const std::exception& e) {
        applog::warn(std::string("Store: invalid default session template, using built-in defaults: ") + e.what());
        SessionConfig s;
        s.id = "default";
        return s;
    }
}

fs::path Store::logDirectory() const {
    std::string configured;
    {
        std::lock_guard lock(mutex_);
        auto logging = settings_.find("logging");
        if (logging != settings_.end() && logging->is_object()) {
            auto dir = logging->find("directory");
            if (dir != logging->end() && dir->is_string()) configured = trim(dir->get<std::string>());
        }
    }
    if (configured.empty()) return dir_ / "logs";
    fs::path p(widen(configured));
    if (p.is_relative()) p = dir_ / p;
    return p.lexically_normal();
}

/* ---- UI state ----------------------------------------------------------- */

Json Store::getState(const std::string& key) const {
    std::lock_guard lock(mutex_);
    auto it = state_.find(key);
    return it != state_.end() ? *it : Json(nullptr);
}

void Store::setState(const std::string& key, const Json& value) {
    if (key.empty()) throw std::invalid_argument("State key is empty");
    std::lock_guard lock(mutex_);
    auto it = state_.find(key);
    if (value.is_null()) {
        if (it == state_.end()) return;
    } else if (it != state_.end() && *it == value) {
        return;  // unchanged: skip the disk write
    }
    Json previous = state_;
    if (value.is_null())
        state_.erase(key);
    else
        state_[key] = value;
    try {
        writeStateLocked();
    } catch (...) {
        state_ = std::move(previous);
        throw;
    }
}

}  // namespace st
