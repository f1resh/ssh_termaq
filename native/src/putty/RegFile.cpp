#include "putty/RegFile.h"

#include "core/Strings.h"
#include "core/Win.h"

#include <cctype>
#include <stdexcept>

namespace st::putty {

bool CaseInsensitiveLess::operator()(const std::string& a, const std::string& b) const {
    return _stricmp(a.c_str(), b.c_str()) < 0;
}

namespace {

/** Decodes the file bytes into UTF-8 text. */
std::string decodeText(std::string_view bytes) {
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        std::wstring w((bytes.size() - 2) / 2, L'\0');
        memcpy(w.data(), bytes.data() + 2, w.size() * 2);
        return narrow(w);
    }
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF)
        return std::string(bytes.substr(3));
    // No BOM: valid UTF-8 stays as is, otherwise treat as the ANSI code page (REGEDIT4).
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (n > 0 || bytes.empty()) return std::string(bytes);
    const int m = MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(m), L'\0');
    MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), w.data(), m);
    return narrow(w);
}

/** Parses a quoted string starting at s[i] == '"'; advances i past the closing quote. */
std::string parseQuoted(const std::string& s, size_t& i) {
    std::string out;
    ++i;
    while (i < s.size()) {
        char c = s[i++];
        if (c == '\\' && i < s.size()) {
            out.push_back(s[i++]);
        } else if (c == '"') {
            return out;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string hexBytes(const std::string& list) {
    std::string out;
    for (const auto& part : split(list, ',', true)) {
        std::string h = trim(part);
        if (h.empty()) continue;
        out.push_back(static_cast<char>(std::stoi(h, nullptr, 16)));
    }
    return out;
}

std::string utf16leToUtf8(const std::string& bytes) {
    std::wstring w(bytes.size() / 2, L'\0');
    memcpy(w.data(), bytes.data(), w.size() * 2);
    while (!w.empty() && w.back() == L'\0') w.pop_back();
    return narrow(w);
}

}  // namespace

std::vector<RegKey> parseRegFile(std::string_view fileBytes) {
    const std::string text = decodeText(fileBytes);
    // Join continuation lines (trailing backslash) and split into logical lines.
    std::vector<std::string> lines;
    std::string cur;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = trim(line);
        if (!cur.empty()) cur += t;
        else cur = line;
        if (!cur.empty() && cur.back() == '\\' && cur.find('=') != std::string::npos && cur.front() != '[') {
            cur.pop_back();
        } else {
            lines.push_back(cur);
            cur.clear();
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (!cur.empty()) lines.push_back(cur);

    size_t first = 0;
    while (first < lines.size() && trim(lines[first]).empty()) ++first;
    if (first >= lines.size()) throw std::runtime_error("The file is empty");
    const std::string header = trim(lines[first]);
    if (header != "Windows Registry Editor Version 5.00" && header != "REGEDIT4")
        throw std::runtime_error("Not a registry export file (.reg)");

    std::vector<RegKey> keys;
    RegKey* current = nullptr;
    for (size_t i = first + 1; i < lines.size(); ++i) {
        const std::string line = trim(lines[i]);
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[') {
            current = nullptr;
            if (line.size() > 1 && line[1] == '-') continue;  // deletion
            size_t close = line.rfind(']');
            keys.push_back({line.substr(1, close == std::string::npos ? std::string::npos : close - 1), {}});
            current = &keys.back();
            continue;
        }
        if (!current) continue;
        std::string name;
        size_t pos = 0;
        if (line[0] == '"') {
            name = parseQuoted(line, pos);
        } else if (line[0] == '@') {
            pos = 1;
        } else {
            continue;
        }
        while (pos < line.size() && line[pos] != '=') ++pos;
        if (pos >= line.size()) continue;
        ++pos;
        while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
        const std::string data = line.substr(pos);
        RegValue v;
        try {
            if (!data.empty() && data[0] == '"') {
                size_t p = 0;
                v.type = RegValue::Type::String;
                v.str = parseQuoted(data, p);
            } else if (startsWith(toLower(data), "dword:")) {
                v.type = RegValue::Type::Dword;
                v.num = std::stoul(data.substr(6), nullptr, 16);
            } else if (startsWith(toLower(data), "hex(2):")) {
                v.type = RegValue::Type::ExpandString;
                v.bytes = hexBytes(data.substr(7));
                v.str = utf16leToUtf8(v.bytes);
            } else if (startsWith(toLower(data), "hex(7):")) {
                v.type = RegValue::Type::MultiString;
                v.bytes = hexBytes(data.substr(7));
                std::string all = utf16leToUtf8(v.bytes);
                for (char& c : all)
                    if (c == '\0') c = '\n';
                v.str = all;
            } else if (startsWith(toLower(data), "hex(b):")) {
                v.type = RegValue::Type::Qword;
                v.bytes = hexBytes(data.substr(7));
                for (size_t b = 0; b < v.bytes.size() && b < 8; ++b)
                    v.num |= static_cast<uint64_t>(static_cast<unsigned char>(v.bytes[b])) << (8 * b);
            } else if (startsWith(toLower(data), "hex:")) {
                v.type = RegValue::Type::Binary;
                v.bytes = hexBytes(data.substr(4));
            } else {
                continue;  // "-" deletion or unsupported type
            }
        } catch (const std::exception&) {
            continue;
        }
        current->values[name] = std::move(v);
    }
    return keys;
}

std::string writeRegFile(const std::vector<RegKey>& keys) {
    std::string text = "Windows Registry Editor Version 5.00\r\n";
    auto quote = [](const std::string& s) {
        std::string out = "\"";
        for (char c : s) {
            if (c == '\\' || c == '"') out.push_back('\\');
            out.push_back(c);
        }
        return out + "\"";
    };
    for (const auto& k : keys) {
        text += "\r\n[" + k.path + "]\r\n";
        for (const auto& [name, v] : k.values) {
            text += (name.empty() ? std::string("@") : quote(name)) + "=";
            switch (v.type) {
                case RegValue::Type::Dword: {
                    char buf[16];
                    std::snprintf(buf, sizeof buf, "dword:%08x", static_cast<unsigned>(v.num));
                    text += buf;
                    break;
                }
                case RegValue::Type::String:
                default: text += quote(v.str); break;
            }
            text += "\r\n";
        }
    }
    text += "\r\n";
    std::wstring w = widen(text);
    std::string out = "\xFF\xFE";
    out.append(reinterpret_cast<const char*>(w.data()), w.size() * sizeof(wchar_t));
    return out;
}

std::string regString(const RegValues& v, const std::string& name, const std::string& def) {
    auto it = v.find(name);
    if (it == v.end()) return def;
    if (it->second.type == RegValue::Type::Dword || it->second.type == RegValue::Type::Qword) return std::to_string(it->second.num);
    return it->second.str;
}

int64_t regInt(const RegValues& v, const std::string& name, int64_t def) {
    auto it = v.find(name);
    if (it == v.end()) return def;
    if (it->second.type == RegValue::Type::Dword) return static_cast<int32_t>(static_cast<uint32_t>(it->second.num));
    if (it->second.type == RegValue::Type::Qword) return static_cast<int64_t>(it->second.num);
    try {
        return std::stoll(it->second.str);
    } catch (const std::exception&) {
        return def;
    }
}

bool regHas(const RegValues& v, const std::string& name) { return v.count(name) > 0; }

}  // namespace st::putty
