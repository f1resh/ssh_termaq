#include "importers/Importers.h"

#include "core/Crypto.h"
#include "core/Strings.h"
#include "core/Uuid.h"
#include "platform/sys/Env.h"
#include "platform/sys/Fs.h"
#include "putty/Putty.h"
#include "putty/RegFile.h"
#include "store/Store.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>

namespace st::importers {

namespace fs = std::filesystem;
using putty::RegValues;

namespace {

std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot read " + sys::fromPath(p));
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool nameExists(const Store& store, const std::string& name) {
    for (const auto& s : store.listSessions())
        if (iequals(s.name, name)) return true;
    return false;
}

fs::path homeDir() { return sys::homeDirectory(); }

/** Builds a PuTTY preview from (escaped name, values) pairs and host key entries. */
ImportPreview puttyPreview(const std::string& source, const std::string& origin,
                           const std::vector<std::pair<std::string, RegValues>>& sessions,
                           const std::vector<std::pair<std::string, std::string>>& hostKeys, const Store& store,
                           std::string (*unescape)(const std::string&) = putty::unescapeSessionName) {
    ImportPreview p;
    p.source = source;
    p.origin = origin;
    std::map<std::string, std::string> keyByLowerName;
    for (const auto& [escaped, values] : sessions) {
        const std::string name = unescape(escaped);
        std::vector<std::string> warnings;
        SessionConfig s = putty::puttyToSession(name, values, warnings);
        if (name == "Default Settings") {
            p.hasDefaultSettings = true;
            p.defaultSettings = s;
            p.defaultSettings.id = "default";
            p.defaultSettings.name = "Default Settings";
            continue;
        }
        ImportCandidate c;
        c.key = escaped;
        c.name = name;
        c.source = source;
        c.session = s;
        c.warnings = warnings;
        c.exists = nameExists(store, name);
        keyByLowerName[toLower(name)] = escaped;
        p.candidates.push_back(std::move(c));
    }
    // SSH proxy: PuTTY's ProxyHost may name another saved session.
    for (auto& c : p.candidates) {
        if (c.session.proxy.type != ProxyType::Ssh) continue;
        auto it = keyByLowerName.find(toLower(c.session.proxy.host));
        if (it != keyByLowerName.end() && it->second != c.key) c.session.proxy.jumpSessionId = "import:" + it->second;
    }
    std::sort(p.candidates.begin(), p.candidates.end(), [](const ImportCandidate& a, const ImportCandidate& b) {
        return toLower(a.name) < toLower(b.name);
    });
    std::map<std::string, int> reasons;
    for (const auto& [name, data] : hostKeys) {
        std::string reason;
        if (auto kh = putty::convertHostKey(name, data, reason)) p.hostKeys.push_back(*kh);
        else ++reasons[reason];
    }
    for (const auto& [reason, n] : reasons)
        p.errors.push_back(std::to_string(n) + " host key" + (n == 1 ? "" : "s") + " skipped: " + reason);
    return p;
}

/* ---- ssh config ---- */

bool globMatch(const std::string& pattern, const std::string& text) {
    // '*' and '?' wildcards, case-insensitive.
    size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || std::tolower(static_cast<unsigned char>(pattern[p])) ==
                                                           std::tolower(static_cast<unsigned char>(text[t])))) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

struct ConfigBlock {
    std::vector<std::string> patterns;
    std::vector<std::pair<std::string, std::string>> options;  // lower-case keyword -> raw value
};

/** Splits "key value" / "key=value", honouring double quotes in the value. */
bool splitDirective(const std::string& line, std::string& key, std::string& value) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '#') return false;
    size_t i = 0;
    while (i < t.size() && !std::isspace(static_cast<unsigned char>(t[i])) && t[i] != '=') ++i;
    key = toLower(t.substr(0, i));
    while (i < t.size() && (std::isspace(static_cast<unsigned char>(t[i])) || t[i] == '=')) ++i;
    value = trim(t.substr(i));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
    return !key.empty();
}

void parseConfigFile(const fs::path& path, std::vector<ConfigBlock>& blocks, std::vector<std::string>& errors, int depth) {
    if (depth > 8) return;
    std::string content;
    try {
        content = readAll(path);
    } catch (const std::exception& e) {
        if (depth == 0) throw;
        errors.push_back(e.what());
        return;
    }
    if (blocks.empty()) blocks.push_back({{"*"}, {}});  // options before the first Host apply to all
    for (auto& rawLine : split(content, '\n')) {
        std::string key, value;
        if (!splitDirective(rawLine, key, value)) continue;
        if (key == "host") {
            ConfigBlock b;
            for (auto& pat : split(value, ' ', true)) b.patterns.push_back(trim(pat));
            blocks.push_back(std::move(b));
        } else if (key == "match") {
            blocks.push_back({{"!__match_block__"}, {}});  // Match blocks are not evaluated
            errors.push_back("\"Match\" blocks are not supported and were ignored");
        } else if (key == "include") {
            for (auto& inc : split(value, ' ', true)) {
                fs::path ip = sys::toPath(inc);
                if (!inc.empty() && inc[0] == '~') ip = homeDir() / sys::toPath(inc.substr(inc[1] == '/' || inc[1] == '\\' ? 2 : 1));
                else if (ip.is_relative()) ip = homeDir() / ".ssh" / ip;
                const std::string pattern = sys::fromPath(ip.filename());
                if (pattern.find_first_of("*?") != std::string::npos) {
                    std::error_code ec;
                    for (auto& e : fs::directory_iterator(ip.parent_path(), ec))
                        if (globMatch(pattern, sys::fromPath(e.path().filename())))
                            parseConfigFile(e.path(), blocks, errors, depth + 1);
                } else {
                    parseConfigFile(ip, blocks, errors, depth + 1);
                }
            }
        } else {
            blocks.back().options.emplace_back(key, value);
        }
    }
}

bool blockMatches(const ConfigBlock& b, const std::string& alias) {
    bool matched = false;
    for (const auto& p : b.patterns) {
        if (!p.empty() && p[0] == '!') {
            if (globMatch(p.substr(1), alias)) return false;
        } else if (globMatch(p, alias)) {
            matched = true;
        }
    }
    return matched;
}

PortForward parseSshForward(ForwardType type, const std::string& value) {
    PortForward f;
    f.id = newUuid();
    f.type = type;
    auto parts = split(value, ' ', true);
    std::string src = parts.empty() ? "" : parts[0];
    if (auto c = src.rfind(':'); c != std::string::npos) {
        f.bindAddress = src.substr(0, c);
        src = src.substr(c + 1);
    }
    f.sourcePort = std::atoi(src.c_str());
    if (type != ForwardType::Dynamic && parts.size() >= 2) {
        std::string dst = parts[1];
        auto c = dst.rfind(':');
        f.destHost = c == std::string::npos ? dst : dst.substr(0, c);
        if (f.destHost.size() > 2 && f.destHost.front() == '[') f.destHost = f.destHost.substr(1, f.destHost.size() - 2);
        f.destPort = c == std::string::npos ? 0 : std::atoi(dst.substr(c + 1).c_str());
    } else if (type == ForwardType::Dynamic) {
        f.destHost.clear();
    }
    return f;
}

std::string translateProxyCommand(std::string cmd) {
    cmd = replaceAll(cmd, "%%", "\x01");
    cmd = replaceAll(cmd, "%h", "%host");
    cmd = replaceAll(cmd, "%p", "%port");
    cmd = replaceAll(cmd, "%r", "%user");
    return replaceAll(cmd, "\x01", "%%");
}

}  // namespace

ImportPreview previewPuttyRegistry(const Store& store) {
    auto sessions = putty::readRegistrySessions();
    auto hostKeys = putty::readRegistryHostKeys();
    // No registry (macOS) or nothing in it: PuTTY for Unix / macOS keeps its settings in a folder.
    if (sessions.empty() && hostKeys.empty()) {
        if (const auto dir = putty::unixSettingsDir())
            return puttyPreview("putty-registry", sys::fromPath(*dir), putty::readUnixSessions(*dir), putty::readUnixHostKeys(*dir),
                                store, putty::unescapeUnixSessionName);
    }
    return puttyPreview("putty-registry", "HKEY_CURRENT_USER\\Software\\SimonTatham\\PuTTY", sessions, hostKeys, store);
}

ImportPreview previewPuttyRegFile(const std::string& path, const Store& store) {
    const auto keys = putty::parseRegFile(readAll(sys::toPath(path)));
    std::vector<std::pair<std::string, RegValues>> sessions;
    std::vector<std::pair<std::string, std::string>> hostKeys;
    for (const auto& k : keys) {
        const std::string lower = toLower(k.path);
        const auto sessPos = lower.find("\\simontatham\\putty\\sessions\\");
        if (sessPos != std::string::npos) {
            std::string name = k.path.substr(sessPos + std::string("\\simontatham\\putty\\sessions\\").size());
            if (!name.empty() && name.find('\\') == std::string::npos) sessions.emplace_back(name, k.values);
        } else if (endsWith(lower, "\\simontatham\\putty\\sshhostkeys")) {
            for (const auto& [n, v] : k.values)
                if (v.type == putty::RegValue::Type::String) hostKeys.emplace_back(n, v.str);
        }
    }
    ImportPreview p = puttyPreview("putty-regfile", path, sessions, hostKeys, store);
    if (sessions.empty() && hostKeys.empty()) p.errors.push_back("The file contains no PuTTY sessions or host keys");
    return p;
}

ImportPreview previewSshConfig(const std::string& pathIn, const Store& store) {
    ImportPreview p;
    p.source = "ssh-config";
    fs::path path = pathIn.empty() ? homeDir() / ".ssh" / "config" : sys::toPath(pathIn);
    p.origin = sys::fromPath(path);
    std::vector<ConfigBlock> blocks;
    std::vector<std::string> errors;
    try {
        parseConfigFile(path, blocks, errors, 0);
    } catch (const std::exception& e) {
        p.errors.push_back(e.what());
        return p;
    }
    std::set<std::string> uniqueErrors(errors.begin(), errors.end());
    p.errors.assign(uniqueErrors.begin(), uniqueErrors.end());

    std::vector<std::string> aliases;
    for (const auto& b : blocks)
        for (const auto& pat : b.patterns)
            if (pat.find_first_of("*?!") == std::string::npos && std::find(aliases.begin(), aliases.end(), pat) == aliases.end())
                aliases.push_back(pat);

    static const std::set<std::string> ignored = {"stricthostkeychecking", "userknownhostsfile", "identitiesonly",
                                                   "addkeystoagent", "loglevel", "hashknownhosts", "visualhostkey",
                                                   "updatehostkeys", "preferredauthentications", "serveralivecountmax",
                                                   "tcpkeepalive", "checkhostip", "canonicalizehostname", "batchmode",
                                                   "usekeychain", "ignoreunknown"};  // macOS: the Keychain for passphrases
    for (const auto& alias : aliases) {
        // First value wins (OpenSSH semantics).
        std::map<std::string, std::string> opt;
        std::vector<std::string> localFwd, remoteFwd, dynFwd, setEnv;
        for (const auto& b : blocks) {
            if (!blockMatches(b, alias)) continue;
            for (const auto& [k, v] : b.options) {
                if (k == "localforward") localFwd.push_back(v);
                else if (k == "remoteforward") remoteFwd.push_back(v);
                else if (k == "dynamicforward") dynFwd.push_back(v);
                else if (k == "setenv") setEnv.push_back(v);
                else if (!opt.count(k)) opt[k] = v;
            }
        }
        std::vector<std::string> warnings;
        SessionConfig s = makeDefaultSession();
        s.name = alias;
        s.host = opt.count("hostname") ? replaceAll(opt["hostname"], "%h", alias) : alias;
        s.port = opt.count("port") ? std::atoi(opt["port"].c_str()) : 22;
        s.username = opt.count("user") ? opt["user"] : "";
        if (opt.count("identityfile")) {
            std::string idf = opt["identityfile"];
            if (!idf.empty() && idf[0] == '~') idf = sys::fromPath(homeDir() / sys::toPath(idf.substr(idf.size() > 1 && (idf[1] == '/' || idf[1] == '\\') ? 2 : 1)));
            s.ssh.auth.privateKeyPath = sys::fromPath(sys::toPath(idf).make_preferred());
        }
        if (opt.count("proxyjump") && !iequals(opt["proxyjump"], "none")) {
            std::string first = split(opt["proxyjump"], ',', true).front();
            s.proxy.type = ProxyType::Ssh;
            if (std::find(aliases.begin(), aliases.end(), first) != aliases.end() && first != alias) {
                s.proxy.jumpSessionId = "import:" + first;
                s.proxy.host = first;
            } else {
                std::string hp = first;
                if (auto at = hp.find('@'); at != std::string::npos) {
                    s.proxy.username = hp.substr(0, at);
                    hp = hp.substr(at + 1);
                }
                s.proxy.port = 22;
                if (auto c = hp.rfind(':'); c != std::string::npos) {
                    s.proxy.port = std::atoi(hp.substr(c + 1).c_str());
                    hp = hp.substr(0, c);
                }
                s.proxy.host = hp;
            }
            if (split(opt["proxyjump"], ',', true).size() > 1) warnings.push_back("Only the first ProxyJump hop was imported");
        } else if (opt.count("proxycommand") && !iequals(opt["proxycommand"], "none")) {
            s.proxy.type = ProxyType::Command;
            s.proxy.localCommand = translateProxyCommand(opt["proxycommand"]);
        }
        for (const auto& f : localFwd) s.ssh.tunnels.push_back(parseSshForward(ForwardType::Local, f));
        for (const auto& f : remoteFwd) s.ssh.tunnels.push_back(parseSshForward(ForwardType::Remote, f));
        for (const auto& f : dynFwd) s.ssh.tunnels.push_back(parseSshForward(ForwardType::Dynamic, f));
        auto yes = [&](const char* k) { return opt.count(k) && iequals(opt[k], "yes"); };
        auto no = [&](const char* k) { return opt.count(k) && iequals(opt[k], "no"); };
        s.ssh.auth.agentForwarding = yes("forwardagent");
        s.ssh.x11.enabled = yes("forwardx11") || yes("forwardx11trusted");
        s.ssh.compression = yes("compression");
        if (opt.count("serveraliveinterval")) s.connection.keepaliveSeconds = std::atoi(opt["serveraliveinterval"].c_str());
        if (opt.count("connecttimeout")) s.connection.connectTimeoutSeconds = std::max(1, std::atoi(opt["connecttimeout"].c_str()));
        if (opt.count("addressfamily")) {
            const std::string af = toLower(opt["addressfamily"]);
            s.connection.addressFamily = af == "inet" ? AddressFamily::IPv4 : af == "inet6" ? AddressFamily::IPv6 : AddressFamily::Auto;
        }
        for (const auto& line : setEnv)
            for (const auto& kv : split(line, ' ', true))
                if (auto eq = kv.find('='); eq != std::string::npos) s.connection.environment.push_back({kv.substr(0, eq), kv.substr(eq + 1)});
        if (opt.count("remotecommand")) s.ssh.remoteCommand = opt["remotecommand"];
        if (no("requesttty")) s.ssh.allocatePty = false;
        if (no("passwordauthentication")) s.ssh.auth.password = false;
        if (no("kbdinteractiveauthentication") || no("challengeresponseauthentication")) s.ssh.auth.keyboardInteractive = false;
        auto algos = [&](const char* k, std::vector<std::string>& out) {
            if (!opt.count(k)) return;
            std::string v = opt[k];
            if (!v.empty() && (v[0] == '+' || v[0] == '-' || v[0] == '^')) {
                warnings.push_back(std::string("Relative algorithm list for ") + k + " is not supported; defaults kept");
                return;
            }
            for (auto& a : split(v, ',', true)) out.push_back(trim(a));
        };
        algos("ciphers", s.ssh.ciphers);
        algos("kexalgorithms", s.ssh.kex);
        algos("hostkeyalgorithms", s.ssh.hostKeyAlgorithms);
        algos("macs", s.ssh.macs);
        static const std::set<std::string> handled = {
            "hostname", "port", "user", "identityfile", "proxyjump", "proxycommand", "forwardagent", "forwardx11",
            "forwardx11trusted", "compression", "serveraliveinterval", "connecttimeout", "addressfamily", "remotecommand",
            "requesttty", "passwordauthentication", "kbdinteractiveauthentication", "challengeresponseauthentication",
            "ciphers", "kexalgorithms", "hostkeyalgorithms", "macs", "sendenv", "pubkeyauthentication"};
        // IgnoreUnknown: directives (globs) the file itself says may be unknown, quietly.
        std::vector<std::string> quiet;
        if (opt.count("ignoreunknown"))
            for (const auto& pattern : split(opt["ignoreunknown"], ',', true)) quiet.push_back(trim(pattern));
        for (const auto& [k, v] : opt) {
            if (handled.count(k) || ignored.count(k)) continue;
            if (std::any_of(quiet.begin(), quiet.end(), [&](const std::string& p) { return globMatch(p, k); })) continue;
            warnings.push_back("Directive \"" + k + "\" is not supported");
        }

        ImportCandidate c;
        c.key = alias;
        c.name = alias;
        c.source = "ssh-config";
        c.session = s;
        c.warnings = warnings;
        c.exists = nameExists(store, alias);
        p.candidates.push_back(std::move(c));
    }
    if (p.candidates.empty()) p.errors.push_back("No concrete Host entries found");
    return p;
}

ImportPreview previewJsonBackup(const std::string& path, const Store& store) {
    ImportPreview p;
    p.source = "json";
    p.origin = path;
    Json j;
    try {
        j = Json::parse(readAll(sys::toPath(path)));
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Not a valid JSON backup: ") + e.what());
    }
    if (!j.is_object() || j.value("format", "") != "sshtermaq-sessions" || !j.contains("sessions") || !j["sessions"].is_array())
        throw std::runtime_error("Not an SSHTermaq session backup file");
    for (const auto& raw : j["sessions"]) {
        if (!raw.is_object()) continue;
        try {
            SessionConfig s = sessionFromJson(raw);
            ImportCandidate c;
            c.key = s.id.empty() ? newUuid() : s.id;
            c.name = s.name.empty() ? sessionLabel(s) : s.name;
            c.source = "json";
            c.session = s;
            if (!s.proxy.jumpSessionId.empty()) c.session.proxy.jumpSessionId = "import:" + s.proxy.jumpSessionId;
            c.exists = nameExists(store, c.name);
            p.candidates.push_back(std::move(c));
        } catch (const std::exception& e) {
            p.errors.push_back(std::string("Skipped an invalid session: ") + e.what());
        }
    }
    return p;
}

std::vector<KnownHost> parseOpenSshKnownHosts(const std::string& content, int& skipped) {
    std::vector<KnownHost> out;
    skipped = 0;
    for (auto& rawLine : split(content, '\n')) {
        std::string line = trim(rawLine);
        if (line.empty() || line[0] == '#') continue;
        auto parts = split(line, ' ', true);
        if (parts.size() < 3 || parts[0][0] == '@' || parts[0][0] == '|') {
            ++skipped;
            continue;
        }
        std::string blob;
        if (!crypto::base64Decode(parts[2], blob) || crypto::keyTypeOfBlob(blob) != parts[1]) {
            ++skipped;
            continue;
        }
        for (auto& hostSpec : split(parts[0], ',', true)) {
            if (hostSpec.empty() || hostSpec[0] == '!' || hostSpec.find_first_of("*?") != std::string::npos) continue;
            KnownHost kh;
            kh.id = newUuid();
            kh.port = 22;
            kh.host = hostSpec;
            if (hostSpec[0] == '[') {
                auto close = hostSpec.find(']');
                if (close == std::string::npos) continue;
                kh.host = hostSpec.substr(1, close - 1);
                if (close + 2 < hostSpec.size() && hostSpec[close + 1] == ':') kh.port = std::atoi(hostSpec.substr(close + 2).c_str());
            }
            kh.keyType = parts[1];
            kh.key = crypto::base64Encode(blob);
            kh.fingerprint = crypto::fingerprintSha256(blob);
            kh.addedAt = nowMs();
            kh.source = "openssh-import";
            if (parts.size() > 3) kh.comment = parts[3];
            out.push_back(kh);
        }
    }
    return out;
}

ImportResult applyImport(Store& store, const ImportRequest& req) {
    ImportResult r;
    const std::vector<SessionConfig> existing = store.listSessions();
    std::map<std::string, std::string> idByKey;
    std::set<std::string> takenNames;  // folder|name (lower) including this import
    for (const auto& s : existing) takenNames.insert(toLower(s.folder) + "|" + toLower(s.name));

    struct Planned {
        SessionConfig s;
        bool overwrite = false;
    };
    std::vector<Planned> planned;
    const int64_t now = nowMs();
    for (const auto& c : req.candidates) {
        SessionConfig s = c.session;
        std::string folder = req.targetFolder;
        if (!s.folder.empty()) folder = folder.empty() ? s.folder : folder + "/" + s.folder;
        s.folder = normalizeFolder(folder);
        s.name = c.name.empty() ? sessionLabel(s) : c.name;
        const std::string keyOf = toLower(s.folder) + "|" + toLower(s.name);
        bool overwrite = false;
        if (takenNames.count(keyOf)) {
            if (req.conflict == "skip") {
                ++r.skipped;
                continue;
            }
            if (req.conflict == "overwrite") {
                auto it = std::find_if(existing.begin(), existing.end(), [&](const SessionConfig& e) {
                    return iequals(e.folder, s.folder) && iequals(e.name, s.name);
                });
                if (it != existing.end()) {
                    s.id = it->id;
                    s.createdAt = it->createdAt;
                    overwrite = true;
                }
            } else {
                const std::string base = s.name;
                for (int n = 2; takenNames.count(toLower(s.folder) + "|" + toLower(s.name)); ++n) s.name = base + " (" + std::to_string(n) + ")";
            }
        }
        if (!overwrite) {
            s.id = newUuid();
            s.createdAt = now;
        }
        s.updatedAt = now;
        s.lastUsedAt = 0;
        takenNames.insert(toLower(s.folder) + "|" + toLower(s.name));
        idByKey[c.key] = s.id;
        planned.push_back({s, overwrite});
    }
    Json arr = Json::array();
    for (auto& p : planned) {
        std::string& jump = p.s.proxy.jumpSessionId;
        if (startsWith(jump, "import:")) {
            auto it = idByKey.find(jump.substr(7));
            if (it != idByKey.end()) {
                jump = it->second;
            } else {
                // Reference to an existing saved session with the same name, else drop it.
                const std::string target = jump.substr(7);
                auto ex = std::find_if(existing.begin(), existing.end(), [&](const SessionConfig& e) { return iequals(e.name, target) || e.id == target; });
                jump = ex != existing.end() ? ex->id : "";
                if (jump.empty()) r.errors.push_back("Jump host of \"" + p.s.name + "\" was not imported; using its host settings instead");
            }
        }
        arr.push_back(Json(p.s));
        if (p.overwrite) ++r.overwritten;
        else ++r.imported;
    }
    if (!req.targetFolder.empty()) store.createFolder(req.targetFolder);
    if (!arr.empty()) store.saveSessions(arr);
    if (!req.hostKeys.empty()) r.hostKeysImported = store.knownHosts().addMany(req.hostKeys);
    if (req.hasDefaultSettings) {
        SessionConfig d = req.defaultSettings;
        d.id = "default";
        d.name = "Default Settings";
        d.folder.clear();
        store.updateSettings(Json{{"defaultSession", Json(d)}});
    }
    return r;
}

std::string exportPuttyReg(const std::vector<SessionConfig>& sessions) {
    std::vector<putty::RegKey> keys;
    std::set<std::string> used;
    for (const auto& s : sessions) {
        std::string name = s.name.empty() ? sessionLabel(s) : s.name;
        if (!s.folder.empty()) name = s.folder + "/" + name;  // PuTTY has no folders; keep the path in the name
        std::string unique = name;
        for (int n = 2; used.count(toLower(unique)); ++n) unique = name + " (" + std::to_string(n) + ")";
        used.insert(toLower(unique));
        keys.push_back({"HKEY_CURRENT_USER\\" + std::string(putty::kSessionsKey) + "\\" + putty::escapeSessionName(unique),
                        putty::sessionToPutty(s)});
    }
    return putty::writeRegFile(keys);
}

std::string exportJsonBackup(const Json& sessions, const std::vector<std::string>& folders) {
    Json j = {{"format", "sshtermaq-sessions"}, {"version", 1}, {"exportedAt", nowMs()}, {"sessions", sessions}, {"folders", folders}};
    // Never export secrets.
    for (auto& s : j["sessions"])
        if (s.contains("proxy") && s["proxy"].is_object()) s["proxy"]["password"] = "";
    return j.dump(2);
}

}  // namespace st::importers
