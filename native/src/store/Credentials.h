#pragma once
// Secret storage encrypted with DPAPI (CryptProtectData, current user scope), persisted as
// credentials.json: { "<key>": "<base64 of DPAPI blob>" }. Thread-safe.
// DPAPI calls use CRYPTPROTECT_UI_FORBIDDEN and a constant application entropy, so blobs written by other
// applications for the same user cannot be decrypted here by accident (and vice versa). Blobs are bound to the
// Windows user account: a credentials.json copied to another account/machine yields "no secret" (get -> nullopt).
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace sc {

class Credentials {
public:
    explicit Credentials(std::filesystem::path file);
    /** Loads credentials.json (missing => empty; corrupt => backed up and empty). */
    void load();

    /** Decrypted secret, nullopt when missing or undecryptable. */
    std::optional<std::string> get(const std::string& key) const;
    /** Encrypts and persists (atomic write). An empty secret removes the key. Throws std::runtime_error on failure. */
    void set(const std::string& key, const std::string& secret);
    void remove(const std::string& key);
    bool has(const std::string& key) const;

    static std::string passwordKey(const std::string& sessionId) { return "session:" + sessionId + ":password"; }
    static std::string passphraseKey(const std::string& keyPath) { return "key:" + keyPath + ":passphrase"; }

    /** DPAPI helpers (base64 in/out). protect("") == "". unprotect returns nullopt when the blob is invalid. */
    static std::string protect(const std::string& plain);
    static std::optional<std::string> unprotect(const std::string& b64);

    /** Fired after a key was set/removed (outside the lock, on the calling thread). Never receives the secret. */
    std::function<void(const std::string& key)> onChanged;

private:
    void saveLocked() const;
    std::filesystem::path file_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> blobs_;  // key -> base64 DPAPI blob
};

}  // namespace sc
