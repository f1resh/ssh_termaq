#pragma once
// Secret storage encrypted with DPAPI (CryptProtectData, current user scope), persisted as
// credentials.json: { "<key>": "<base64 of DPAPI blob>" }. Thread-safe.
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace sc {

class Credentials {
public:
    explicit Credentials(std::filesystem::path file);
    void load();

    std::optional<std::string> get(const std::string& key) const;
    void set(const std::string& key, const std::string& secret);
    void remove(const std::string& key);
    bool has(const std::string& key) const;

    static std::string passwordKey(const std::string& sessionId) { return "session:" + sessionId + ":password"; }
    static std::string passphraseKey(const std::string& keyPath) { return "key:" + keyPath + ":passphrase"; }

    /** DPAPI helpers (base64 in/out). protect("") == "". unprotect returns nullopt when the blob is invalid. */
    static std::string protect(const std::string& plain);
    static std::optional<std::string> unprotect(const std::string& b64);

private:
    void saveLocked() const;
    std::filesystem::path file_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> blobs_;  // key -> base64 DPAPI blob
};

}  // namespace sc
