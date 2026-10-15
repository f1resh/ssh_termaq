#include "store/Credentials.h"

#include "core/Crypto.h"
#include "core/Json.h"
#include "core/Log.h"
#include "core/Strings.h"
#include "core/Win.h"
#include "store/JsonFile.h"

#include <wincrypt.h>
#include <dpapi.h>

#include <map>
#include <stdexcept>

namespace st {

namespace {

// Constant application entropy mixed into every DPAPI blob (see header). Changing it invalidates stored secrets.
constexpr char kEntropy[] = "SSHTermaq/credentials/v1:6f1d9c2e-4b7a-4e15-9a3c-1d2b8e7f0a64";

DATA_BLOB entropyBlob() {
    DATA_BLOB b{};
    b.cbData = static_cast<DWORD>(sizeof(kEntropy) - 1);
    b.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy));
    return b;
}

/** Frees a LocalAlloc'ed DPAPI output blob, wiping it first. */
struct BlobGuard {
    DATA_BLOB& blob;
    bool wipe;
    ~BlobGuard() {
        if (blob.pbData) {
            if (wipe) SecureZeroMemory(blob.pbData, blob.cbData);
            LocalFree(blob.pbData);
            blob.pbData = nullptr;
        }
    }
};

}  // namespace

Credentials::Credentials(std::filesystem::path file) : file_(std::move(file)) {}

void Credentials::load() {
    Json root;
    auto status = storeio::loadJson(file_, root);
    std::unordered_map<std::string, std::string> blobs;
    if (status == storeio::LoadStatus::Ok) {
        if (!root.is_object()) {
            storeio::backupCorrupt(file_, "root is not an object");
        } else {
            for (auto it = root.begin(); it != root.end(); ++it)
                if (it.value().is_string() && !it.value().get_ref<const std::string&>().empty())
                    blobs[it.key()] = it.value().get<std::string>();
        }
    }
    std::lock_guard lock(mutex_);
    blobs_ = std::move(blobs);
}

std::optional<std::string> Credentials::get(const std::string& key) const {
    std::string blob;
    {
        std::lock_guard lock(mutex_);
        auto it = blobs_.find(key);
        if (it == blobs_.end()) return std::nullopt;
        blob = it->second;
    }
    auto plain = unprotect(blob);
    if (!plain) applog::warn("Credentials: stored secret could not be decrypted (different Windows user or corrupt entry)");
    return plain;
}

void Credentials::set(const std::string& key, const std::string& secret) {
    if (key.empty()) throw std::invalid_argument("Credential key is empty");
    if (secret.empty()) {
        remove(key);
        return;
    }
    std::string blob = protect(secret);  // DPAPI outside the lock
    {
        std::lock_guard lock(mutex_);
        auto it = blobs_.find(key);
        std::optional<std::string> previous;
        if (it != blobs_.end()) previous = it->second;
        blobs_[key] = blob;
        try {
            saveLocked();
        } catch (...) {
            if (previous)
                blobs_[key] = *previous;
            else
                blobs_.erase(key);
            throw;
        }
    }
    if (onChanged) onChanged(key);
}

void Credentials::remove(const std::string& key) {
    {
        std::lock_guard lock(mutex_);
        auto it = blobs_.find(key);
        if (it == blobs_.end()) return;
        std::string previous = it->second;
        blobs_.erase(it);
        try {
            saveLocked();
        } catch (...) {
            blobs_[key] = previous;
            throw;
        }
    }
    if (onChanged) onChanged(key);
}

bool Credentials::has(const std::string& key) const {
    std::lock_guard lock(mutex_);
    return blobs_.count(key) != 0;
}

std::string Credentials::protect(const std::string& plain) {
    if (plain.empty()) return {};
    DATA_BLOB in{};
    in.cbData = static_cast<DWORD>(plain.size());
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
    DATA_BLOB entropy = entropyBlob();
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"SSHTermaq", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        throw std::runtime_error("Cannot encrypt secret (CryptProtectData): " + lastErrorMessage());
    BlobGuard guard{out, false};
    return crypto::base64Encode(std::string_view(reinterpret_cast<const char*>(out.pbData), out.cbData));
}

std::optional<std::string> Credentials::unprotect(const std::string& b64) {
    if (b64.empty()) return std::string();
    std::string blob;
    if (!crypto::base64Decode(b64, blob) || blob.empty()) return std::nullopt;
    DATA_BLOB in{};
    in.cbData = static_cast<DWORD>(blob.size());
    in.pbData = reinterpret_cast<BYTE*>(blob.data());
    DATA_BLOB entropy = entropyBlob();
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return std::nullopt;
    BlobGuard guard{out, true};
    return std::string(reinterpret_cast<const char*>(out.pbData), out.cbData);
}

void Credentials::saveLocked() const {
    // Sorted output keeps the file diff-friendly.
    std::map<std::string, std::string> sorted(blobs_.begin(), blobs_.end());
    Json root = Json::object();
    for (const auto& [k, v] : sorted) root[k] = v;
    storeio::saveJson(file_, root);
}

}  // namespace st
