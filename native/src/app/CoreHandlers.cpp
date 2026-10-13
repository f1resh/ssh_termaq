#include "app/CoreHandlers.h"

#include "app/Dialogs.h"
#include "app/MainWindow.h"
#include "bridge/Bridge.h"
#include "core/Crypto.h"
#include "core/Log.h"
#include "core/Strings.h"
#include "core/Win.h"
#include "store/Store.h"
#include "transport/SerialTransport.h"

#include <shellapi.h>
#include <shlobj.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>

#ifndef SC_VERSION
#define SC_VERSION "0.0.0"
#endif

namespace sc {

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
/* Clipboard                                                                 */
/* ------------------------------------------------------------------------ */

namespace {

bool openClipboard(HWND owner) {
    for (int i = 0; i < 10; ++i) {
        if (OpenClipboard(owner)) return true;
        Sleep(15);
    }
    return false;
}

}  // namespace

std::string readClipboardText(void* ownerHwnd) {
    if (!openClipboard(static_cast<HWND>(ownerHwnd))) return {};
    std::string out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            out = narrow(p);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

void writeClipboardText(void* ownerHwnd, const std::string& utf8) {
    // Normalize line endings to CRLF (Windows convention) without doubling existing CRLFs.
    std::string text;
    text.reserve(utf8.size() + 16);
    for (size_t i = 0; i < utf8.size(); ++i) {
        char c = utf8[i];
        if (c == '\n' && (i == 0 || utf8[i - 1] != '\r')) text += '\r';
        text += c;
    }
    std::wstring w = widen(text);
    if (!openClipboard(static_cast<HWND>(ownerHwnd))) return;
    EmptyClipboard();
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t))) {
        if (auto* p = static_cast<wchar_t*>(GlobalLock(mem))) {
            memcpy(p, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
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
    std::string ext = toLower(narrow(p.extension().wstring()));
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

bool isSafeExternalUrl(const std::string& url) {
    const std::string l = toLower(url);
    return startsWith(l, "http://") || startsWith(l, "https://") || startsWith(l, "mailto:");
}

void writeUtf8File(const fs::path& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot write " + narrow(path.wstring()));
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!f) throw std::runtime_error("Cannot write " + narrow(path.wstring()));
}

}  // namespace

/* ------------------------------------------------------------------------ */
/* Registration                                                              */
/* ------------------------------------------------------------------------ */

void registerCoreHandlers(const CoreHandlerContext& c) {
    Bridge& b = c.bridge;
    Store& store = c.store;
    MainWindow& win = c.window;
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
        auto r = dialogs::openFile(dialogOptions(a, 0));
        return r ? Json(*r) : Json(nullptr);
    });
    b.handleAsync("dialog:openFiles", [](const Json& a) { return Json(dialogs::openFiles(dialogOptions(a, 0))); });
    b.handleAsync("dialog:openDirectory", [](const Json& a) {
        auto r = dialogs::openDirectory(dialogOptions(a, 0));
        return r ? Json(*r) : Json(nullptr);
    });
    b.handleAsync("dialog:saveFile", [](const Json& a) {
        auto r = dialogs::saveFile(dialogOptions(a, 0));
        return r ? Json(*r) : Json(nullptr);
    });
    b.handleAsync("dialog:saveTextFile", [](const Json& a) {
        const std::string content = argStr(a, 0);
        auto path = dialogs::saveFile(dialogOptions(a, 1));
        if (!path) return Json(nullptr);
        writeUtf8File(fs::path(widen(*path)), content);
        return Json(*path);
    });

    /* ---- app ---- */
    b.handleSync("app:info", [&win, paths](const Json&) {
        return Json{{"name", "SSH Crawler"},
                    {"version", SC_VERSION},
                    {"electron", ""},
                    {"chrome", win.browserVersion()},
                    {"node", ""},
                    {"platform", "win32"},
                    {"userDataDir", narrow(paths.dataDir.wstring())},
                    {"portable", paths.portable}};
    });
    b.handleSync("app:openExternal", [](const Json& a) {
        const std::string url = argStr(a, 0);
        if (!isSafeExternalUrl(url)) throw std::invalid_argument("Only http(s) and mailto links can be opened");
        ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return Json(nullptr);
    });
    b.handleSync("app:openPath", [](const Json& a) {
        fs::path p(widen(argStr(a, 0)));
        std::error_code ec;
        if (p.empty() || !fs::exists(p, ec)) throw std::invalid_argument("Path does not exist");
        ShellExecuteW(nullptr, L"open", p.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return Json(nullptr);
    });
    b.on("app:showItemInFolder", [](const Json& a) {
        fs::path p(widen(argStr(a, 0)));
        std::error_code ec;
        if (p.empty() || !fs::exists(p, ec)) return;
        if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(p.wstring().c_str())) {
            SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
            ILFree(pidl);
        }
    });
    b.on("app:setBackgroundMaterial", [&win](const Json& a) { win.setBackgroundMaterial(argStr(a, 0, "none")); });
    b.on("app:setThemeSource", [&win](const Json& a) { win.setThemeSource(argStr(a, 0, "system")); });
    b.handleSync("app:systemPrefersDark", [](const Json&) { return Json(MainWindow::systemPrefersDark()); });
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
    b.handleSync("app:readClipboard", [&win](const Json&) { return Json(readClipboardText(win.hwnd())); });
    b.on("app:writeClipboard", [&win](const Json& a) { writeClipboardText(win.hwnd(), argStr(a, 0)); });
    b.on("app:beep", [](const Json&) { MessageBeep(MB_OK); });
    b.handleAsync("app:readFileDataUrl", [](const Json& a) -> Json {
        fs::path p(widen(argStr(a, 0)));
        std::error_code ec;
        if (p.empty() || !fs::is_regular_file(p, ec)) return nullptr;
        const auto size = fs::file_size(p, ec);
        if (ec || size > 5 * 1024 * 1024) return nullptr;
        std::ifstream f(p, std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!f && !f.eof()) return nullptr;
        return "data:" + mimeForPath(p) + ";base64," + crypto::base64Encode(data);
    });
    b.handleSync("app:logDirectory", [&store](const Json&) { return Json(narrow(store.logDirectory().wstring())); });
    b.on("app:confirmClose", [&win](const Json& a) { win.confirmClose(argBool(a, 0)); });
    b.on("app:quit", [&win](const Json&) { win.quit(); });

    /* ---- window ---- */
    b.on("window:minimize", [&win](const Json&) { win.minimize(); });
    b.on("window:toggleMaximize", [&win](const Json&) { win.toggleMaximize(); });
    b.on("window:close", [&win](const Json&) { win.requestClose(); });
    b.handleSync("window:isMaximized", [&win](const Json&) { return Json(win.isMaximized()); });
    b.on("window:setTitle", [&win](const Json& a) { win.setTitle(argStr(a, 0)); });
    b.on("window:flash", [&win](const Json&) { win.flash(); });

    /* ---- store change events ---- */
    store.onSessionsChanged = [&b] { b.emit("sessions:onChanged", Json::array()); };
    store.onSettingsChanged = [&b](const Json& s) { b.emit("settings:onChanged", Json::array({s})); };
    store.knownHosts().onChanged = [&b] { b.emit("knownHosts:onChanged", Json::array()); };
}

}  // namespace sc
