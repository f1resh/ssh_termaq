#include "sftp/LocalOpen.h"

#include "core/Strings.h"
#include "core/Uuid.h"
#include "core/Win.h"
#include "sftp/RemoteFs.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <fstream>
#include <mutex>

namespace sc::sftpimpl {

namespace fs = std::filesystem;

namespace {

struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

/** Removes temp folders older than a week (best effort, once per process). */
void pruneTemp(const fs::path& root) {
    static std::once_flag once;
    std::call_once(once, [&root] {
        std::error_code ec;
        const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24 * 7);
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code ec2;
            if (it->is_directory(ec2) && it->last_write_time(ec2) < cutoff) fs::remove_all(it->path(), ec2);
        }
    });
}

void revealInExplorer(const fs::path& file) {
    if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(file.c_str())) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
    }
}

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

fs::path tempRoot() {
    wchar_t buf[MAX_PATH + 1];
    const DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    fs::path root = n ? fs::path(std::wstring(buf, n)) : fs::temp_directory_path();
    return root / L"SSH Crawler";
}

fs::path newTempFolder() {
    const fs::path root = tempRoot();
    pruneTemp(root);
    const fs::path dir = root / widen(newUuid().substr(0, 8));
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) throw SftpError("Cannot create a temporary folder: " + ec.message());
    return dir;
}

bool isExecutableType(const fs::path& file) {
    static const wchar_t* kTypes[] = {L".exe", L".com", L".bat", L".cmd", L".scr", L".pif", L".msi", L".msp", L".msc",
                                      L".ps1", L".psm1", L".vbs", L".vbe", L".js", L".jse", L".wsf", L".wsh", L".hta",
                                      L".cpl", L".jar", L".lnk", L".url", L".reg", L".inf", L".appref-ms", L".application",
                                      L".gadget", L".chm", L".dll", L".sys", L".ocx", L".scf", L".settingcontent-ms"};
    std::wstring ext = file.extension().wstring();
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    for (const wchar_t* t : kTypes)
        if (ext == t) return true;
    return false;
}

void markOfTheWeb(const fs::path& file) {
    std::ofstream ads(file.wstring() + L":Zone.Identifier", std::ios::binary | std::ios::trunc);
    if (ads) ads << "[ZoneTransfer]\r\nZoneId=3\r\n";
}

void shellOpen(const fs::path& file) {
    ComScope com;
    markOfTheWeb(file);
    if (isExecutableType(file)) {
        revealInExplorer(file);  // show the file instead of running downloaded code
        return;
    }
    bool ok = shellExecute(file, nullptr);
    if (!ok && GetLastError() == ERROR_NO_ASSOCIATION) ok = shellExecute(file, L"openas");
    if (!ok) throw SftpError("Cannot open '" + narrow(file.filename().wstring()) + "': " + lastErrorMessage());
}

void shellEdit(const fs::path& file, const std::string& editor) {
    ComScope com;
    markOfTheWeb(file);
    if (!editor.empty()) {
        const std::wstring program = widen(editor);
        if (!shellExecute(file, L"open", program.c_str())) {
            throw SftpError("Cannot start the editor '" + editor + "': " + lastErrorMessage());
        }
        return;
    }
    if (hasVerb(file, L"edit") && shellExecute(file, L"edit")) return;
    if (!isExecutableType(file) && hasVerb(file, L"open") && shellExecute(file, nullptr)) return;
    if (!shellExecute(file, L"open", L"notepad.exe")) {
        throw SftpError("Cannot open '" + narrow(file.filename().wstring()) + "' for editing: " + lastErrorMessage());
    }
}

}  // namespace sc::sftpimpl
