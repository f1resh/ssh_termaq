// Bridge handlers: importer:*, keys:*, knownHosts:importOpenSsh.
#include "importers/ImportHandlers.h"

#include "app/Dialogs.h"
#include "bridge/Bridge.h"
#include "core/Strings.h"
#include "core/Win.h"
#include "importers/Importers.h"
#include "keys/Keygen.h"
#include "store/Store.h"

#include <shlobj.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace st {

namespace {

namespace fs = std::filesystem;

std::string userSshDir() {
    PWSTR p = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &p)) && p) out = narrow((fs::path(p) / ".ssh").wstring());
    CoTaskMemFree(p);
    return out;
}

void writeBytes(const std::string& path, const std::string& bytes) {
    std::ofstream f(fs::path(widen(path)), std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot write " + path);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!f) throw std::runtime_error("Cannot write " + path);
}

std::optional<std::string> pathOrDialog(const Json& args, size_t i, FileDialogOptions opts) {
    const std::string p = argStr(args, i);
    if (!p.empty()) return p;
    return dialogs::openFile(opts);
}

std::vector<SessionConfig> sessionsByIds(const Store& store, const std::vector<std::string>& ids) {
    std::vector<SessionConfig> out;
    for (const auto& id : ids)
        if (auto s = store.getSession(id)) out.push_back(*s);
    return out;
}

}  // namespace

void registerImportHandlers(Bridge& bridge, Store& store) {
    bridge.handleAsync("importer:scanPuttyRegistry", [&store](const Json&) { return toJson(importers::previewPuttyRegistry(store)); });

    bridge.handleAsync("importer:parsePuttyRegFile", [&store](const Json& a) -> Json {
        auto path = pathOrDialog(a, 0, {"Import PuTTY sessions from a .reg file", "", {{"Registry files", {"reg"}}, {"All files", {"*"}}}});
        if (!path) return nullptr;
        return toJson(importers::previewPuttyRegFile(*path, store));
    });

    bridge.handleAsync("importer:scanSshConfig", [&store](const Json& a) { return toJson(importers::previewSshConfig(argStr(a, 0), store)); });

    bridge.handleAsync("importer:parseJsonFile", [&store](const Json& a) -> Json {
        auto path = pathOrDialog(a, 0, {"Import an SSHTerm backup", "", {{"JSON files", {"json"}}, {"All files", {"*"}}}});
        if (!path) return nullptr;
        return toJson(importers::previewJsonBackup(*path, store));
    });

    bridge.handleAsync("importer:apply", [&store](const Json& a) {
        const Json& req = jsonArg(a, 0);
        if (!req.is_object()) throw std::invalid_argument("import request must be an object");
        return Json(importers::applyImport(store, importRequestFromJson(req)));
    });

    bridge.handleAsync("importer:exportPuttyRegFile", [&store](const Json& a) -> Json {
        auto sessions = sessionsByIds(store, argStrList(a, 0));
        if (sessions.empty()) throw std::invalid_argument("No sessions selected");
        auto path = dialogs::saveFile({"Export sessions for PuTTY", "putty-sessions.reg", {{"Registry files", {"reg"}}}});
        if (!path) return nullptr;
        writeBytes(*path, importers::exportPuttyReg(sessions));
        return *path;
    });

    bridge.handleAsync("importer:exportJsonFile", [&store](const Json& a) -> Json {
        const auto ids = argStrList(a, 0);
        Json arr = Json::array();
        std::vector<std::string> folders;
        for (const auto& id : ids)
            if (auto j = store.getSessionJson(id)) {
                arr.push_back(*j);
                const std::string f = j->value("folder", std::string());
                if (!f.empty() && std::find(folders.begin(), folders.end(), f) == folders.end()) folders.push_back(f);
            }
        if (arr.empty()) throw std::invalid_argument("No sessions selected");
        auto path = dialogs::saveFile({"Export sessions", "sshterm-sessions.json", {{"JSON files", {"json"}}}});
        if (!path) return nullptr;
        writeBytes(*path, importers::exportJsonBackup(arr, folders));
        return *path;
    });

    bridge.handleAsync("keys:generate", [](const Json& a) {
        const Json& req = jsonArg(a, 0);
        return Json(keys::generateKey(req.is_object() ? req.get<KeygenRequest>() : KeygenRequest{}));
    });
    bridge.handleAsync("keys:inspect", [](const Json& a) { return Json(keys::inspectKey(argStr(a, 0), argStr(a, 1))); });
    bridge.handleAsync("keys:convert", [](const Json& a) { return Json(keys::convertKey(argStr(a, 0), argStr(a, 1), argStr(a, 2))); });

    bridge.handleAsync("knownHosts:importOpenSsh", [&store](const Json& a) -> Json {
        const std::string sshDir = userSshDir();
        auto path = pathOrDialog(a, 0, {"Import OpenSSH known_hosts", sshDir + "\\known_hosts", {{"All files", {"*"}}}});
        if (!path) return 0;
        std::ifstream f(fs::path(widen(*path)), std::ios::binary);
        if (!f) throw std::runtime_error("Cannot read " + *path);
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        int skipped = 0;
        auto hosts = importers::parseOpenSshKnownHosts(content, skipped);
        return store.knownHosts().addMany(hosts);
    });
}

}  // namespace st
