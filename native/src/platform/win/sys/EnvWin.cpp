#include "platform/sys/Env.h"

#include "platform/detail/WinApi.h"
#include "platform/win/sys/WinText.h"

#include <shlobj.h>

#include <iterator>
#include <vector>

namespace fs = std::filesystem;

extern "C" IMAGE_DOS_HEADER __ImageBase;  // the module this code is linked into

namespace st::sys {

namespace {

fs::path knownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)) && raw) out = raw;
    CoTaskMemFree(raw);
    return out;
}

}  // namespace

fs::path executablePath() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) return fs::path(std::wstring(buf.data(), n));
        if (buf.size() >= 32768) return {};
        buf.resize(buf.size() * 2);
    }
}

/** Link time stamped into the PE header of this module by the linker. */
std::int64_t executableBuildTime() {
    const auto* dos = &__ImageBase;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(dos) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->FileHeader.TimeDateStamp;
}

fs::path userDataRoot() {
    fs::path out = knownFolder(FOLDERID_RoamingAppData);
    if (out.empty()) {
        wchar_t env[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"APPDATA", env, MAX_PATH);
        if (n > 0 && n < MAX_PATH) out = env;
    }
    return out;
}

fs::path homeDirectory() { return knownFolder(FOLDERID_Profile); }

std::optional<std::string> getEnv(const std::string& name) {
    std::vector<wchar_t> env(32768);
    DWORD n = GetEnvironmentVariableW(widen(name).c_str(), env.data(), static_cast<DWORD>(env.size()));
    if (n == 0 || n >= env.size()) return std::nullopt;
    return narrow(std::wstring_view(env.data(), n));
}

std::string userName() {
    wchar_t buf[256];
    DWORD n = static_cast<DWORD>(std::size(buf));
    if (GetUserNameW(buf, &n) && n > 1) return narrow(std::wstring(buf, n - 1));
    return {};
}

}  // namespace st::sys
