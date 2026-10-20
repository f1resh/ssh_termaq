#include "keys/KeyFile.h"

#include "core/Crypto.h"
#include "core/Strings.h"
#include "keys/Ppk.h"
#include "keys/SshWire.h"
#include "platform/sys/Fs.h"

#include <openssl/crypto.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace st::keys {

namespace {

constexpr size_t kMaxKeyFileSize = 1 << 20;  // key files are tiny; refuse anything absurd

/** Frees (and wipes) a char* allocated by libssh. */
struct LibsshCharDeleter {
    void operator()(char* p) const {
        if (!p) return;
        OPENSSL_cleanse(p, std::strlen(p));
        ssh_string_free_char(p);
    }
};
using LibsshChars = std::unique_ptr<char, LibsshCharDeleter>;

std::string_view skipBom(std::string_view s) {
    if (startsWith(s, "\xEF\xBB\xBF")) s.remove_prefix(3);
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
        s.remove_prefix(1);
    return s;
}

}  // namespace

LoadedKey::~LoadedKey() { wipe(passphrase); }

KeyFormat detectFormat(std::string_view content) {
    const std::string_view s = skipBom(content);
    if (startsWith(s, "PuTTY-User-Key-File-2:")) return KeyFormat::Ppk2;
    if (startsWith(s, "PuTTY-User-Key-File-3:")) return KeyFormat::Ppk3;
    if (s.find("-----BEGIN OPENSSH PRIVATE KEY-----") != std::string_view::npos) return KeyFormat::OpenSsh;
    for (std::string_view tag : {"-----BEGIN RSA PRIVATE KEY-----", "-----BEGIN DSA PRIVATE KEY-----",
                                 "-----BEGIN EC PRIVATE KEY-----", "-----BEGIN PRIVATE KEY-----",
                                 "-----BEGIN ENCRYPTED PRIVATE KEY-----"})
        if (s.find(tag) != std::string_view::npos) return KeyFormat::Pem;
    return KeyFormat::Unknown;
}

std::string formatName(KeyFormat f) {
    switch (f) {
        case KeyFormat::OpenSsh: return "openssh";
        case KeyFormat::Pem: return "pem";
        case KeyFormat::Ppk2: return "ppk2";
        case KeyFormat::Ppk3: return "ppk3";
        default: return "unknown";
    }
}

bool isEncrypted(std::string_view content, KeyFormat f) {
    switch (f) {
        case KeyFormat::OpenSsh: {
            OpenSshContainer c;
            std::string err;
            return parseOpenSshContainer(content, c, err) && c.encrypted();
        }
        case KeyFormat::Pem:
            return content.find("-----BEGIN ENCRYPTED PRIVATE KEY-----") != std::string_view::npos ||
                   content.find("Proc-Type: 4,ENCRYPTED") != std::string_view::npos;
        case KeyFormat::Ppk2:
        case KeyFormat::Ppk3: {
            PpkFile p;
            std::string err;
            return parsePpk(content, p, err) && p.encrypted();
        }
        default: return false;
    }
}

std::optional<std::string> readKeyFile(const std::string& path) {
    std::error_code ec;
    const std::filesystem::path p = sys::toPath(path);
    const auto size = std::filesystem::file_size(p, ec);
    if (ec || size > kMaxKeyFileSize) return std::nullopt;
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::string data(static_cast<size_t>(size), '\0');
    in.read(data.data(), static_cast<std::streamsize>(data.size()));
    if (!in && !in.eof()) return std::nullopt;
    data.resize(static_cast<size_t>(in.gcount()));
    return data;
}

SshKeyPtr importPrivateKeyText(const std::string& content, const char* passphrase) {
    ssh_key key = nullptr;
    if (ssh_pki_import_privkey_base64(content.c_str(), passphrase, nullptr, nullptr, &key) != SSH_OK) {
        if (key) ssh_key_free(key);
        return nullptr;
    }
    return SshKeyPtr(key);
}

bool publicBlobOfSshKey(ssh_key key, std::string& blob) {
    char* raw = nullptr;
    if (ssh_pki_export_pubkey_base64(key, &raw) != SSH_OK || !raw) return false;
    LibsshChars b64(raw);
    return crypto::base64Decode(b64.get(), blob);
}

bool materialFromSshKey(ssh_key key, KeyMaterial& out, std::string& error) {
    if (!key || !ssh_key_is_private(key)) {
        error = "Not a private key";
        return false;
    }
    char* raw = nullptr;
    if (ssh_pki_export_privkey_base64_format(key, nullptr, nullptr, nullptr, &raw, SSH_FILE_FORMAT_OPENSSH) != SSH_OK ||
        !raw) {
        error = "Cannot export private key";
        return false;
    }
    LibsshChars text(raw);
    OpenSshContainer c;
    if (!parseOpenSshContainer(text.get(), c, error)) return false;
    ScopedWipe wc(c.privateSection);
    return decodeOpenSshPrivate(c, out, error);
}

SshKeyPtr sshKeyFromMaterial(const KeyMaterial& m, std::string& error) {
    if (!libsshSupportsKeyType(m.type)) {
        error = "Key type " + m.type + " is not supported";
        return nullptr;
    }
    std::string armored;
    ScopedWipe wa(armored);
    if (!encodeOpenSshPrivate(m, armored, error)) return nullptr;
    SshKeyPtr key = importPrivateKeyText(armored, nullptr);
    if (!key) error = "The " + m.type + " key could not be loaded";
    return key;
}

std::optional<std::string> sidecarPublicKey(const std::string& path, std::string* comment) {
    if (path.empty()) return std::nullopt;
    auto text = readKeyFile(path + ".pub");
    if (!text) return std::nullopt;
    std::string line = trim(*text);
    if (const size_t nl = line.find_first_of("\r\n"); nl != std::string::npos) line.resize(nl);
    // "type base64 [comment]"
    const size_t s1 = line.find(' ');
    if (s1 == std::string::npos) return std::nullopt;
    const size_t s2 = line.find(' ', s1 + 1);
    std::string blob;
    if (!crypto::base64Decode(line.substr(s1 + 1, s2 == std::string::npos ? std::string::npos : s2 - s1 - 1), blob))
        return std::nullopt;
    if (crypto::keyTypeOfBlob(blob) != line.substr(0, s1)) return std::nullopt;
    if (comment) *comment = s2 == std::string::npos ? std::string() : trim(line.substr(s2 + 1));
    return blob;
}

bool loadKeyMaterial(const std::string& content, const std::string& path, const PassphraseCallback& askPassphrase,
                     int maxAttempts, LoadedKey& out, std::string& error) {
    out.format = detectFormat(content);
    out.encrypted = isEncrypted(content, out.format);
    if (maxAttempts < 1) maxAttempts = 1;

    // Runs `attemptFn(passphrase)` for up to maxAttempts passphrases. attemptFn returns 1 = ok, 0 = wrong, -1 = fatal.
    auto withPassphrases = [&](const auto& attemptFn) -> bool {
        for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
            std::optional<std::string> pass = askPassphrase ? askPassphrase(attempt) : std::nullopt;
            if (!pass) {
                error = "Cancelled";
                return false;
            }
            ScopedWipe wp(*pass);
            const int r = attemptFn(*pass);
            if (r > 0) {
                out.passphrase = *pass;
                return true;
            }
            if (r < 0) return false;
        }
        error = "Wrong passphrase";
        return false;
    };

    switch (out.format) {
        case KeyFormat::Ppk2:
        case KeyFormat::Ppk3: {
            PpkFile f;
            if (!parsePpk(content, f, error)) return false;
            auto attemptFn = [&](const std::string& pass) {
                const PpkStatus st = decryptPpk(f, pass, out.material, error);
                return st == PpkStatus::Ok ? 1 : st == PpkStatus::WrongPassphrase ? 0 : -1;
            };
            return f.encrypted() ? withPassphrases(attemptFn) : attemptFn(std::string()) > 0;
        }
        case KeyFormat::OpenSsh: {
            OpenSshContainer c;
            if (!parseOpenSshContainer(content, c, error)) return false;
            ScopedWipe wc(c.privateSection);
            if (!c.encrypted()) return decodeOpenSshPrivate(c, out.material, error);
            const std::string type = crypto::keyTypeOfBlob(c.publicBlob);
            if (!libsshSupportsKeyType(type)) {
                error = "Encrypted " + (type.empty() ? std::string("OpenSSH") : type) + " keys are not supported";
                return false;
            }
            auto attemptFn = [&](const std::string& pass) {
                SshKeyPtr key = importPrivateKeyText(content, pass.c_str());
                if (!key) return 0;
                return materialFromSshKey(key.get(), out.material, error) ? 1 : -1;
            };
            if (!withPassphrases(attemptFn)) return false;
            std::string comment;
            if (auto blob = sidecarPublicKey(path, &comment); blob && *blob == out.material.publicBlob)
                out.material.comment = comment;
            return true;
        }
        case KeyFormat::Pem: {
            auto attemptFn = [&](const std::string& pass) {
                SshKeyPtr key = importPrivateKeyText(content, out.encrypted ? pass.c_str() : nullptr);
                if (!key) {
                    if (!out.encrypted) error = "Invalid or unsupported PEM private key";
                    return out.encrypted ? 0 : -1;
                }
                return materialFromSshKey(key.get(), out.material, error) ? 1 : -1;
            };
            if (out.encrypted ? !withPassphrases(attemptFn) : attemptFn(std::string()) <= 0) return false;
            std::string comment;
            if (auto blob = sidecarPublicKey(path, &comment); blob && *blob == out.material.publicBlob)
                out.material.comment = comment;
            return true;
        }
        default:
            error = "Unsupported key format";
            return false;
    }
}

}  // namespace st::keys
