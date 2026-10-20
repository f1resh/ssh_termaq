#include "platform/sys/Registry.h"

#include "platform/detail/WinApi.h"
#include "platform/win/sys/WinText.h"

#include <iterator>

namespace st::sys::registry {

namespace {

std::vector<std::pair<std::string, Value>> readValues(HKEY key) {
    std::vector<std::pair<std::string, Value>> values;
    for (DWORD i = 0;; ++i) {
        wchar_t name[16384];
        DWORD nameLen = static_cast<DWORD>(std::size(name));
        DWORD type = 0, size = 0;
        LSTATUS rc = RegEnumValueW(key, i, name, &nameLen, nullptr, &type, nullptr, &size);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc != ERROR_SUCCESS) continue;
        std::vector<BYTE> data(size + 4, 0);
        nameLen = static_cast<DWORD>(std::size(name));
        DWORD dataLen = size;
        if (RegEnumValueW(key, i, name, &nameLen, nullptr, &type, data.data(), &dataLen) != ERROR_SUCCESS) continue;
        Value v;
        if (type == REG_DWORD && dataLen >= 4) {
            v.type = Value::Type::Dword;
            v.dword = *reinterpret_cast<const DWORD*>(data.data());
        } else if (type == REG_SZ || type == REG_EXPAND_SZ) {
            v.type = Value::Type::String;
            v.str = narrow(reinterpret_cast<const wchar_t*>(data.data()));
        } else {
            v.type = Value::Type::Binary;
            v.bytes.assign(reinterpret_cast<const char*>(data.data()), dataLen);
        }
        values.emplace_back(narrow(std::wstring(name, nameLen)), std::move(v));
    }
    return values;
}

}  // namespace

std::vector<std::string> subkeys(const std::string& path) {
    std::vector<std::string> out;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, widen(path).c_str(), 0, KEY_READ, &root) != ERROR_SUCCESS) return out;
    for (DWORD i = 0;; ++i) {
        wchar_t name[1024];
        DWORD len = static_cast<DWORD>(std::size(name));
        LSTATUS rc = RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc != ERROR_SUCCESS) continue;
        out.push_back(narrow(std::wstring(name, len)));
    }
    RegCloseKey(root);
    return out;
}

std::optional<std::vector<std::pair<std::string, Value>>> values(const std::string& path) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, widen(path).c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return std::nullopt;
    auto out = readValues(key);
    RegCloseKey(key);
    return out;
}

}  // namespace st::sys::registry
