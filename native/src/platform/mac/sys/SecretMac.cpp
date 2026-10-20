// Secrets on macOS: AES-256-GCM (OpenSSL) under a random 256-bit key that lives in the user's login Keychain as a
// generic password (service "SSHTermaq", account "credentials-key"). The Keychain is only asked once per process.
//
// Blob: "STK1" | nonce (12 bytes) | ciphertext | tag (16 bytes); the magic and a constant context string are
// authenticated as associated data, so blobs of other applications or formats never decrypt by accident.
#include "platform/sys/Secret.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <mutex>
#include <stdexcept>

namespace st::sys {

namespace {

constexpr char kMagic[] = "STK1";
constexpr size_t kMagicLen = 4;
constexpr size_t kKeyLen = 32;
constexpr size_t kNonceLen = 12;
constexpr size_t kTagLen = 16;
constexpr char kContext[] = "SSHTermaq/credentials/v1";
constexpr char kService[] = "SSHTermaq";
constexpr char kAccount[] = "credentials-key";

std::mutex g_mutex;
std::string g_key;      // cached Keychain key
std::string g_testKey;  // useTestSecretKey

struct CfRelease {
    CFTypeRef ref;
    ~CfRelease() {
        if (ref) CFRelease(ref);
    }
};

CFStringRef cfString(const char* s) { return CFStringCreateWithCString(nullptr, s, kCFStringEncodingUTF8); }

std::string statusMessage(OSStatus status) {
    std::string out = "OSStatus " + std::to_string(status);
    if (CFStringRef msg = SecCopyErrorMessageString(status, nullptr)) {
        char buf[256];
        if (CFStringGetCString(msg, buf, sizeof buf, kCFStringEncodingUTF8)) out = buf;
        CFRelease(msg);
    }
    return out;
}

/** The base query for the key's Keychain item. */
CFMutableDictionaryRef keyQuery() {
    CFMutableDictionaryRef q =
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef service = cfString(kService), account = cfString(kAccount);
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, service);
    CFDictionarySetValue(q, kSecAttrAccount, account);
    CFRelease(service);
    CFRelease(account);
    return q;
}

/** Reads the key from the Keychain: errSecSuccess with `key` set, errSecItemNotFound, or another error. */
OSStatus readKey(std::string& key) {
    CFMutableDictionaryRef q = keyQuery();
    CfRelease qr{q};
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef data = nullptr;
    const OSStatus status = SecItemCopyMatching(q, &data);
    if (status != errSecSuccess) return status;
    CfRelease dr{data};
    if (CFGetTypeID(data) != CFDataGetTypeID() || CFDataGetLength(static_cast<CFDataRef>(data)) != kKeyLen)
        return errSecDecode;
    key.assign(reinterpret_cast<const char*>(CFDataGetBytePtr(static_cast<CFDataRef>(data))), kKeyLen);
    return errSecSuccess;
}

OSStatus addKey(const std::string& key) {
    CFMutableDictionaryRef q = keyQuery();
    CfRelease qr{q};
    CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(key.data()), static_cast<CFIndex>(key.size()));
    CfRelease dr{data};
    CFStringRef label = cfString("SSHTermaq credentials key");
    CfRelease lr{label};
    CFDictionarySetValue(q, kSecValueData, data);
    CFDictionarySetValue(q, kSecAttrLabel, label);
    return SecItemAdd(q, nullptr);
}

/**
 * The key to use: the test key, else the Keychain's (created when `create` and missing). Empty with `error` set when
 * the Keychain cannot be used (locked and no UI allowed, access denied, ...).
 */
std::string currentKey(bool create, std::string& error) {
    std::lock_guard lock(g_mutex);
    if (!g_testKey.empty()) return g_testKey;
    if (!g_key.empty()) return g_key;
    std::string key;
    OSStatus status = readKey(key);
    if (status == errSecItemNotFound && create) {
        std::string fresh(kKeyLen, '\0');
        if (SecRandomCopyBytes(kSecRandomDefault, kKeyLen, fresh.data()) != errSecSuccess) {
            error = "no random bytes for a new key";
            return {};
        }
        status = addKey(fresh);
        if (status == errSecSuccess) key = fresh;
        else if (status == errSecDuplicateItem) status = readKey(key);  // another process made it meanwhile
        OPENSSL_cleanse(fresh.data(), fresh.size());
    }
    if (status != errSecSuccess) {
        error = statusMessage(status);
        return {};
    }
    g_key = key;
    OPENSSL_cleanse(key.data(), key.size());
    return g_key;
}

struct CipherCtx {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    ~CipherCtx() { EVP_CIPHER_CTX_free(ctx); }
};

}  // namespace

std::string protectSecret(std::string_view plain) {
    std::string error;
    std::string key = currentKey(true, error);
    if (key.empty()) throw std::runtime_error("Cannot encrypt secret (Keychain: " + error + ")");

    std::string out(kMagic, kMagicLen);
    std::string nonce(kNonceLen, '\0');
    if (SecRandomCopyBytes(kSecRandomDefault, kNonceLen, nonce.data()) != errSecSuccess)
        throw std::runtime_error("Cannot encrypt secret (no random bytes)");
    out += nonce;

    CipherCtx c;
    int len = 0;
    std::string cipher(plain.size(), '\0');
    unsigned char tag[kTagLen];
    unsigned char rest[16];  // GCM's final step writes nothing
    const bool ok =
        c.ctx && EVP_EncryptInit_ex(c.ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(c.ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kNonceLen), nullptr) == 1 &&
        EVP_EncryptInit_ex(c.ctx, nullptr, nullptr, reinterpret_cast<const unsigned char*>(key.data()),
                           reinterpret_cast<const unsigned char*>(nonce.data())) == 1 &&
        EVP_EncryptUpdate(c.ctx, nullptr, &len, reinterpret_cast<const unsigned char*>(kMagic), kMagicLen) == 1 &&
        EVP_EncryptUpdate(c.ctx, nullptr, &len, reinterpret_cast<const unsigned char*>(kContext), sizeof kContext - 1) == 1 &&
        EVP_EncryptUpdate(c.ctx, reinterpret_cast<unsigned char*>(cipher.data()), &len,
                          reinterpret_cast<const unsigned char*>(plain.data()), static_cast<int>(plain.size())) == 1 &&
        EVP_EncryptFinal_ex(c.ctx, rest, &len) == 1 &&
        EVP_CIPHER_CTX_ctrl(c.ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(kTagLen), tag) == 1;
    OPENSSL_cleanse(key.data(), key.size());
    if (!ok) throw std::runtime_error("Cannot encrypt secret (AES-GCM)");
    out += cipher;
    out.append(reinterpret_cast<const char*>(tag), kTagLen);
    return out;
}

std::optional<std::string> unprotectSecret(std::string_view blob) {
    if (blob.size() < kMagicLen + kNonceLen + kTagLen || blob.substr(0, kMagicLen) != std::string_view(kMagic, kMagicLen))
        return std::nullopt;
    std::string error;
    std::string key = currentKey(false, error);
    if (key.empty()) return std::nullopt;

    const std::string_view nonce = blob.substr(kMagicLen, kNonceLen);
    const std::string_view cipher = blob.substr(kMagicLen + kNonceLen, blob.size() - kMagicLen - kNonceLen - kTagLen);
    std::string tag(blob.substr(blob.size() - kTagLen));

    CipherCtx c;
    int len = 0;
    std::string plain(cipher.size(), '\0');
    unsigned char rest[16];  // GCM's final step writes nothing
    const bool ok =
        c.ctx && EVP_DecryptInit_ex(c.ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(c.ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kNonceLen), nullptr) == 1 &&
        EVP_DecryptInit_ex(c.ctx, nullptr, nullptr, reinterpret_cast<const unsigned char*>(key.data()),
                           reinterpret_cast<const unsigned char*>(nonce.data())) == 1 &&
        EVP_DecryptUpdate(c.ctx, nullptr, &len, reinterpret_cast<const unsigned char*>(kMagic), kMagicLen) == 1 &&
        EVP_DecryptUpdate(c.ctx, nullptr, &len, reinterpret_cast<const unsigned char*>(kContext), sizeof kContext - 1) == 1 &&
        EVP_DecryptUpdate(c.ctx, reinterpret_cast<unsigned char*>(plain.data()), &len,
                          reinterpret_cast<const unsigned char*>(cipher.data()), static_cast<int>(cipher.size())) == 1 &&
        EVP_CIPHER_CTX_ctrl(c.ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(kTagLen), tag.data()) == 1 &&
        EVP_DecryptFinal_ex(c.ctx, rest, &len) == 1;
    OPENSSL_cleanse(key.data(), key.size());
    if (!ok) {
        OPENSSL_cleanse(plain.data(), plain.size());
        return std::nullopt;
    }
    return plain;
}

void useTestSecretKey(std::string key) {
    if (!key.empty() && key.size() != kKeyLen) throw std::invalid_argument("test secret key must be 32 bytes");
    std::lock_guard lock(g_mutex);
    OPENSSL_cleanse(g_testKey.data(), g_testKey.size());
    g_testKey = std::move(key);
}

}  // namespace st::sys
