#pragma once
// Key-file level helpers shared by KeyLoader and Keygen: format detection, reading key files, loading any
// supported private key into KeyMaterial, and bridging KeyMaterial <-> libssh ssh_key.
#include "keys/KeyLoader.h"
#include "keys/KeyMaterial.h"

#include <libssh/libssh.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace sc::keys {

struct SshKeyDeleter {
    void operator()(ssh_key k) const { ssh_key_free(k); }
};
/** Owning ssh_key handle. */
using SshKeyPtr = std::unique_ptr<std::remove_pointer_t<ssh_key>, SshKeyDeleter>;

enum class KeyFormat { Unknown, OpenSsh, Pem, Ppk2, Ppk3 };

KeyFormat detectFormat(std::string_view content);
/** 'openssh' | 'pem' | 'ppk2' | 'ppk3' | 'unknown' (KeyInfo::format strings). */
std::string formatName(KeyFormat f);
/** Whether the key file is passphrase-protected (determined from headers only; no decryption). */
bool isEncrypted(std::string_view content, KeyFormat f);

/** Reads a whole file (UTF-8 path). nullopt when missing/unreadable. */
std::optional<std::string> readKeyFile(const std::string& path);

/** Imports OpenSSH/PEM private key text with libssh (passphrase may be null for unencrypted keys). */
SshKeyPtr importPrivateKeyText(const std::string& content, const char* passphrase);

/** Extracts the private material of a libssh private key (via an unencrypted OpenSSH export; comment = ""). */
bool materialFromSshKey(ssh_key key, KeyMaterial& out, std::string& error);
/** Builds a libssh private key from material (only types libsshSupportsKeyType()). */
SshKeyPtr sshKeyFromMaterial(const KeyMaterial& m, std::string& error);
/** SSH wire-format public blob of a libssh key. */
bool publicBlobOfSshKey(ssh_key key, std::string& blob);

struct LoadedKey {
    KeyMaterial material;
    KeyFormat format = KeyFormat::Unknown;
    bool encrypted = false;
    std::string passphrase;  // the passphrase that worked ('' when unencrypted); wiped on destruction

    LoadedKey() = default;
    LoadedKey(const LoadedKey&) = delete;
    LoadedKey& operator=(const LoadedKey&) = delete;
    ~LoadedKey();
};

/**
 * Loads any supported private key file content into material. Encrypted keys ask `askPassphrase` up to
 * `maxAttempts` times (error "Cancelled" when it returns nullopt, "Wrong passphrase" after the last failure).
 * For encrypted OpenSSH keys the comment is taken from a "<path>.pub" sidecar when `path` is given.
 */
bool loadKeyMaterial(const std::string& content, const std::string& path, const PassphraseCallback& askPassphrase,
                     int maxAttempts, LoadedKey& out, std::string& error);

/** Public blob (and comment) of the "<path>.pub" sidecar file, if present and well-formed. */
std::optional<std::string> sidecarPublicKey(const std::string& path, std::string* comment);

}  // namespace sc::keys
