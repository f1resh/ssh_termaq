#include "platform/sys/Secret.h"

#include "platform/detail/WinApi.h"
#include "platform/sys/Errors.h"

#include <wincrypt.h>
#include <dpapi.h>

#include <stdexcept>

namespace st::sys {

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

std::string protectSecret(std::string_view plain) {
    DATA_BLOB in{};
    in.cbData = static_cast<DWORD>(plain.size());
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
    DATA_BLOB entropy = entropyBlob();
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"SSHTermaq", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        throw std::runtime_error("Cannot encrypt secret (CryptProtectData): " + lastErrorMessage());
    BlobGuard guard{out, false};
    return std::string(reinterpret_cast<const char*>(out.pbData), out.cbData);
}

std::optional<std::string> unprotectSecret(std::string_view blob) {
    std::string copy(blob);
    DATA_BLOB in{};
    in.cbData = static_cast<DWORD>(copy.size());
    in.pbData = reinterpret_cast<BYTE*>(copy.data());
    DATA_BLOB entropy = entropyBlob();
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return std::nullopt;
    BlobGuard guard{out, true};
    return std::string(reinterpret_cast<const char*>(out.pbData), out.cbData);
}

void useTestSecretKey(std::string) {}  // DPAPI keeps its keys itself

}  // namespace st::sys
