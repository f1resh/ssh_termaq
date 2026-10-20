#pragma once
// PuTTYgen equivalent: key generation, inspection and conversion. All functions block (crypto + disk) and must
// run off the UI thread. Failures throw std::runtime_error with a user-readable message (never key material).
#include "core/Types.h"

#include <string>

namespace st::keys {

/**
 * Generates a key pair: type 'ed25519' | 'rsa' (bits 2048/3072/4096, default 3072) | 'ecdsa' (bits 256/384/521,
 * default 256). Outputs the OpenSSH private key (encrypted with `passphrase` when non-empty), a PPK v3 file
 * (Argon2id-encrypted when a passphrase is set), the public key line "type base64 comment" and the SHA256
 * fingerprint.
 * Note: libssh writes encrypted OpenSSH keys without a comment (its exporter has no comment parameter); the
 * unencrypted OpenSSH output and the PPK carry the comment.
 */
KeygenResult generateKey(const KeygenRequest& req);

/**
 * Describes a key file. The public part (type, fingerprint, public key line) is available without a passphrase
 * for PPK and OpenSSH-format files; for encrypted PEM files it needs `passphrase` (or a "<path>.pub" sidecar).
 * When `passphrase` is non-empty and the key is encrypted it is verified (throws "Wrong passphrase").
 */
KeyInfo inspectKey(const std::string& path, const std::string& passphrase);

/**
 * Converts a private key file to `to` = "openssh" | "ppk" (PPK v3) and returns the new file content.
 * `passphrase` decrypts the input; an encrypted input stays encrypted with the same passphrase.
 */
std::string convertKey(const std::string& path, const std::string& passphrase, const std::string& to);

}  // namespace st::keys
