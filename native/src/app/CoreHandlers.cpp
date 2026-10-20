#include "app/CoreHandlers.h"

#include "app/BuildInfo.h"
#include "app/HostController.h"
#include "bridge/Bridge.h"
#include "core/Crypto.h"
#include "core/Log.h"
#include "core/Strings.h"
#include "platform/Platform.h"
#include "platform/sys/Fs.h"
#include "platform/ui/Clipboard.h"
#include "platform/ui/Dialogs.h"
#include "platform/ui/Fonts.h"
#include "platform/ui/HostWindow.h"
#include "platform/ui/Shell.h"
#include "store/Store.h"
#include "transport/SerialTransport.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace st {

namespace fs = std::filesystem;

/* ------------------------------------------------------------------------ */
/* Open requests                                                             */
/* ------------------------------------------------------------------------ */

void OpenRequestQueue::push(const OpenRequest& req) {
    bool deliver = false;
    {
        std::lock_guard lock(mutex_);
        if (uiReady_)
            deliver = true;
        else
            pending_.push_back(req);
    }
    if (deliver) bridge_.emit("app:onOpenRequest", Json::array({toJson(req)}));
}

std::vector<OpenRequest> OpenRequestQueue::take() {
    std::lock_guard lock(mutex_);
    uiReady_ = true;
    std::vector<OpenRequest> out;
    out.swap(pending_);
    return out;
}

void OpenRequestQueue::reset() {
    std::lock_guard lock(mutex_);
    uiReady_ = false;
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

namespace {

FileDialogOptions dialogOptions(const Json& args, size_t i) {
    const Json& v = jsonArg(args, i);
    if (!v.is_object()) return {};
    try {
        return v.get<FileDialogOptions>();
    } catch (const std::exception&) {
        return {};
    }
}

std::string mimeForPath(const fs::path& p) {
    std::string ext = toLower(sys::fromPath(p.extension()));
    if (ext == ".wav") return "audio/wav";
    if (ext == ".mp3") return "audio/mpeg";
    if (ext == ".ogg" || ext == ".oga") return "audio/ogg";
    if (ext == ".flac") return "audio/flac";
    if (ext == ".m4a") return "audio/mp4";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".ico") return "image/x-icon";
    return "application/octet-stream";
}

/** window:setTitleBarLayout's {height, drag: [[x, y, w, h], ...], noDrag: [...]}; malformed parts are left out. */
ui::TitleBarLayout titleBarLayout(const Json& v) {
    ui::TitleBarLayout layout;
    if (!v.is_object()) return layout;
    if (v.contains("height") && v["height"].is_number()) layout.height = v["height"].get<double>();
    auto rects = [&v](const char* key, std::vector<ui::PageRect>& out) {
        if (!v.contains(key) || !v[key].is_array()) return;
        for (const Json& r : v[key]) {
            if (!r.is_array() || r.size() != 4) continue;
            if (!r[0].is_number() || !r[1].is_number() || !r[2].is_number() || !r[3].is_number()) continue;
            out.push_back({r[0].get<double>(), r[1].get<double>(), r[2].get<double>(), r[3].get<double>()});
        }
    };
    rects("drag", layout.drag);
    rects("noDrag", layout.noDrag);
    return layout;
}

/** window:setMenuBar's [{title, items: [{command, label, shortcut, checked, enabled} | {separator: true}]}]. */
std::vector<ui::MenuBarMenu> menuBar(const Json& v) {
    std::vector<ui::MenuBarMenu> menus;
    if (!v.is_array()) return menus;
    auto str = [](const Json& o, const char* key) {
        return o.contains(key) && o[key].is_string() ? o[key].get<std::string>() : std::string();
    };
    auto flag = [](const Json& o, const char* key, bool fallback) {
        return o.contains(key) && o[key].is_boolean() ? o[key].get<bool>() : fallback;
    };
    for (const Json& m : v) {
        if (!m.is_object() || !m.contains("items") || !m["items"].is_array()) continue;
        ui::MenuBarMenu menu;
        menu.title = str(m, "title");
        for (const Json& i : m["items"]) {
            if (!i.is_object()) continue;
            ui::MenuBarItem item;
            item.separator = flag(i, "separator", false);
            item.command = str(i, "command");
            item.label = str(i, "label");
            item.shortcut = str(i, "shortcut");
            item.checked = flag(i, "checked", false);
            item.enabled = flag(i, "enabled", true);
            if (item.separator || (!item.command.empty() && !item.label.empty())) menu.items.push_back(std::move(item));
        }
        menus.push_back(std::move(menu));
    }
    return menus;
}

void writeUtf8File(const fs::path& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot write " + sys::fromPath(path));
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!f) throw std::runtime_error("Cannot write " + sys::fromPath(path));
}

}  // namespace

/* ------------------------------------------------------------------------ */
/* Registration                                                              */
/* ------------------------------------------------------------------------ */

void registerCoreHandlers(const CoreHandlerContext& c) {
    Bridge& b = c.bridge;
    Store& store = c.store;
    HostController& host = c.host;
    ui::HostWindow& win = host.window();
    const AppPaths paths = c.paths;
    OpenRequestQueue& open = c.openRequests;

    /* ---- sessions ---- */
    b.handleSync("sessions:list", [&store](const Json&) { return store.listSessionsJson(); });
    b.handleSync("sessions:save", [&store](const Json& a) {
        const Json& s = jsonArg(a, 0);
        if (!s.is_object()) throw std::invalid_argument("session must be an object");
        return store.saveSession(s);
    });
    b.handleSync("sessions:saveMany", [&store](const Json& a) {
        const Json& arr = jsonArg(a, 0);
        if (!arr.is_array()) throw std::invalid_argument("sessions must be an array");
        store.saveSessions(arr);
        return Json(nullptr);
    });
    b.handleSync("sessions:delete", [&store](const Json& a) {
        store.deleteSessions(argStrList(a, 0));
        return Json(nullptr);
    });
    b.handleSync("sessions:folders", [&store](const Json&) { return Json(store.folders()); });
    b.handleSync("sessions:createFolder", [&store](const Json& a) {
        store.createFolder(argStr(a, 0));
        return Json(nullptr);
    });
    b.handleSync("sessions:renameFolder", [&store](const Json& a) {
        store.renameFolder(argStr(a, 0), argStr(a, 1));
        return Json(nullptr);
    });
    b.handleSync("sessions:deleteFolder", [&store](const Json& a) {
        store.deleteFolder(argStr(a, 0), argBool(a, 1));
        return Json(nullptr);
    });

    /* ---- credentials ---- */
    b.handleSync("credentials:hasPassword", [&store](const Json& a) {
        return Json(store.credentials().has(Credentials::passwordKey(argStr(a, 0))));
    });
    b.handleAsync("credentials:setPassword", [&store](const Json& a) {
        const std::string id = argStr(a, 0);
        if (id.empty()) throw std::invalid_argument("session id required");
        store.credentials().set(Credentials::passwordKey(id), argStr(a, 1));
        return Json(nullptr);
    });
    b.handleAsync("credentials:clearPassword", [&store](const Json& a) {
        store.credentials().remove(Credentials::passwordKey(argStr(a, 0)));
        return Json(nullptr);
    });

    /* ---- settings ---- */
    b.handleSync("settings:get", [&store](const Json&) { return store.settings(); });
    b.handleSync("settings:update", [&store](const Json& a) {
        const Json& patch = jsonArg(a, 0);
        if (!patch.is_object()) throw std::invalid_argument("patch must be an object");
        return store.updateSettings(patch);
    });

    /* ---- known hosts ---- */
    b.handleSync("knownHosts:list", [&store](const Json&) { return Json(store.knownHosts().list()); });
    b.handleSync("knownHosts:remove", [&store](const Json& a) {
        store.knownHosts().remove(argStrList(a, 0));
        return Json(nullptr);
    });

    /* ---- serial ---- */
    b.handleAsync("serial:listPorts", [](const Json&) { return Json(listSerialPorts()); });

    /* ---- dialogs (run on the pool; the dialog itself is marshalled to the UI thread) ---- */
    b.handleAsync("dialog:openFile", [](const Json& a) {
        auto r = ui::dialogs::openFile(dialogOptions(a, 0));
        return r ? Json(*r) : Json(nullptr);
    });
    b.handleAsync("dialog:openFiles", [](const Json& a) { return Json(ui::dialogs::openFiles(dialogOptions(a, 0))); });
    b.handleAsync("dialog:openDirectory", [](const Json& a) {
        auto r = ui::dialogs::openDirectory(dialogOptions(a, 0));
        return r ? Json(*r) : Json(nullptr);
    });
    b.handleAsync("dialog:saveFile", [](const Json& a) {
        auto r = ui::dialogs::saveFile(dialogOptions(a, 0));
        return r ? Json(*r) : Json(nullptr);
    });
    b.handleAsync("dialog:saveTextFile", [](const Json& a) {
        const std::string content = argStr(a, 0);
        auto path = ui::dialogs::saveFile(dialogOptions(a, 1));
        if (!path) return Json(nullptr);
        if (argBool(a, 2, false)) {
            // A private key: written whole, then readable by its owner only (Windows: only not read-only).
            sys::writeFileAtomic(sys::toPath(*path), content);
            std::error_code ec;
            fs::permissions(sys::toPath(*path), fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
        } else {
            writeUtf8File(sys::toPath(*path), content);
        }
        return Json(*path);
    });

    /* ---- app ---- */
    b.handleSync("app:info", [&win, paths](const Json&) {
        const BuildInfo& build = buildInfo();
        return Json{{"name", "SSHTermaq"},
                    {"version", build.version},
                    {"build", build.number},
                    {"commit", build.commit},
                    {"buildTime", build.time},
                    {"electron", ""},
                    {"chrome", win.browserVersion()},
                    {"node", ""},
                    {"platform", sys::osName()},
                    {"userDataDir", sys::fromPath(paths.dataDir)},
                    {"portable", paths.portable}};
    });
    b.handleSync("app:openExternal", [](const Json& a) {
        const std::string url = argStr(a, 0);
        if (!isSafeExternalUrl(url)) throw std::invalid_argument("Only http(s) and mailto links can be opened");
        ui::shell::openExternal(url);
        return Json(nullptr);
    });
    b.handleSync("app:openPath", [](const Json& a) {
        fs::path p = sys::toPath(argStr(a, 0));
        std::error_code ec;
        if (p.empty() || !fs::exists(p, ec)) throw std::invalid_argument("Path does not exist");
        ui::shell::openExternal(sys::fromPath(p));
        return Json(nullptr);
    });
    b.on("app:showItemInFolder", [](const Json& a) {
        fs::path p = sys::toPath(argStr(a, 0));
        std::error_code ec;
        if (p.empty() || !fs::exists(p, ec)) return;
        ui::shell::showInFolder(p);
    });
    b.on("app:setBackgroundMaterial", [&win](const Json& a) { win.setBackgroundMaterial(argStr(a, 0, "none")); });
    b.on("app:setThemeSource", [&win](const Json& a) { win.setThemeSource(argStr(a, 0, "system")); });
    b.handleSync("app:systemPrefersDark", [](const Json&) { return Json(ui::systemPrefersDark()); });
    b.on("app:toggleFullscreen", [&win](const Json&) { win.toggleFullscreen(); });
    b.on("app:setUiScale", [&win](const Json& a) { win.setUiScale(argNum(a, 0, 1.0)); });
    b.handleSync("app:takeOpenRequests", [&open](const Json&) {
        Json arr = Json::array();
        for (const auto& r : open.take()) arr.push_back(toJson(r));
        return arr;
    });
    b.handleSync("app:getState", [&store](const Json& a) { return store.getState(argStr(a, 0)); });
    b.handleSync("app:setState", [&store](const Json& a) {
        const std::string key = argStr(a, 0);
        if (key.empty()) throw std::invalid_argument("state key required");
        store.setState(key, jsonArg(a, 1));
        return Json(nullptr);
    });
    b.handleSync("app:readClipboard", [&win](const Json&) { return Json(ui::clipboard::readText(win.nativeHandle())); });
    b.on("app:writeClipboard", [&win](const Json& a) { ui::clipboard::writeText(win.nativeHandle(), argStr(a, 0)); });
    b.on("app:beep", [](const Json&) { ui::shell::beep(); });
    b.handleAsync("app:listFonts", [](const Json&) { return Json(ui::fonts::installedFamilies()); });
    b.handleAsync("app:readFileDataUrl", [](const Json& a) -> Json {
        fs::path p = sys::toPath(argStr(a, 0));
        std::error_code ec;
        if (p.empty() || !fs::is_regular_file(p, ec)) return nullptr;
        const auto size = fs::file_size(p, ec);
        if (ec || size > 5 * 1024 * 1024) return nullptr;
        std::ifstream f(p, std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!f && !f.eof()) return nullptr;
        return "data:" + mimeForPath(p) + ";base64," + crypto::base64Encode(data);
    });
    b.handleSync("app:logDirectory", [&store](const Json&) { return Json(sys::fromPath(store.logDirectory())); });
    b.on("app:confirmClose", [&host](const Json& a) { host.confirmClose(argBool(a, 0)); });
    b.on("app:quit", [&host](const Json&) { host.quit(); });

    /* ---- window ---- */
    b.on("window:minimize", [&win](const Json&) { win.minimize(); });
    b.on("window:toggleMaximize", [&win](const Json&) { win.toggleMaximize(); });
    b.on("window:close", [&win](const Json&) { win.requestClose(); });
    b.handleSync("window:isMaximized", [&win](const Json&) { return Json(win.isMaximized()); });
    b.on("window:setTitle", [&win](const Json& a) { win.setTitle(argStr(a, 0)); });
    b.on("window:flash", [&win](const Json&) { win.flash(); });
    b.on("window:setTitleBarLayout", [&win](const Json& a) { win.setTitleBarLayout(titleBarLayout(jsonArg(a, 0))); });
    b.on("window:setMenuBar", [&win](const Json& a) { win.setMenuBar(menuBar(jsonArg(a, 0))); });

    /* ---- store change events ---- */
    store.onSessionsChanged = [&b] { b.emit("sessions:onChanged", Json::array()); };
    store.onSettingsChanged = [&b](const Json& s) { b.emit("settings:onChanged", Json::array({s})); };
    store.knownHosts().onChanged = [&b] { b.emit("knownHosts:onChanged", Json::array()); };
}

}  // namespace st
