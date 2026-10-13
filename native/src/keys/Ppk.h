#pragma once
// PuTTY private key files (.ppk), format versions 2 and 3.
//
//   v2: AES-256-CBC (zero IV) keyed by SHA1(00000000||pass)||SHA1(00000001||pass); MAC = HMAC-SHA1 keyed by
//       SHA1("putty-private-key-file-mac-key"||pass) (pass = "" when unencrypted).
//   v3: Argon2id/i/d (OpenSSL 3 EVP_KDF) over the passphrase -> 80 bytes = 32 cipher key | 16 IV | 32 MAC key;
//       MAC = HMAC-SHA256 (empty MAC key when unencrypted).
//   MAC input: string(alg) string(encryption) string(comment) string(public) string(padded private plaintext).
#include "keys/KeyMaterial.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace sc::keys {

struct PpkFile {
    int version = 0;          // 2 | 3
    std::string algorithm;    // header key type, e.g. "ssh-ed25519"
    std::string encryption;   // "none" | "aes256-cbc"
    std::string comment;
    std::string publicBlob;   // decoded
    std::string privateBlob;  // decoded; ciphertext when encrypted (includes padding)
    std::string mac;          // decoded MAC bytes
    // v3 key derivation (encrypted files only)
    std::string kdf;          // "Argon2id" | "Argon2i" | "Argon2d"
    uint32_t argonMemory = 0;  // KiB
    uint32_t argonPasses = 0;
    uint32_t argonParallelism = 0;
    std::string argonSalt;  // decoded

    PpkFile() = default;
    PpkFile(const PpkFile&) = default;
    PpkFile& operator=(const PpkFile&) = default;
    PpkFile(PpkFile&&) noexcept = default;
    PpkFile& operator=(PpkFile&&) noexcept = default;
    ~PpkFile();  // wipes privateBlob (plaintext for unencrypted files)

    bool encrypted() const { return encryption != "none"; }
};

/** Parses the text form (no decryption, no MAC check). Validates structure and the public blob. */
bool parsePpk(std::string_view text, PpkFile& out, std::string& error);

enum class PpkStatus {
    Ok,
    WrongPassphrase,  // encrypted file, MAC mismatch after decryption
    Corrupt,          // unencrypted file with a bad MAC, or malformed private data
    Error             // crypto backend failure / unsupported KDF or cipher
};

/**
 * Decrypts (when encrypted) and verifies the MAC. On Ok fills `out` (type, public blob, unpadded private blob,
 * comment). `passphrase` is ignored for unencrypted files.
 */
PpkStatus decryptPpk(const PpkFile& file, std::string_view passphrase, KeyMaterial& out, std::string& error);

struct PpkWriteOptions {
    int version = 3;          // 2 | 3
    std::string passphrase;   // empty => "Encryption: none"
    std::string kdf = "Argon2id";
    uint32_t argonMemory = 8192;  // KiB (PuTTYgen default)
    uint32_t argonPasses = 13;    // PuTTYgen calibrates to ~100 ms; 13 is typical on current hardware
    uint32_t argonParallelism = 1;
};

/** Serializes `m` as a .ppk file (LF line endings, 64-column base64 like PuTTYgen). */
bool writePpk(const KeyMaterial& m, const PpkWriteOptions& opts, std::string& out, std::string& error);

}  // namespace sc::keys
