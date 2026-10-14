#include "keys/Ppk.h"

#include "core/Crypto.h"
#include "core/Strings.h"
#include "keys/SshWire.h"

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/params.h>

#include <charconv>
#include <memory>
#include <vector>

namespace st::keys {

namespace {

constexpr size_t kMaxLines = 4096;  // generous bound for Public-/Private-Lines
constexpr std::string_view kMacKeyPrefix = "putty-private-key-file-mac-key";

struct CipherCtxDeleter {
    void operator()(EVP_CIPHER_CTX* c) const { EVP_CIPHER_CTX_free(c); }
};
struct KdfDeleter {
    void operator()(EVP_KDF* k) const { EVP_KDF_free(k); }
};
struct KdfCtxDeleter {
    void operator()(EVP_KDF_CTX* c) const { EVP_KDF_CTX_free(c); }
};

bool parseU32(std::string_view s, uint32_t& v) {
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return ec == std::errc() && p == s.data() + s.size() && !s.empty();
}

/** Splits "Key: value" (exactly one space after the colon, as PuTTY writes it; extra whitespace tolerated). */
bool splitHeader(std::string_view line, std::string_view& key, std::string_view& value) {
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) return false;
    key = line.substr(0, colon);
    value = line.substr(colon + 1);
    if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    return true;
}

std::string macData(const PpkFile& f, std::string_view plainPrivate) {
    WireWriter w;
    w.string(f.algorithm);
    w.string(f.encryption);
    w.string(f.comment);
    w.string(f.publicBlob);
    w.string(plainPrivate);
    return w.take();
}

bool hmac(const EVP_MD* md, std::string_view key, std::string_view data, std::string& out) {
    unsigned char buf[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (!HMAC(md, key.empty() ? nullptr : key.data(), static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char*>(data.data()), data.size(), buf, &len))
        return false;
    out.assign(reinterpret_cast<const char*>(buf), len);
    OPENSSL_cleanse(buf, sizeof buf);
    return true;
}

bool aes256cbc(bool encrypt, std::string_view key, std::string_view iv, std::string_view in, std::string& out) {
    if (key.size() < 32 || iv.size() < 16 || in.size() % 16 != 0) return false;
    std::unique_ptr<EVP_CIPHER_CTX, CipherCtxDeleter> ctx(EVP_CIPHER_CTX_new());
    if (!ctx) return false;
    const auto* k = reinterpret_cast<const unsigned char*>(key.data());
    const auto* v = reinterpret_cast<const unsigned char*>(iv.data());
    if (EVP_CipherInit_ex(ctx.get(), EVP_aes_256_cbc(), nullptr, k, v, encrypt ? 1 : 0) != 1) return false;
    EVP_CIPHER_CTX_set_padding(ctx.get(), 0);
    out.assign(in.size(), '\0');
    int n1 = 0, n2 = 0;
    if (EVP_CipherUpdate(ctx.get(), reinterpret_cast<unsigned char*>(out.data()), &n1,
                         reinterpret_cast<const unsigned char*>(in.data()), static_cast<int>(in.size())) != 1 ||
        EVP_CipherFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(out.data()) + n1, &n2) != 1) {
        wipe(out);
        return false;
    }
    out.resize(static_cast<size_t>(n1 + n2));
    return true;
}

/** v2 cipher key: SHA1(00000000||pass) || SHA1(00000001||pass), first 32 bytes. */
std::string v2CipherKey(std::string_view pass) {
    std::string a = std::string("\0\0\0\0", 4) + std::string(pass);
    std::string b = std::string("\0\0\0\1", 4) + std::string(pass);
    std::string key = crypto::sha1(a) + crypto::sha1(b);
    wipe(a);
    wipe(b);
    key.resize(32);
    return key;
}

const char* opensslArgonName(std::string_view kdf) {
    if (kdf == "Argon2id") return "ARGON2ID";
    if (kdf == "Argon2i") return "ARGON2I";
    if (kdf == "Argon2d") return "ARGON2D";
    return nullptr;
}

/** Argon2 (RFC 9106, version 0x13) via OpenSSL 3.2+ EVP_KDF. */
bool argon2(std::string_view kdf, std::string_view pass, std::string_view salt, uint32_t memoryKiB, uint32_t passes,
            uint32_t lanes, size_t outLen, std::string& out, std::string& error) {
    const char* name = opensslArgonName(kdf);
    if (!name) {
        error = "Unsupported key derivation '" + std::string(kdf) + "'";
        return false;
    }
    if (passes == 0 || lanes == 0 || memoryKiB < 8 * lanes || memoryKiB > (1u << 22) || passes > 10000 || lanes > 255) {
        error = "Invalid Argon2 parameters";
        return false;
    }
    std::unique_ptr<EVP_KDF, KdfDeleter> k(EVP_KDF_fetch(nullptr, name, nullptr));
    if (!k) {
        error = "Argon2 is not available in this OpenSSL build";
        return false;
    }
    std::unique_ptr<EVP_KDF_CTX, KdfCtxDeleter> ctx(EVP_KDF_CTX_new(k.get()));
    if (!ctx) {
        error = "Out of memory";
        return false;
    }
    uint32_t threads = 1, version = 0x13;
    // OpenSSL rejects a NULL password pointer even with length 0; point at a dummy byte instead.
    static const char kEmpty = 0;
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD, const_cast<char*>(pass.empty() ? &kEmpty : pass.data()),
                                          pass.size()),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, const_cast<char*>(salt.data()), salt.size()),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &passes),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_LANES, &lanes),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_THREADS, &threads),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST, &memoryKiB),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_VERSION, &version),
        OSSL_PARAM_construct_end()};
    out.assign(outLen, '\0');
    if (EVP_KDF_derive(ctx.get(), reinterpret_cast<unsigned char*>(out.data()), outLen, params) != 1) {
        wipe(out);
        error = "Argon2 key derivation failed";
        return false;
    }
    return true;
}

void appendBase64Lines(std::string& out, std::string_view header, std::string_view data) {
    const std::string b64 = crypto::base64Encode(data);
    const size_t lines = (b64.size() + 63) / 64;
    out.append(header).append(": ").append(std::to_string(lines)).append("\n");
    for (size_t i = 0; i < b64.size(); i += 64) out.append(b64.substr(i, 64)).append("\n");
}

}  // namespace

PpkFile::~PpkFile() { wipe(privateBlob); }

bool parsePpk(std::string_view text, PpkFile& out, std::string& error) {
    std::vector<std::string_view> lines;
    for (size_t pos = 0; pos <= text.size();) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(pos, nl - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        lines.push_back(line);
        pos = nl + 1;
    }
    if (!lines.empty() && startsWith(lines[0], "\xEF\xBB\xBF")) lines[0].remove_prefix(3);

    PpkFile f;
    std::string_view key, value;
    if (lines.empty() || !splitHeader(lines[0], key, value)) {
        error = "Not a PuTTY key file";
        return false;
    }
    if (key == "PuTTY-User-Key-File-2")
        f.version = 2;
    else if (key == "PuTTY-User-Key-File-3")
        f.version = 3;
    else if (key == "PuTTY-User-Key-File-1" || key == "PuTTY-User-Key-File") {
        error = "PuTTY key file format version 1 is not supported (re-save it with a newer PuTTYgen)";
        return false;
    } else if (startsWith(key, "PuTTY-User-Key-File-")) {
        error = "Unsupported PuTTY key file version";
        return false;
    } else {
        error = "Not a PuTTY key file";
        return false;
    }
    f.algorithm = std::string(value);

    bool haveEnc = false, haveComment = false, havePub = false, havePriv = false, haveMac = false;
    std::string macHex, saltHex;
    for (size_t i = 1; i < lines.size(); ++i) {
        if (lines[i].empty()) continue;
        if (!splitHeader(lines[i], key, value)) {
            error = "Malformed PuTTY key file (line " + std::to_string(i + 1) + ")";
            return false;
        }
        if (key == "Public-Lines" || key == "Private-Lines") {
            uint32_t n = 0;
            if (!parseU32(value, n) || n > kMaxLines || i + n >= lines.size()) {
                error = "Malformed PuTTY key file (" + std::string(key) + ")";
                return false;
            }
            std::string b64;
            ScopedWipe wb(b64);
            for (uint32_t j = 0; j < n; ++j) b64.append(lines[++i]);
            std::string& dst = key == "Public-Lines" ? f.publicBlob : f.privateBlob;
            if (!crypto::base64Decode(b64, dst)) {
                error = "Malformed PuTTY key file (bad base64 in " + std::string(key) + ")";
                return false;
            }
            (key == "Public-Lines" ? havePub : havePriv) = true;
        } else if (key == "Encryption") {
            f.encryption = std::string(value);
            haveEnc = true;
        } else if (key == "Comment") {
            f.comment = std::string(value);
            haveComment = true;
        } else if (key == "Private-MAC") {
            macHex = std::string(value);
            haveMac = true;
        } else if (key == "Private-Hash") {
            error = "PuTTY key files with Private-Hash (format version 1) are not supported";
            return false;
        } else if (key == "Key-Derivation") {
            f.kdf = std::string(value);
        } else if (key == "Argon2-Memory") {
            if (!parseU32(value, f.argonMemory)) return error = "Malformed Argon2-Memory", false;
        } else if (key == "Argon2-Passes") {
            if (!parseU32(value, f.argonPasses)) return error = "Malformed Argon2-Passes", false;
        } else if (key == "Argon2-Parallelism") {
            if (!parseU32(value, f.argonParallelism)) return error = "Malformed Argon2-Parallelism", false;
        } else if (key == "Argon2-Salt") {
            saltHex = std::string(value);
        }
        // Unknown headers are ignored (forward compatibility).
    }
    if (!haveEnc || !haveComment || !havePub || !havePriv || !haveMac) {
        error = "Incomplete PuTTY key file";
        return false;
    }
    if (f.encryption != "none" && f.encryption != "aes256-cbc") {
        error = "Unsupported PuTTY key encryption '" + f.encryption + "'";
        return false;
    }
    if (!hexDecode(macHex, f.mac) || f.mac.size() != (f.version == 2 ? 20u : 32u)) {
        error = "Malformed Private-MAC";
        return false;
    }
    if (f.version == 3 && f.encrypted()) {
        if (!opensslArgonName(f.kdf)) {
            error = f.kdf.empty() ? "Missing Key-Derivation" : "Unsupported key derivation '" + f.kdf + "'";
            return false;
        }
        if (!hexDecode(saltHex, f.argonSalt) || f.argonSalt.empty() || f.argonMemory == 0 || f.argonPasses == 0 ||
            f.argonParallelism == 0) {
            error = "Malformed Argon2 parameters";
            return false;
        }
    }
    if (f.encrypted() && f.privateBlob.size() % 16 != 0) {
        error = "Malformed encrypted private key";
        return false;
    }
    if (!isKnownKeyType(f.algorithm)) {
        error = "Unsupported key type '" + f.algorithm + "'";
        return false;
    }
    if (!validatePublicBlob(f.publicBlob, f.algorithm, error)) return false;
    out = std::move(f);
    return true;
}

PpkStatus decryptPpk(const PpkFile& f, std::string_view passphrase, KeyMaterial& out, std::string& error) {
    std::string plain, macKey, mac;
    ScopedWipe w1(plain), w2(macKey);
    if (!f.encrypted()) passphrase = {};

    if (f.version == 2) {
        if (f.encrypted()) {
            std::string key = v2CipherKey(passphrase);
            ScopedWipe wk(key);
            if (!aes256cbc(false, key, std::string(16, '\0'), f.privateBlob, plain)) {
                error = "Decryption failed";
                return PpkStatus::Error;
            }
        } else {
            plain = f.privateBlob;
        }
        std::string seed = std::string(kMacKeyPrefix) + std::string(passphrase);
        macKey = crypto::sha1(seed);
        wipe(seed);
        if (!hmac(EVP_sha1(), macKey, macData(f, plain), mac)) {
            error = "HMAC failed";
            return PpkStatus::Error;
        }
    } else {
        if (f.encrypted()) {
            std::string derived;
            ScopedWipe wd(derived);
            if (!argon2(f.kdf, passphrase, f.argonSalt, f.argonMemory, f.argonPasses, f.argonParallelism, 80, derived,
                        error))
                return PpkStatus::Error;
            if (!aes256cbc(false, std::string_view(derived).substr(0, 32), std::string_view(derived).substr(32, 16),
                           f.privateBlob, plain)) {
                error = "Decryption failed";
                return PpkStatus::Error;
            }
            macKey = derived.substr(48, 32);
        } else {
            plain = f.privateBlob;
        }
        if (!hmac(EVP_sha256(), macKey, macData(f, plain), mac)) {
            error = "HMAC failed";
            return PpkStatus::Error;
        }
    }

    if (mac.size() != f.mac.size() || CRYPTO_memcmp(mac.data(), f.mac.data(), mac.size()) != 0) {
        if (f.encrypted()) {
            error = "Wrong passphrase";
            return PpkStatus::WrongPassphrase;
        }
        error = "Corrupt key file (MAC mismatch)";
        return PpkStatus::Corrupt;
    }

    const size_t used = ppkPrivateLength(f.algorithm, plain);
    if (used == 0) {
        error = "Malformed private key data";
        return PpkStatus::Corrupt;
    }
    KeyMaterial m;
    m.type = f.algorithm;
    m.publicBlob = f.publicBlob;
    m.ppkPrivate.assign(plain.data(), used);
    m.comment = f.comment;
    if (!validateMaterial(m, error)) return PpkStatus::Corrupt;
    out = std::move(m);
    return PpkStatus::Ok;
}

bool writePpk(const KeyMaterial& m, const PpkWriteOptions& opts, std::string& out, std::string& error) {
    if (opts.version != 2 && opts.version != 3) {
        error = "Unsupported PPK version";
        return false;
    }
    if (!validateMaterial(m, error)) return false;

    PpkFile f;
    f.version = opts.version;
    f.algorithm = m.type;
    f.encryption = opts.passphrase.empty() ? "none" : "aes256-cbc";
    f.comment = replaceAll(replaceAll(m.comment, "\r", " "), "\n", " ");
    f.publicBlob = m.publicBlob;

    // Pad to the cipher block size with bytes of SHA1(private) (as PuTTYgen does) when encrypting.
    std::string plain = m.ppkPrivate;
    ScopedWipe wp(plain);
    if (f.encrypted()) {
        const size_t padLen = (16 - plain.size() % 16) % 16;
        std::string h = crypto::sha1(m.ppkPrivate);
        plain.append(h.substr(0, padLen));
        wipe(h);
    }

    std::string macKey, cipherKey, iv, derived;
    ScopedWipe w1(macKey), w2(cipherKey), w3(iv), w4(derived);
    if (opts.version == 2) {
        if (f.encrypted()) {
            cipherKey = v2CipherKey(opts.passphrase);
            iv.assign(16, '\0');
        }
        std::string seed = std::string(kMacKeyPrefix) + opts.passphrase;
        macKey = crypto::sha1(seed);
        wipe(seed);
    } else if (f.encrypted()) {
        f.kdf = opts.kdf;
        f.argonMemory = opts.argonMemory;
        f.argonPasses = opts.argonPasses;
        f.argonParallelism = opts.argonParallelism;
        f.argonSalt = crypto::randomBytes(16);
        if (!argon2(f.kdf, opts.passphrase, f.argonSalt, f.argonMemory, f.argonPasses, f.argonParallelism, 80, derived,
                    error))
            return false;
        cipherKey = derived.substr(0, 32);
        iv = derived.substr(32, 16);
        macKey = derived.substr(48, 32);
    }

    if (!hmac(opts.version == 2 ? EVP_sha1() : EVP_sha256(), macKey, macData(f, plain), f.mac)) {
        error = "HMAC failed";
        return false;
    }
    if (f.encrypted()) {
        if (!aes256cbc(true, cipherKey, iv, plain, f.privateBlob)) {
            error = "Encryption failed";
            return false;
        }
    } else {
        f.privateBlob = plain;
    }

    std::string s;
    s.append("PuTTY-User-Key-File-").append(std::to_string(f.version)).append(": ").append(f.algorithm).append("\n");
    s.append("Encryption: ").append(f.encryption).append("\n");
    s.append("Comment: ").append(f.comment).append("\n");
    appendBase64Lines(s, "Public-Lines", f.publicBlob);
    if (f.version == 3 && f.encrypted()) {
        s.append("Key-Derivation: ").append(f.kdf).append("\n");
        s.append("Argon2-Memory: ").append(std::to_string(f.argonMemory)).append("\n");
        s.append("Argon2-Passes: ").append(std::to_string(f.argonPasses)).append("\n");
        s.append("Argon2-Parallelism: ").append(std::to_string(f.argonParallelism)).append("\n");
        s.append("Argon2-Salt: ").append(hexEncode(f.argonSalt)).append("\n");
    }
    appendBase64Lines(s, "Private-Lines", f.privateBlob);
    s.append("Private-MAC: ").append(hexEncode(f.mac)).append("\n");
    out = std::move(s);
    return true;
}

}  // namespace st::keys
