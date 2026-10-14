#include "core/Types.h"

#include "core/Strings.h"
#include "core/Uuid.h"

#include <chrono>

namespace sc {

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

int defaultPort(Protocol p) {
    switch (p) {
        case Protocol::Ssh: return 22;
        case Protocol::Telnet: return 23;
        case Protocol::Rlogin: return 513;
        default: return 0;
    }
}

SessionConfig makeDefaultSession() {
    SessionConfig s;
    s.id = newUuid();
    s.createdAt = s.updatedAt = nowMs();
    return s;
}

SessionConfig sessionFromJson(const Json& j) {
    SessionConfig s = j.get<SessionConfig>();
    if (!s.appearance.is_object()) s.appearance = Json::object();
    s.folder = normalizeFolder(s.folder);
    return s;
}

std::string sessionLabel(const SessionConfig& s) {
    if (!s.name.empty()) return s.name;
    if (s.protocol == Protocol::Serial) return s.serial.line;
    std::string out;
    if (!s.username.empty()) out += s.username + "@";
    out += s.host;
    if (s.port && s.port != defaultPort(s.protocol)) out += ":" + std::to_string(s.port);
    return out;
}

std::string normalizeFolder(const std::string& folder) {
    std::vector<std::string> parts;
    for (auto& p : split(folder, '/')) {
        std::string t = trim(p);
        if (!t.empty()) parts.push_back(t);
    }
    return join(parts, "/");
}

/* ---- prompts ------------------------------------------------------------ */

namespace prompts {

PromptInput username(const std::string& title, const std::string& message) {
    PromptInput p;
    p.json = {{"kind", "username"}, {"title", title}};
    if (!message.empty()) p.json["message"] = message;
    return p;
}

PromptInput password(const std::string& title, const std::string& message, const std::string& user, bool allowSave) {
    PromptInput p;
    p.json = {{"kind", "password"}, {"title", title}, {"allowSave", allowSave}};
    if (!message.empty()) p.json["message"] = message;
    if (!user.empty()) p.json["username"] = user;
    return p;
}

PromptInput kbdint(const std::string& title, const std::string& name, const std::string& instructions,
                   const std::vector<KbdPrompt>& items, bool allowSave) {
    PromptInput p;
    Json arr = Json::array();
    for (const auto& it : items) arr.push_back({{"prompt", it.prompt}, {"echo", it.echo}});
    p.json = {{"kind", "kbdint"}, {"title", title}, {"prompts", arr}, {"allowSave", allowSave}};
    if (!name.empty()) p.json["name"] = name;
    if (!instructions.empty()) p.json["instructions"] = instructions;
    return p;
}

PromptInput passphrase(const std::string& title, const std::string& keyPath, const std::string& message) {
    PromptInput p;
    p.json = {{"kind", "passphrase"}, {"title", title}, {"keyPath", keyPath}};
    if (!message.empty()) p.json["message"] = message;
    return p;
}

PromptInput hostkey(const std::string& host, int port, const std::string& keyType, const std::string& fingerprint,
                    const std::string& fingerprintMd5, bool mismatch, const std::vector<std::string>& previous) {
    PromptInput p;
    p.json = {{"kind", "hostkey"},
              {"host", host},
              {"port", port},
              {"keyType", keyType},
              {"fingerprint", fingerprint},
              {"fingerprintMd5", fingerprintMd5},
              {"status", mismatch ? "mismatch" : "unknown"},
              {"previous", previous}};
    return p;
}

PromptInput confirm(const std::string& title, const std::string& message, const std::vector<std::string>& buttons, bool danger) {
    PromptInput p;
    p.json = {{"kind", "confirm"}, {"title", title}, {"message", message}, {"buttons", buttons}, {"danger", danger}};
    return p;
}

}  // namespace prompts

/* ---- import ------------------------------------------------------------- */

Json toJson(const ImportPreview& p) {
    Json j = {{"source", p.source},
              {"candidates", p.candidates},
              {"hostKeys", p.hostKeys},
              {"errors", p.errors},
              {"origin", p.origin}};
    if (p.hasDefaultSettings) j["defaultSettings"] = p.defaultSettings;
    return j;
}

ImportRequest importRequestFromJson(const Json& j) {
    ImportRequest r;
    if (j.contains("candidates") && j["candidates"].is_array())
        for (const auto& c : j["candidates"]) r.candidates.push_back(c.get<ImportCandidate>());
    if (j.contains("hostKeys") && j["hostKeys"].is_array())
        for (const auto& h : j["hostKeys"]) r.hostKeys.push_back(h.get<KnownHost>());
    r.conflict = j.value("conflict", std::string("rename"));
    r.targetFolder = normalizeFolder(j.value("targetFolder", std::string()));
    if (j.contains("defaultSettings") && j["defaultSettings"].is_object()) {
        r.hasDefaultSettings = true;
        r.defaultSettings = sessionFromJson(j["defaultSettings"]);
    }
    return r;
}

Json toJson(const OpenRequest& r) {
    Json j = {{"session", r.session}};
    if (!r.savedSessionId.empty()) j["savedSessionId"] = r.savedSessionId;
    if (!r.password.empty()) j["password"] = r.password;
    if (!r.view.empty()) j["view"] = r.view;
    if (!r.remotePath.empty()) j["remotePath"] = r.remotePath;
    return j;
}

/* ---- file manager -------------------------------------------------------- */

void to_json(Json& j, const SyncItem& v) {
    j = Json{{"relPath", v.relPath}, {"isDir", v.isDir}, {"action", v.action}, {"reason", v.reason}};
    if (v.local) j["local"] = *v.local;
    if (v.remote) j["remote"] = *v.remote;
}

void from_json(const Json& j, SyncItem& v) {
    v = SyncItem{};
    v.relPath = j.value("relPath", std::string());
    v.isDir = j.value("isDir", false);
    v.action = j.value("action", std::string());
    v.reason = j.value("reason", std::string());
    if (j.contains("local") && j["local"].is_object()) v.local = j["local"].get<SyncSide>();
    if (j.contains("remote") && j["remote"].is_object()) v.remote = j["remote"].get<SyncSide>();
}

void to_json(Json& j, const LocalEntry& v) {
    j = Json{{"name", v.name},     {"path", v.path},         {"type", v.type},
             {"size", v.size},     {"mtime", v.mtime},       {"hidden", v.hidden},
             {"system", v.system}, {"readonly", v.readonly}, {"link", v.link}};
    if (!v.label.empty()) j["label"] = v.label;
    if (v.freeBytes >= 0) j["freeBytes"] = v.freeBytes;
    if (v.totalBytes >= 0) j["totalBytes"] = v.totalBytes;
    if (!v.icon.empty()) j["icon"] = v.icon;
}

}  // namespace sc
