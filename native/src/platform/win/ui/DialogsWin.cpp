// File dialogs on Windows: IFileOpenDialog / IFileSaveDialog on the UI thread.
#include "platform/ui/Dialogs.h"

#include "core/Log.h"
#include "core/Threading.h"
#include "platform/detail/WinApi.h"
#include "platform/win/sys/WinText.h"

#include <shobjidl.h>
#include <wrl/client.h>

#include <filesystem>
#include <future>

using Microsoft::WRL::ComPtr;

namespace st::ui::dialogs {

namespace {

HWND g_owner = nullptr;
Dispatcher* g_dispatcher = nullptr;

/** Runs fn on the UI thread (inline when already there) and returns its result. */
template <class T, class F>
T onUiThread(F fn, T fallback) {
    if (!g_dispatcher || g_dispatcher->isUiThread()) return fn();
    auto promise = std::make_shared<std::promise<T>>();
    auto future = promise->get_future();
    g_dispatcher->post([promise, fn]() mutable {
        try {
            promise->set_value(fn());
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    });
    try {
        return future.get();
    } catch (const std::exception& e) {
        applog::error(std::string("File dialog failed: ") + e.what());
        return fallback;
    }
}

struct ComInit {
    HRESULT hr;
    ComInit() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ~ComInit() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

void applyCommon(IFileDialog* dlg, const FileDialogOptions& opts, bool folderDefaultIsFile) {
    if (!opts.title.empty()) dlg->SetTitle(widen(opts.title).c_str());

    std::vector<std::wstring> names, specs;
    for (const auto& f : opts.filters) {
        std::wstring spec;
        for (const auto& ext : f.extensions) {
            if (!spec.empty()) spec += L";";
            spec += (ext == "*" ? std::wstring(L"*.*") : L"*." + widen(ext));
        }
        if (spec.empty()) spec = L"*.*";
        names.push_back(widen(f.name));
        specs.push_back(spec);
    }
    if (!names.empty()) {
        std::vector<COMDLG_FILTERSPEC> fs;
        for (size_t i = 0; i < names.size(); ++i) fs.push_back({names[i].c_str(), specs[i].c_str()});
        dlg->SetFileTypes(static_cast<UINT>(fs.size()), fs.data());
        dlg->SetFileTypeIndex(1);
        // Default extension for save dialogs = first extension of the first filter.
        if (!opts.filters.empty() && !opts.filters[0].extensions.empty() && opts.filters[0].extensions[0] != "*")
            dlg->SetDefaultExtension(widen(opts.filters[0].extensions[0]).c_str());
    }

    if (!opts.defaultPath.empty()) {
        std::filesystem::path p(widen(opts.defaultPath));
        std::error_code ec;
        std::filesystem::path folder = p;
        std::wstring fileName;
        if (folderDefaultIsFile && !std::filesystem::is_directory(p, ec)) {
            folder = p.parent_path();
            fileName = p.filename().wstring();
        }
        if (!fileName.empty()) dlg->SetFileName(fileName.c_str());
        if (!folder.empty() && std::filesystem::is_directory(folder, ec)) {
            ComPtr<IShellItem> item;
            if (SUCCEEDED(SHCreateItemFromParsingName(folder.wstring().c_str(), nullptr, IID_PPV_ARGS(&item))))
                dlg->SetFolder(item.Get());
        }
    }
}

std::string itemPath(IShellItem* item) {
    PWSTR path = nullptr;
    std::string out;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
        out = narrow(path);
        CoTaskMemFree(path);
    }
    return out;
}

std::vector<std::string> runOpen(const FileDialogOptions& opts, bool multi, bool folders) {
    ComInit com;
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
    FILEOPENDIALOGOPTIONS fos = 0;
    dlg->GetOptions(&fos);
    fos |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_PATHMUSTEXIST;
    if (multi) fos |= FOS_ALLOWMULTISELECT;
    if (folders) fos |= FOS_PICKFOLDERS;
    else fos |= FOS_FILEMUSTEXIST;
    dlg->SetOptions(fos);
    applyCommon(dlg.Get(), opts, !folders);
    if (dlg->Show(g_owner) != S_OK) return {};

    std::vector<std::string> out;
    ComPtr<IShellItemArray> items;
    if (SUCCEEDED(dlg->GetResults(&items)) && items) {
        DWORD count = 0;
        items->GetCount(&count);
        for (DWORD i = 0; i < count; ++i) {
            ComPtr<IShellItem> item;
            if (SUCCEEDED(items->GetItemAt(i, &item))) {
                std::string p = itemPath(item.Get());
                if (!p.empty()) out.push_back(p);
            }
        }
    }
    return out;
}

}  // namespace

void init(void* ownerWindow, Dispatcher* dispatcher) {
    g_owner = static_cast<HWND>(ownerWindow);
    g_dispatcher = dispatcher;
}

std::optional<std::string> openFile(const FileDialogOptions& opts) {
    auto r = onUiThread<std::vector<std::string>>([opts] { return runOpen(opts, false, false); }, {});
    if (r.empty()) return std::nullopt;
    return r.front();
}

std::vector<std::string> openFiles(const FileDialogOptions& opts) {
    return onUiThread<std::vector<std::string>>([opts] { return runOpen(opts, true, false); }, {});
}

std::optional<std::string> openDirectory(const FileDialogOptions& opts) {
    auto r = onUiThread<std::vector<std::string>>([opts] { return runOpen(opts, false, true); }, {});
    if (r.empty()) return std::nullopt;
    return r.front();
}

std::optional<std::string> saveFile(const FileDialogOptions& opts) {
    return onUiThread<std::optional<std::string>>(
        [opts]() -> std::optional<std::string> {
            ComInit com;
            ComPtr<IFileSaveDialog> dlg;
            if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
                return std::nullopt;
            FILEOPENDIALOGOPTIONS fos = 0;
            dlg->GetOptions(&fos);
            dlg->SetOptions(fos | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_OVERWRITEPROMPT);
            applyCommon(dlg.Get(), opts, true);
            if (dlg->Show(g_owner) != S_OK) return std::nullopt;
            ComPtr<IShellItem> item;
            if (FAILED(dlg->GetResult(&item)) || !item) return std::nullopt;
            std::string p = itemPath(item.Get());
            if (p.empty()) return std::nullopt;
            return p;
        },
        std::nullopt);
}

}  // namespace st::ui::dialogs
