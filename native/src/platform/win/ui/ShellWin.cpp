// The shell on Windows: ShellExecute(Ex), SHOpenFolderAndSelectItems, file association verbs, MessageBeep.
#include "platform/ui/Shell.h"

#include "platform/detail/WinApi.h"
#include "platform/sys/Errors.h"
#include "platform/sys/Fs.h"
#include "platform/win/sys/WinText.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <stdexcept>

namespace st::ui::shell {

namespace fs = std::filesystem;

namespace {

struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

bool hasVerb(const fs::path& file, const wchar_t* verb) {
    DWORD size = 0;
    const std::wstring ext = file.extension().wstring();
    if (ext.empty()) return false;
    const HRESULT hr = AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_COMMAND, ext.c_str(), verb, nullptr, &size);
    return hr == S_FALSE || (SUCCEEDED(hr) && size > 0);
}

bool shellExecute(const fs::path& file, const wchar_t* verb, const wchar_t* program = nullptr) {
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = verb;
    std::wstring params;
    if (program) {
        sei.lpFile = program;
        params = L"\"" + file.wstring() + L"\"";
        sei.lpParameters = params.c_str();
    } else {
        sei.lpFile = file.c_str();
    }
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != 0;
}

}  // namespace

void openExternal(const std::string& target) {
    ShellExecuteW(nullptr, L"open", widen(target).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void showInFolder(const fs::path& path) {
    ComScope com;
    if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str())) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
    }
}

void openFile(const fs::path& file) {
    ComScope com;
    bool ok = shellExecute(file, nullptr);
    if (!ok && GetLastError() == ERROR_NO_ASSOCIATION) ok = shellExecute(file, L"openas");
    if (!ok) throw std::runtime_error("Cannot open '" + narrow(file.filename().wstring()) + "': " + sys::lastErrorMessage());
}

void editFile(const fs::path& file, const std::string& editor) {
    ComScope com;
    if (!editor.empty()) {
        const std::wstring program = widen(editor);
        if (!shellExecute(file, L"open", program.c_str())) {
            throw std::runtime_error("Cannot start the editor '" + editor + "': " + sys::lastErrorMessage());
        }
        return;
    }
    if (hasVerb(file, L"edit") && shellExecute(file, L"edit")) return;
    if (!sys::isExecutableType(file) && hasVerb(file, L"open") && shellExecute(file, nullptr)) return;
    if (!shellExecute(file, L"open", L"notepad.exe")) {
        throw std::runtime_error("Cannot open '" + narrow(file.filename().wstring()) + "' for editing: " +
                                 sys::lastErrorMessage());
    }
}

void beep() { MessageBeep(MB_OK); }

}  // namespace st::ui::shell
