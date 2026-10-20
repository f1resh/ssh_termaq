#include "putty/Putty.h"

#include "core/Crypto.h"
#include "core/Strings.h"
#include "core/Uuid.h"
#include "platform/sys/CodePage.h"
#include "platform/sys/Env.h"
#include "platform/sys/Fs.h"
#include "platform/sys/Registry.h"

#include <libssh/libssh.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace st::putty {

/* ------------------------------------------------------------------------ */
/* Names                                                                     */
/* ------------------------------------------------------------------------ */

namespace {

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string unescapeSessionName(const std::string& keyName) {
    const std::string ansi = sys::codepage::toAnsi(keyName);
    std::string raw;
    for (size_t i = 0; i < ansi.size(); ++i) {
        if (ansi[i] == '%' && i + 2 < ansi.size() && hexVal(ansi[i + 1]) >= 0 && hexVal(ansi[i + 2]) >= 0) {
            raw.push_back(static_cast<char>(hexVal(ansi[i + 1]) * 16 + hexVal(ansi[i + 2])));
            i += 2;
        } else {
            raw.push_back(ansi[i]);
        }
    }
    return sys::codepage::fromAnsi(raw);
}

std::string escapeSessionName(const std::string& name) {
    const std::string ansi = sys::codepage::toAnsi(name);
    std::string out;
    for (unsigned char c : ansi) {
        if (c == ' ' || c == '\\' || c == '*' || c == '?' || c == '%' || c < ' ' || (c > '~' && c < 0xA0)) {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return sys::codepage::fromAnsi(out);
}

/* ------------------------------------------------------------------------ */
/* Registry                                                                  */
/* ------------------------------------------------------------------------ */

namespace {

RegValues toRegValues(const std::vector<std::pair<std::string, sys::registry::Value>>& in) {
    RegValues values;
    for (const auto& [name, rv] : in) {
        RegValue v;
        switch (rv.type) {
            case sys::registry::Value::Type::Dword:
                v.type = RegValue::Type::Dword;
                v.num = rv.dword;
                break;
            case sys::registry::Value::Type::String:
                v.type = RegValue::Type::String;
                v.str = rv.str;
                break;
            case sys::registry::Value::Type::Binary:
                v.type = RegValue::Type::Binary;
                v.bytes = rv.bytes;
                break;
        }
        values[name] = std::move(v);
    }
    return values;
}

}  // namespace

std::vector<std::pair<std::string, RegValues>> readRegistrySessions() {
    std::vector<std::pair<std::string, RegValues>> out;
    for (const auto& name : sys::registry::subkeys(kSessionsKey)) {
        if (auto values = sys::registry::values(std::string(kSessionsKey) + "\\" + name))
            out.emplace_back(name, toRegValues(*values));
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> readRegistryHostKeys() {
    std::vector<std::pair<std::string, std::string>> out;
    auto values = sys::registry::values(kHostKeysKey);
    if (!values) return out;
    for (const auto& [name, v] : toRegValues(*values))
        if (v.type == RegValue::Type::String) out.emplace_back(name, v.str);
    return out;
}

/* ------------------------------------------------------------------------ */
/* PuTTY for Unix / macOS: files                                             */
/* ------------------------------------------------------------------------ */

namespace {

/** The lines of a text file (LF or CRLF), none for a missing one. */
std::vector<std::string> fileLines(const std::filesystem::path& file) {
    std::vector<std::string> out;
    const auto content = sys::readFile(file);
    if (!content) return out;
    size_t start = 0;
    while (start < content->size()) {
        size_t end = content->find('\n', start);
        if (end == std::string::npos) end = content->size();
        std::string line = content->substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(std::move(line));
        start = end + 1;
    }
    return out;
}

}  // namespace

std::optional<std::filesystem::path> unixSettingsDir() {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (const auto env = sys::getEnv("PUTTYDIR"); env && !env->empty() && fs::is_directory(sys::toPath(*env), ec))
        return sys::toPath(*env);
    const fs::path home = sys::homeDirectory();
    if (fs::is_directory(home / ".putty", ec)) return home / ".putty";
    const auto xdg = sys::getEnv("XDG_CONFIG_HOME");
    const fs::path config = (xdg && !xdg->empty() ? sys::toPath(*xdg) : home / ".config") / "putty";
    if (fs::is_directory(config, ec)) return config;
    return std::nullopt;
}

std::vector<std::pair<std::string, RegValues>> readUnixSessions(const std::filesystem::path& dir) {
    std::vector<std::pair<std::string, RegValues>> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir / "sessions", ec)) {
        if (!e.is_regular_file(ec)) continue;
        RegValues values;
        for (const auto& line : fileLines(e.path())) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            RegValue v;
            v.str = line.substr(eq + 1);
            values[line.substr(0, eq)] = std::move(v);
        }
        out.emplace_back(sys::fromPath(e.path().filename()), std::move(values));
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> readUnixHostKeys(const std::filesystem::path& dir) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& line : fileLines(dir / "sshhostkeys")) {
        const size_t space = line.find(' ');
        if (space == std::string::npos || space == 0) continue;
        out.emplace_back(line.substr(0, space), line.substr(space + 1));
    }
    return out;
}

std::string unescapeUnixSessionName(const std::string& fileName) {
    std::string out;
    for (size_t i = 0; i < fileName.size(); ++i) {
        if (fileName[i] == '%' && i + 2 < fileName.size() && hexVal(fileName[i + 1]) >= 0 && hexVal(fileName[i + 2]) >= 0) {
            out.push_back(static_cast<char>(hexVal(fileName[i + 1]) * 16 + hexVal(fileName[i + 2])));
            i += 2;
        } else {
            out.push_back(fileName[i]);
        }
    }
    return out;
}

/* ------------------------------------------------------------------------ */
/* Host keys                                                                 */
/* ------------------------------------------------------------------------ */

namespace {

void putU32(std::string& out, uint32_t v) {
    out.push_back(static_cast<char>(v >> 24));
    out.push_back(static_cast<char>(v >> 16));
    out.push_back(static_cast<char>(v >> 8));
    out.push_back(static_cast<char>(v));
}
void putString(std::string& out, const std::string& s) {
    putU32(out, static_cast<uint32_t>(s.size()));
    out += s;
}
void putMpint(std::string& out, std::string be) {
    size_t z = 0;
    while (z < be.size() && be[z] == 0) ++z;
    be.erase(0, z);
    if (!be.empty() && (static_cast<unsigned char>(be[0]) & 0x80)) be.insert(be.begin(), '\0');
    putString(out, be);
}

/** "0x1abc" -> big-endian bytes. */
bool parseHexNumber(std::string s, std::string& out) {
    s = trim(s);
    if (startsWith(s, "0x") || startsWith(s, "0X")) s = s.substr(2);
    if (s.empty()) return false;
    if (s.size() % 2) s.insert(s.begin(), '0');
    return hexDecode(s, out);
}

bool validBlob(const std::string& blob, const std::string& type) {
    ssh_key key = nullptr;
    const std::string b64 = crypto::base64Encode(blob);
    const bool ok = ssh_pki_import_pubkey_base64(b64.c_str(), ssh_key_type_from_name(type.c_str()), &key) == SSH_OK;
    if (key) ssh_key_free(key);
    return ok;
}

}  // namespace

std::optional<KnownHost> convertHostKey(const std::string& valueName, const std::string& data, std::string& reason) {
    // "<type>@<port>:<host>"
    const auto at = valueName.find('@');
    const auto colon = valueName.find(':', at == std::string::npos ? 0 : at);
    if (at == std::string::npos || colon == std::string::npos) {
        reason = "unrecognised entry name";
        return std::nullopt;
    }
    const std::string ptype = valueName.substr(0, at);
    const int port = std::atoi(valueName.substr(at + 1, colon - at - 1).c_str());
    const std::string host = valueName.substr(colon + 1);
    std::vector<std::string> parts = split(data, ',', true);

    std::string type, blob;
    if (ptype == "rsa2" && parts.size() == 2) {
        std::string e, n;
        if (!parseHexNumber(parts[0], e) || !parseHexNumber(parts[1], n)) return reason = "malformed RSA key", std::nullopt;
        type = "ssh-rsa";
        putString(blob, type);
        putMpint(blob, e);
        putMpint(blob, n);
    } else if (ptype == "dss" && parts.size() == 4) {
        type = "ssh-dss";
        putString(blob, type);
        for (const auto& p : parts) {
            std::string v;
            if (!parseHexNumber(p, v)) return reason = "malformed DSA key", std::nullopt;
            putMpint(blob, v);
        }
    } else if (ptype == "ssh-ed25519" && parts.size() == 2) {
        std::string x, y;
        if (!parseHexNumber(parts[0], x) || !parseHexNumber(parts[1], y) || y.size() > 32)
            return reason = "malformed Ed25519 key", std::nullopt;
        // Compressed Edwards point: y little-endian, top bit = x's low bit.
        std::string point(32, '\0');
        for (size_t i = 0; i < y.size(); ++i) point[i] = y[y.size() - 1 - i];
        if (!x.empty() && (static_cast<unsigned char>(x.back()) & 1)) point[31] = static_cast<char>(point[31] | 0x80);
        type = "ssh-ed25519";
        putString(blob, type);
        putString(blob, point);
    } else if (startsWith(ptype, "ecdsa-sha2-nistp")) {
        const std::string curve = ptype.substr(std::string("ecdsa-sha2-").size());
        if (parts.size() == 3) parts.erase(parts.begin());  // optional leading curve name
        const size_t len = curve == "nistp256" ? 32 : curve == "nistp384" ? 48 : curve == "nistp521" ? 66 : 0;
        std::string x, y;
        if (!len || parts.size() != 2 || !parseHexNumber(parts[0], x) || !parseHexNumber(parts[1], y) || x.size() > len ||
            y.size() > len)
            return reason = "malformed ECDSA key", std::nullopt;
        std::string point(1, '\x04');
        point += std::string(len - x.size(), '\0') + x + std::string(len - y.size(), '\0') + y;
        type = ptype;
        putString(blob, type);
        putString(blob, curve);
        putString(blob, point);
    } else {
        reason = "unsupported key type " + ptype;
        return std::nullopt;
    }
    if (type != "ssh-dss" && !validBlob(blob, type)) {
        reason = "invalid " + type + " key data";
        return std::nullopt;
    }
    KnownHost kh;
    kh.id = newUuid();
    kh.host = host;
    kh.port = port > 0 ? port : 22;
    kh.keyType = type;
    kh.key = crypto::base64Encode(blob);
    kh.fingerprint = crypto::fingerprintSha256(blob);
    kh.addedAt = nowMs();
    kh.source = "putty-import";
    return kh;
}

/* ------------------------------------------------------------------------ */
/* Settings mapping                                                          */
/* ------------------------------------------------------------------------ */

std::string charsetFromPutty(const std::string& lcp) {
    const std::string l = toLower(trim(lcp));
    if (l.empty() || startsWith(l, "utf-8") || startsWith(l, "use font encoding")) return "UTF-8";
    static const std::pair<const char*, const char*> table[] = {
        {"iso-8859-1:", "ISO-8859-1"},   {"iso-8859-2:", "ISO-8859-2"},   {"iso-8859-3:", "ISO-8859-3"},
        {"iso-8859-4:", "ISO-8859-4"},   {"iso-8859-5:", "ISO-8859-5"},   {"iso-8859-6:", "ISO-8859-6"},
        {"iso-8859-7:", "ISO-8859-7"},   {"iso-8859-8:", "ISO-8859-8"},   {"iso-8859-9:", "ISO-8859-9"},
        {"iso-8859-10:", "ISO-8859-10"}, {"iso-8859-11:", "ISO-8859-11"}, {"iso-8859-13:", "ISO-8859-13"},
        {"iso-8859-14:", "ISO-8859-14"}, {"iso-8859-15:", "ISO-8859-15"}, {"iso-8859-16:", "ISO-8859-16"},
        {"koi8-u", "KOI8-U"},            {"koi8-r", "KOI8-R"},            {"cp437", "CP437"},
        {"cp850", "CP850"},              {"cp852", "CP852"},              {"cp866", "CP866"},
        {"win1250", "windows-1250"},     {"win1251", "windows-1251"},     {"win1252", "windows-1252"},
        {"win1253", "windows-1253"},     {"win1254", "windows-1254"},     {"win1255", "windows-1255"},
        {"win1256", "windows-1256"},     {"win1257", "windows-1257"},     {"win1258", "windows-1258"},
        {"cp1250", "windows-1250"},      {"cp1251", "windows-1251"},      {"cp1252", "windows-1252"},
        {"mac roman", "macintosh"},      {"shift_jis", "Shift_JIS"},      {"euc-jp", "EUC-JP"},
        {"gbk", "GBK"},                  {"big5", "Big5"},                {"euc-kr", "EUC-KR"},
    };
    for (const auto& [prefix, name] : table)
        if (startsWith(l, prefix)) return name;
    // Bare "ISO-8859-1" style names.
    if (startsWith(l, "iso-8859-")) {
        std::string n = l.substr(0, l.find_first_of(": "));
        for (auto& c : n) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return n;
    }
    return "UTF-8";
}

namespace {

std::string charsetToPutty(const std::string& cs) {
    if (iequals(cs, "UTF-8")) return "UTF-8";
    static const std::pair<const char*, const char*> table[] = {
        {"ISO-8859-1", "ISO-8859-1:1998 (Latin-1, West Europe)"}, {"ISO-8859-2", "ISO-8859-2:1999 (Latin-2, East Europe)"},
        {"ISO-8859-5", "ISO-8859-5:1999 (Latin/Cyrillic)"},       {"ISO-8859-15", "ISO-8859-15:1999 (Latin-9, \"euro\")"},
        {"KOI8-R", "KOI8-R"},                                      {"KOI8-U", "KOI8-U"},
        {"CP437", "CP437"},                                        {"CP866", "CP866"},
        {"windows-1250", "Win1250 (Central European)"},           {"windows-1251", "Win1251 (Cyrillic)"},
        {"windows-1252", "Win1252 (Western)"},
    };
    for (const auto& [ours, theirs] : table)
        if (iequals(cs, ours)) return theirs;
    return cs;
}

/** PuTTY algorithm tokens -> libssh names. */
const std::vector<std::pair<std::string, std::vector<std::string>>>& cipherMap() {
    static const std::vector<std::pair<std::string, std::vector<std::string>>> m = {
        {"aes", {"aes256-ctr", "aes192-ctr", "aes128-ctr", "aes256-cbc", "aes192-cbc", "aes128-cbc"}},
        {"chacha20", {"chacha20-poly1305@openssh.com"}},
        {"aesgcm", {"aes256-gcm@openssh.com", "aes128-gcm@openssh.com"}},
        {"3des", {"3des-cbc"}},
        {"blowfish", {"blowfish-cbc"}},
        {"arcfour", {"arcfour256", "arcfour128"}},
        {"des", {}},
    };
    return m;
}
const std::vector<std::pair<std::string, std::vector<std::string>>>& kexMap() {
    static const std::vector<std::pair<std::string, std::vector<std::string>>> m = {
        {"ntru-curve25519", {"sntrup761x25519-sha512", "sntrup761x25519-sha512@openssh.com"}},
        {"mlkem-curve25519", {}},  // ML-KEM hybrids are not used (see SshClient)
        {"mlkem-nist", {}},
        {"ecdh", {"curve25519-sha256", "curve25519-sha256@libssh.org", "ecdh-sha2-nistp256", "ecdh-sha2-nistp384", "ecdh-sha2-nistp521"}},
        {"dh-gex-sha1", {"diffie-hellman-group-exchange-sha256", "diffie-hellman-group-exchange-sha1"}},
        {"dh-group18-sha512", {"diffie-hellman-group18-sha512"}},
        {"dh-group17-sha512", {"diffie-hellman-group17-sha512"}},
        {"dh-group16-sha512", {"diffie-hellman-group16-sha512"}},
        {"dh-group15-sha512", {"diffie-hellman-group15-sha512"}},
        {"dh-group14-sha1", {"diffie-hellman-group14-sha256", "diffie-hellman-group14-sha1"}},
        {"rsa", {}},
        {"dh-group1-sha1", {"diffie-hellman-group1-sha1"}},
    };
    return m;
}
const std::vector<std::pair<std::string, std::vector<std::string>>>& hostKeyMap() {
    static const std::vector<std::pair<std::string, std::vector<std::string>>> m = {
        {"ed448", {}},
        {"ed25519", {"ssh-ed25519"}},
        {"ecdsa", {"ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp521"}},
        {"rsa", {"rsa-sha2-512", "rsa-sha2-256", "ssh-rsa"}},
        {"dsa", {"ssh-dss"}},
    };
    return m;
}

/** Tokens before WARN equal to a known PuTTY default => keep our (empty = library) defaults. */
bool isDefaultList(const std::vector<std::string>& tokens, const std::vector<std::string>& defaults) {
    for (const auto& d : defaults)
        if (join(tokens, ",") == d) return true;
    return false;
}

std::vector<std::string> mapAlgorithms(const std::string& value, const std::vector<std::pair<std::string, std::vector<std::string>>>& map,
                                       const std::vector<std::string>& defaults, const char* what, std::vector<std::string>& warnings) {
    std::vector<std::string> tokens;
    for (const auto& t : split(value, ',', true)) {
        std::string tok = trim(t);
        if (tok == "WARN") break;
        tokens.push_back(tok);
    }
    if (tokens.empty() || isDefaultList(tokens, defaults)) return {};
    std::vector<std::string> out;
    for (const auto& tok : tokens) {
        auto it = std::find_if(map.begin(), map.end(), [&](const auto& e) { return e.first == tok; });
        if (it == map.end()) {
            warnings.push_back(std::string("Unknown ") + what + " \"" + tok + "\" ignored");
            continue;
        }
        if (it->second.empty()) {
            warnings.push_back(std::string(what) + " \"" + tok + "\" is not supported and was skipped");
            continue;
        }
        for (const auto& n : it->second)
            if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    }
    return out;
}

std::string unmapAlgorithms(const std::vector<std::string>& names, const std::vector<std::pair<std::string, std::vector<std::string>>>& map,
                            const std::string& def) {
    if (names.empty()) return def;
    std::vector<std::string> tokens;
    for (const auto& n : names)
        for (const auto& [tok, list] : map)
            if (std::find(list.begin(), list.end(), n) != list.end() && std::find(tokens.begin(), tokens.end(), tok) == tokens.end())
                tokens.push_back(tok);
    // Unlisted tokens go below WARN, like PuTTY's configuration does.
    std::string out = join(tokens, ",") + ",WARN";
    for (const auto& [tok, list] : map)
        if (std::find(tokens.begin(), tokens.end(), tok) == tokens.end()) out += "," + tok;
    return out;
}

const std::vector<std::string> kDefaultCiphers = {"aes,chacha20,aesgcm,3des", "aes,chacha20,3des", "aes,blowfish,3des"};
const std::vector<std::string> kDefaultKex = {
    "mlkem-curve25519,mlkem-nist,ntru-curve25519,ecdh,dh-gex-sha1,dh-group18-sha512,dh-group17-sha512,dh-group16-sha512,dh-group15-sha512,dh-group14-sha1,rsa",
    "ntru-curve25519,mlkem-curve25519,mlkem-nist,ecdh,dh-gex-sha1,dh-group18-sha512,dh-group17-sha512,dh-group16-sha512,dh-group15-sha512,dh-group14-sha1,rsa",
    "ntru-curve25519,ecdh,dh-gex-sha1,dh-group18-sha512,dh-group17-sha512,dh-group16-sha512,dh-group15-sha512,dh-group14-sha1,rsa",
    "ecdh,dh-gex-sha1,dh-group18-sha512,dh-group17-sha512,dh-group16-sha512,dh-group15-sha512,dh-group14-sha1,rsa",
    "ecdh,dh-gex-sha1,dh-group14-sha1,rsa"};
const std::vector<std::string> kDefaultHostKeys = {"ed448,ed25519,ecdsa,rsa,dsa", "ed25519,ecdsa,rsa,dsa"};

/** PuTTY's default palette (Colour0..Colour21). */
const int kPuttyPalette[22][3] = {{187, 187, 187}, {255, 255, 255}, {0, 0, 0},       {85, 85, 85},    {0, 0, 0},       {0, 255, 0},
                                  {0, 0, 0},       {85, 85, 85},    {187, 0, 0},     {255, 85, 85},   {0, 187, 0},     {85, 255, 85},
                                  {187, 187, 0},   {255, 255, 85},  {0, 0, 187},     {85, 85, 255},   {187, 0, 187},   {255, 85, 255},
                                  {0, 187, 187},   {85, 255, 255},  {187, 187, 187}, {255, 255, 255}};

std::string hexColor(int r, int g, int b) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", r & 255, g & 255, b & 255);
    return buf;
}

bool parseRgb(const std::string& s, int out[3]) {
    auto parts = split(s, ',', true);
    if (parts.size() != 3) return false;
    for (int i = 0; i < 3; ++i) out[i] = std::clamp(std::atoi(trim(parts[static_cast<size_t>(i)]).c_str()), 0, 255);
    return true;
}

std::vector<PortForward> parseForwardings(const std::string& value, std::vector<std::string>& warnings) {
    std::vector<PortForward> out;
    for (const auto& raw : split(value, ',', true)) {
        std::string e = trim(raw);
        if (e.empty()) continue;
        PortForward f;
        f.id = newUuid();
        size_t i = 0;
        if (e[i] == '4' || e[i] == '6') {
            f.addressFamily = e[i] == '4' ? AddressFamily::IPv4 : AddressFamily::IPv6;
            ++i;
        }
        if (i >= e.size()) continue;
        const char kind = static_cast<char>(std::toupper(static_cast<unsigned char>(e[i++])));
        if (kind == 'L') f.type = ForwardType::Local;
        else if (kind == 'R') f.type = ForwardType::Remote;
        else if (kind == 'D') f.type = ForwardType::Dynamic;
        else {
            warnings.push_back("Unrecognised port forwarding \"" + e + "\" ignored");
            continue;
        }
        std::string rest = e.substr(i);
        std::string src = rest, dst;
        if (auto eq = rest.find('='); eq != std::string::npos) {
            src = rest.substr(0, eq);
            dst = rest.substr(eq + 1);
        }
        if (auto c = src.rfind(':'); c != std::string::npos) {
            f.bindAddress = src.substr(0, c);
            src = src.substr(c + 1);
        }
        f.sourcePort = std::atoi(src.c_str());
        if (f.type != ForwardType::Dynamic) {
            auto c = dst.rfind(':');
            if (c == std::string::npos) {
                warnings.push_back("Port forwarding \"" + e + "\" has no destination port and was ignored");
                continue;
            }
            f.destHost = dst.substr(0, c);
            if (f.destHost.size() > 2 && f.destHost.front() == '[' && f.destHost.back() == ']')
                f.destHost = f.destHost.substr(1, f.destHost.size() - 2);
            f.destPort = std::atoi(dst.substr(c + 1).c_str());
        } else {
            f.destHost.clear();
        }
        out.push_back(f);
    }
    return out;
}

std::string formatForwardings(const std::vector<PortForward>& fwds) {
    std::vector<std::string> parts;
    for (const auto& f : fwds) {
        if (!f.enabled) continue;
        std::string s;
        if (f.addressFamily == AddressFamily::IPv4) s += "4";
        if (f.addressFamily == AddressFamily::IPv6) s += "6";
        s += f.type == ForwardType::Local ? "L" : f.type == ForwardType::Remote ? "R" : "D";
        if (!f.bindAddress.empty()) s += f.bindAddress + ":";
        s += std::to_string(f.sourcePort);
        if (f.type != ForwardType::Dynamic) {
            const bool v6 = f.destHost.find(':') != std::string::npos;
            s += "=" + (v6 ? "[" + f.destHost + "]" : f.destHost) + ":" + std::to_string(f.destPort);
        }
        parts.push_back(s);
    }
    return join(parts, ",");
}

}  // namespace

SessionConfig puttyToSession(const std::string& name, const RegValues& v, std::vector<std::string>& warnings) {
    SessionConfig s = makeDefaultSession();
    s.name = name;
    auto str = [&](const char* k, const std::string& d = {}) { return regString(v, k, d); };
    auto num = [&](const char* k, int64_t d) { return regInt(v, k, d); };
    auto flag = [&](const char* k, bool d) { return num(k, d ? 1 : 0) != 0; };

    // ---- Session ----
    const std::string proto = toLower(str("Protocol", "ssh"));
    if (proto == "ssh") s.protocol = Protocol::Ssh;
    else if (proto == "telnet") s.protocol = Protocol::Telnet;
    else if (proto == "rlogin") s.protocol = Protocol::Rlogin;
    else if (proto == "raw") s.protocol = Protocol::Raw;
    else if (proto == "serial") s.protocol = Protocol::Serial;
    else if (proto == "ssh-connection" || proto == "bare") {
        s.protocol = Protocol::Ssh;
        warnings.push_back("Bare ssh-connection protocol is not supported; imported as SSH");
    } else if (proto == "supdup") {
        s.protocol = Protocol::Telnet;
        warnings.push_back("SUPDUP is not supported; imported as Telnet");
    } else {
        warnings.push_back("Unknown protocol \"" + proto + "\"; imported as SSH");
    }
    std::string host = trim(str("HostName"));
    std::string user = str("UserName");
    if (auto at = host.rfind('@'); at != std::string::npos) {
        if (user.empty()) user = host.substr(0, at);
        host = host.substr(at + 1);
    }
    s.host = host;
    s.port = static_cast<int>(num("PortNumber", defaultPort(s.protocol) ? defaultPort(s.protocol) : 22));
    s.username = user;
    if (user.empty() && flag("UserNameFromEnvironment", false)) {
        s.username = sys::userName();
    }

    // ---- Connection ----
    const int64_t pingSecs = num("PingIntervalSecs", -1);
    s.connection.keepaliveSeconds = static_cast<int>(pingSecs >= 0 ? pingSecs : num("PingInterval", 0) * 60);
    s.connection.tcpNoDelay = flag("TCPNoDelay", true);
    s.connection.tcpKeepalive = flag("TCPKeepalives", false);
    const int64_t af = num("AddressFamily", 0);
    s.connection.addressFamily = af == 1 ? AddressFamily::IPv4 : af == 2 ? AddressFamily::IPv6 : AddressFamily::Auto;
    s.connection.logicalHostname = str("LogHost");
    s.connection.terminalType = str("TerminalType", "xterm");
    s.connection.terminalSpeed = str("TerminalSpeed", "38400,38400");
    s.connection.environment.clear();
    for (const auto& item : split(str("Environment"), ',', true)) {
        auto tab = item.find('\t');
        if (tab == std::string::npos) continue;
        s.connection.environment.push_back({item.substr(0, tab), item.substr(tab + 1)});
    }

    // ---- Proxy ----
    const int64_t pm = num("ProxyMethod", 0);
    switch (pm) {
        case 1: s.proxy.type = ProxyType::Socks4; break;
        case 2: s.proxy.type = ProxyType::Socks5; break;
        case 3: s.proxy.type = ProxyType::Http; break;
        case 4: s.proxy.type = ProxyType::Telnet; break;
        case 5: s.proxy.type = ProxyType::Command; break;
        case 6:
        case 7:
        case 8: s.proxy.type = ProxyType::Ssh; break;
        default: s.proxy.type = ProxyType::None;
    }
    if (pm == 7 || pm == 8) warnings.push_back("SSH proxy via exec/subsystem is imported as an SSH jump host (port forwarding)");
    s.proxy.host = str("ProxyHost", "proxy");
    s.proxy.port = static_cast<int>(num("ProxyPort", 80));
    s.proxy.username = str("ProxyUsername");
    s.proxy.password = str("ProxyPassword");
    s.proxy.excludeHosts = str("ProxyExcludeList");
    s.proxy.proxyLocalhost = flag("ProxyLocalhost", false);
    const int64_t dns = num("ProxyDNS", 1);
    s.proxy.dnsAtProxy = dns == 0 ? TriState::Off : dns == 2 ? TriState::On : TriState::Auto;
    s.proxy.telnetCommand = str("ProxyTelnetCommand", "connect %host %port\\n");
    if (s.proxy.type == ProxyType::Command) s.proxy.localCommand = s.proxy.telnetCommand;
    if (s.proxy.type == ProxyType::None) {
        s.proxy.host.clear();
        s.proxy.port = 1080;
    }

    // ---- SSH ----
    s.ssh.remoteCommand = str("RemoteCommand");
    s.ssh.noShell = flag("SshNoShell", false);
    s.ssh.allocatePty = !flag("NoPTY", false);
    s.ssh.compression = flag("Compression", false);
    s.ssh.ciphers = mapAlgorithms(str("Cipher"), cipherMap(), kDefaultCiphers, "cipher", warnings);
    s.ssh.kex = mapAlgorithms(str("KEX"), kexMap(), kDefaultKex, "key exchange", warnings);
    s.ssh.hostKeyAlgorithms = mapAlgorithms(str("HostKey"), hostKeyMap(), kDefaultHostKeys, "host key type", warnings);
    s.ssh.auth.tryAgent = flag("TryAgent", true);
    s.ssh.auth.agent = AgentKind::Auto;
    s.ssh.auth.agentForwarding = flag("AgentFwd", false);
    s.ssh.auth.keyboardInteractive = flag("AuthKI", true);
    s.ssh.auth.password = true;
    s.ssh.auth.privateKeyPath = str("PublicKeyFile");
    s.ssh.auth.showBanner = flag("SshBanner", true);
    if (!str("DetachedCertificate").empty()) warnings.push_back("SSH certificates are not supported; the certificate was ignored");
    if (num("SshProt", 3) == 0) warnings.push_back("SSH-1 is not supported; SSH-2 will be used");
    if (flag("ConnectionSharing", false)) warnings.push_back("Connection sharing is not supported");
    s.ssh.x11.enabled = flag("X11Forward", false);
    s.ssh.x11.display = str("X11Display");  // empty: automatic, as in PuTTY
    s.ssh.tunnels = parseForwardings(str("PortForwardings"), warnings);
    s.ssh.localPortsAcceptAll = flag("LocalPortAcceptAll", false);
    s.ssh.remotePortsAcceptAll = flag("RemotePortAcceptAll", false);

    // ---- Telnet / Rlogin ----
    s.telnet.passive = flag("PassiveTelnet", false);
    s.telnet.keyboardSendsSpecials = flag("TelnetKey", false);
    s.telnet.returnSendsNewline = flag("TelnetRet", true);
    s.rlogin.localUsername = str("LocalUserName");

    // ---- Serial ----
    s.serial.line = str("SerialLine", "COM1");
    s.serial.baudRate = static_cast<int>(num("SerialSpeed", 9600));
    s.serial.dataBits = static_cast<int>(std::clamp<int64_t>(num("SerialDataBits", 8), 5, 8));
    const int64_t halfbits = num("SerialStopHalfbits", 2);
    s.serial.stopBits = halfbits >= 4 ? 2.0 : halfbits == 3 ? 1.5 : 1.0;
    static const Parity parities[] = {Parity::None, Parity::Odd, Parity::Even, Parity::Mark, Parity::Space};
    s.serial.parity = parities[std::clamp<int64_t>(num("SerialParity", 0), 0, 4)];
    static const FlowControl flows[] = {FlowControl::None, FlowControl::XonXoff, FlowControl::RtsCts, FlowControl::DsrDtr};
    s.serial.flowControl = flows[std::clamp<int64_t>(num("SerialFlowControl", 1), 0, 3)];

    // ---- Terminal ----
    s.terminal.autoWrap = flag("AutoWrapMode", true);
    auto tri = [&](const char* k) {
        const int64_t x = num(k, 2);  // FORCE_ON=0, FORCE_OFF=1, AUTO=2
        return x == 0 ? TriState::On : x == 1 ? TriState::Off : TriState::Auto;
    };
    s.terminal.localEcho = tri("LocalEcho");
    s.terminal.localLineEditing = tri("LocalEdit");
    s.terminal.implicitCrInLf = flag("LFImpliesCR", false);
    s.terminal.implicitLfInCr = flag("CRImpliesLF", false);
    s.terminal.answerback = str("Answerback", "PuTTY");
    switch (num("BellType", 1)) {
        case 0: s.terminal.bell = BellMode::None; break;
        case 2: s.terminal.bell = BellMode::Visual; break;
        case 3: s.terminal.bell = BellMode::Sound; break;
        default: s.terminal.bell = BellMode::System;
    }
    s.terminal.bellSoundPath = str("BellWaveFile");
    const int64_t sb = num("ScrollbackLines", 2000);
    s.terminal.scrollbackLines = sb == 2000 ? 0 : static_cast<int>(sb);
    s.terminal.allowRemoteTitle = !flag("NoRemoteWinTitle", false);
    s.terminal.windowTitle = str("WinTitle");
    switch (num("CloseOnExit", 1)) {
        case 0: s.terminal.closeOnExit = CloseOnExit::Never; break;
        case 2: s.terminal.closeOnExit = CloseOnExit::Always; break;
        default: s.terminal.closeOnExit = CloseOnExit::Clean;
    }

    // ---- Keyboard ----
    s.keyboard.backspace = flag("BackspaceIsDelete", true) ? BackspaceKey::Del : BackspaceKey::CtrlH;
    s.keyboard.homeEnd = flag("RXVTHomeEnd", false) ? HomeEndMode::Rxvt : HomeEndMode::Standard;
    static const FunctionKeyMode fkeys[] = {FunctionKeyMode::EscN, FunctionKeyMode::Linux, FunctionKeyMode::XtermR6, FunctionKeyMode::Vt400,
                                            FunctionKeyMode::Vt100Plus, FunctionKeyMode::Sco, FunctionKeyMode::Xterm};
    s.keyboard.functionKeys = fkeys[std::clamp<int64_t>(num("LinuxFunctionKeys", 0), 0, 6)];
    s.keyboard.disableAppKeypad = flag("NoApplicationKeys", false);
    s.keyboard.disableAppCursorKeys = flag("NoApplicationCursors", false);
    s.keyboard.ctrlAltIsAltGr = flag("CtrlAltKeys", true);

    // ---- Appearance (only when different from PuTTY's defaults, so our nicer defaults apply otherwise) ----
    Json app = Json::object();
    const std::string font = str("Font", "Courier New");
    const int64_t fontPt = num("FontHeight", 10);
    if (!iequals(font, "Courier New") || fontPt != 10) {
        app["fontFamily"] = "'" + font + "', monospace";
        app["fontSize"] = static_cast<int>(std::lround(static_cast<double>(fontPt) * 96.0 / 72.0));
    }
    const int64_t cur = num("CurType", 0);
    if (cur == 1) app["cursorStyle"] = "underline";
    else if (cur == 2) app["cursorStyle"] = "bar";
    if (regHas(v, "BlinkCur")) app["cursorBlink"] = flag("BlinkCur", false);
    const int64_t bold = num("BoldAsColour", 1);
    app["boldIsBright"] = bold != 0;
    int pal[22][3];
    bool custom = false;
    for (int i = 0; i < 22; ++i) {
        std::string key = "Colour" + std::to_string(i);
        int rgb[3] = {kPuttyPalette[i][0], kPuttyPalette[i][1], kPuttyPalette[i][2]};
        if (regHas(v, key)) parseRgb(str(key.c_str()), rgb);
        for (int c = 0; c < 3; ++c) {
            pal[i][c] = rgb[c];
            if (rgb[c] != kPuttyPalette[i][c]) custom = true;
        }
    }
    if (custom) {
        auto col = [&](int i) { return hexColor(pal[i][0], pal[i][1], pal[i][2]); };
        ColorScheme cs;
        cs.id = "putty-" + newUuid().substr(0, 8);
        cs.name = name + " (PuTTY colours)";
        const int bg = pal[2][0] * 299 + pal[2][1] * 587 + pal[2][2] * 114;
        cs.variant = bg < 128000 ? "dark" : "light";
        cs.foreground = col(0);
        cs.boldForeground = col(1);
        cs.background = col(2);
        cs.cursorAccent = col(4);
        cs.cursor = col(5);
        cs.selectionBackground = col(1);
        cs.black = col(6);
        cs.brightBlack = col(7);
        cs.red = col(8);
        cs.brightRed = col(9);
        cs.green = col(10);
        cs.brightGreen = col(11);
        cs.yellow = col(12);
        cs.brightYellow = col(13);
        cs.blue = col(14);
        cs.brightBlue = col(15);
        cs.magenta = col(16);
        cs.brightMagenta = col(17);
        cs.cyan = col(18);
        cs.brightCyan = col(19);
        cs.white = col(20);
        cs.brightWhite = col(21);
        Json j = cs;
        if (cs.boldForeground.empty()) j.erase("boldForeground");
        app["customScheme"] = j;
    }
    s.appearance = app;

    // ---- Translation ----
    s.translation.charset = charsetFromPutty(str("LineCodePage"));
    s.translation.cjkAmbiguousWide = flag("CJKAmbigWide", false);

    // ---- Logging ----
    switch (num("LogType", 0)) {
        case 1: s.logging.mode = LogMode::Printable; break;
        case 2: s.logging.mode = LogMode::All; break;
        case 3:
        case 4: s.logging.mode = LogMode::SshDebug; break;
        default: s.logging.mode = LogMode::Off;
    }
    if (regHas(v, "LogFileName")) s.logging.path = str("LogFileName");
    const int64_t clash = num("LogFileClash", -1);  // LGXF_ASK=-1, LGXF_APN=0, LGXF_OVR=1
    s.logging.ifExists = clash == 1 ? LogIfExists::Overwrite : clash == 0 ? LogIfExists::Append : LogIfExists::Ask;
    s.logging.flush = flag("LogFlush", true);
    s.logging.header = flag("LogHeader", true);
    return s;
}

RegValues sessionToPutty(const SessionConfig& s) {
    RegValues v;
    auto setS = [&](const char* k, const std::string& val) {
        RegValue r;
        r.type = RegValue::Type::String;
        r.str = val;
        v[k] = r;
    };
    auto setI = [&](const char* k, int64_t val) {
        RegValue r;
        r.type = RegValue::Type::Dword;
        r.num = static_cast<uint32_t>(val);
        v[k] = r;
    };
    static const char* protos[] = {"ssh", "telnet", "rlogin", "raw", "serial"};
    setS("Protocol", protos[static_cast<int>(s.protocol)]);
    setS("HostName", s.host);
    setI("PortNumber", s.port);
    setS("UserName", s.username);
    setI("PingIntervalSecs", s.connection.keepaliveSeconds);
    setI("PingInterval", 0);
    setI("TCPNoDelay", s.connection.tcpNoDelay);
    setI("TCPKeepalives", s.connection.tcpKeepalive);
    setI("AddressFamily", s.connection.addressFamily == AddressFamily::IPv4 ? 1 : s.connection.addressFamily == AddressFamily::IPv6 ? 2 : 0);
    setS("LogHost", s.connection.logicalHostname);
    setS("TerminalType", s.connection.terminalType);
    setS("TerminalSpeed", s.connection.terminalSpeed);
    std::vector<std::string> env;
    for (const auto& e : s.connection.environment) env.push_back(e.name + "\t" + e.value);
    setS("Environment", join(env, ","));
    int pm = 0;
    switch (s.proxy.type) {
        case ProxyType::Socks4: pm = 1; break;
        case ProxyType::Socks5: pm = 2; break;
        case ProxyType::Http: pm = 3; break;
        case ProxyType::Telnet: pm = 4; break;
        case ProxyType::Command: pm = 5; break;
        case ProxyType::Ssh: pm = 6; break;
        default: pm = 0;
    }
    setI("ProxyMethod", pm);
    setS("ProxyHost", s.proxy.host);
    setI("ProxyPort", s.proxy.port);
    setS("ProxyUsername", s.proxy.username);
    setS("ProxyPassword", s.proxy.password);
    setS("ProxyExcludeList", s.proxy.excludeHosts);
    setI("ProxyLocalhost", s.proxy.proxyLocalhost);
    setI("ProxyDNS", s.proxy.dnsAtProxy == TriState::Off ? 0 : s.proxy.dnsAtProxy == TriState::On ? 2 : 1);
    setS("ProxyTelnetCommand", s.proxy.type == ProxyType::Command ? s.proxy.localCommand : s.proxy.telnetCommand);
    setS("RemoteCommand", s.ssh.remoteCommand);
    setI("SshNoShell", s.ssh.noShell);
    setI("NoPTY", !s.ssh.allocatePty);
    setI("Compression", s.ssh.compression);
    setS("Cipher", unmapAlgorithms(s.ssh.ciphers, cipherMap(), "aes,chacha20,aesgcm,3des,WARN,des,blowfish,arcfour"));
    setS("KEX", unmapAlgorithms(s.ssh.kex, kexMap(),
                                "ntru-curve25519,ecdh,dh-gex-sha1,dh-group18-sha512,dh-group17-sha512,dh-group16-sha512,"
                                "dh-group15-sha512,dh-group14-sha1,rsa,WARN,dh-group1-sha1"));
    setS("HostKey", unmapAlgorithms(s.ssh.hostKeyAlgorithms, hostKeyMap(), "ed448,ed25519,ecdsa,rsa,dsa,WARN"));
    setI("TryAgent", s.ssh.auth.tryAgent);
    setI("AgentFwd", s.ssh.auth.agentForwarding);
    setI("AuthKI", s.ssh.auth.keyboardInteractive);
    setS("PublicKeyFile", s.ssh.auth.privateKeyPath);
    setI("SshBanner", s.ssh.auth.showBanner);
    setI("X11Forward", s.ssh.x11.enabled);
    setS("X11Display", s.ssh.x11.display);
    setS("PortForwardings", formatForwardings(s.ssh.tunnels));
    setI("LocalPortAcceptAll", s.ssh.localPortsAcceptAll);
    setI("RemotePortAcceptAll", s.ssh.remotePortsAcceptAll);
    setI("PassiveTelnet", s.telnet.passive);
    setI("TelnetKey", s.telnet.keyboardSendsSpecials);
    setI("TelnetRet", s.telnet.returnSendsNewline);
    setS("LocalUserName", s.rlogin.localUsername);
    setS("SerialLine", s.serial.line);
    setI("SerialSpeed", s.serial.baudRate);
    setI("SerialDataBits", s.serial.dataBits);
    setI("SerialStopHalfbits", s.serial.stopBits >= 2 ? 4 : s.serial.stopBits > 1 ? 3 : 2);
    setI("SerialParity", static_cast<int>(s.serial.parity));
    setI("SerialFlowControl", static_cast<int>(s.serial.flowControl));
    setI("AutoWrapMode", s.terminal.autoWrap);
    auto triOut = [](TriState t) { return t == TriState::On ? 0 : t == TriState::Off ? 1 : 2; };
    setI("LocalEcho", triOut(s.terminal.localEcho));
    setI("LocalEdit", triOut(s.terminal.localLineEditing));
    setI("LFImpliesCR", s.terminal.implicitCrInLf);
    setI("CRImpliesLF", s.terminal.implicitLfInCr);
    setS("Answerback", s.terminal.answerback);
    setI("BellType", s.terminal.bell == BellMode::None ? 0 : s.terminal.bell == BellMode::Visual ? 2 : s.terminal.bell == BellMode::Sound ? 3 : 1);
    setS("BellWaveFile", s.terminal.bellSoundPath);
    setI("ScrollbackLines", s.terminal.scrollbackLines > 0 ? s.terminal.scrollbackLines : 2000);
    setI("NoRemoteWinTitle", !s.terminal.allowRemoteTitle);
    setS("WinTitle", s.terminal.windowTitle);
    setI("CloseOnExit", s.terminal.closeOnExit == CloseOnExit::Never ? 0 : s.terminal.closeOnExit == CloseOnExit::Always ? 2 : 1);
    setI("BackspaceIsDelete", s.keyboard.backspace == BackspaceKey::Del);
    setI("RXVTHomeEnd", s.keyboard.homeEnd == HomeEndMode::Rxvt);
    int fk = 6;
    switch (s.keyboard.functionKeys) {
        case FunctionKeyMode::EscN: fk = 0; break;
        case FunctionKeyMode::Linux: fk = 1; break;
        case FunctionKeyMode::XtermR6: fk = 2; break;
        case FunctionKeyMode::Vt400: fk = 3; break;
        case FunctionKeyMode::Vt100Plus: fk = 4; break;
        case FunctionKeyMode::Sco: fk = 5; break;
        default: fk = 6;
    }
    setI("LinuxFunctionKeys", fk);
    setI("NoApplicationKeys", s.keyboard.disableAppKeypad);
    setI("NoApplicationCursors", s.keyboard.disableAppCursorKeys);
    setI("CtrlAltKeys", s.keyboard.ctrlAltIsAltGr);
    setS("LineCodePage", charsetToPutty(s.translation.charset));
    setI("CJKAmbigWide", s.translation.cjkAmbiguousWide);
    static const int logTypes[] = {0, 1, 2, 3};
    setI("LogType", logTypes[static_cast<int>(s.logging.mode)]);
    setS("LogFileName", s.logging.path);
    setI("LogFileClash", s.logging.ifExists == LogIfExists::Overwrite ? 1 : s.logging.ifExists == LogIfExists::Append ? 0 : -1);
    setI("LogFlush", s.logging.flush);
    setI("LogHeader", s.logging.header);
    if (s.appearance.is_object()) {
        if (s.appearance.contains("customScheme") && s.appearance["customScheme"].is_object()) {
            const Json& cs = s.appearance["customScheme"];
            auto rgb = [&](const char* key, int idx) {
                std::string hex = cs.value(key, std::string());
                int r = kPuttyPalette[idx][0], g = kPuttyPalette[idx][1], b = kPuttyPalette[idx][2];
                if (hex.size() == 7 && hex[0] == '#') {
                    r = std::stoi(hex.substr(1, 2), nullptr, 16);
                    g = std::stoi(hex.substr(3, 2), nullptr, 16);
                    b = std::stoi(hex.substr(5, 2), nullptr, 16);
                }
                setS(("Colour" + std::to_string(idx)).c_str(), std::to_string(r) + "," + std::to_string(g) + "," + std::to_string(b));
            };
            const char* keys[22] = {"foreground", "boldForeground", "background", "background", "cursorAccent", "cursor",
                                    "black", "brightBlack", "red", "brightRed", "green", "brightGreen", "yellow", "brightYellow",
                                    "blue", "brightBlue", "magenta", "brightMagenta", "cyan", "brightCyan", "white", "brightWhite"};
            for (int i = 0; i < 22; ++i) rgb(keys[i], i);
        }
        if (s.appearance.contains("cursorStyle")) {
            const std::string cs = s.appearance.value("cursorStyle", std::string("block"));
            setI("CurType", cs == "underline" ? 1 : cs == "bar" ? 2 : 0);
        }
        if (s.appearance.contains("cursorBlink")) setI("BlinkCur", s.appearance.value("cursorBlink", false));
    }
    return v;
}

}  // namespace st::putty
