#pragma once
// Loading private keys of any supported format into libssh ssh_key objects.
// Supported: OpenSSH ("BEGIN OPENSSH PRIVATE KEY", any cipher libssh supports), PEM (RSA/EC/PKCS#8),
// PuTTY PPK v2 and v3 (unencrypted and encrypted; v3 uses Argon2 via OpenSSL 3 EVP_KDF).
#include "core/Types.h"

#include <libssh/libssh.h>

#include <functional>
#include <optional>
#include <string>

namespace sc {

/** Called when the key is encrypted; `attempt` starts at 1. Return nullopt to cancel. */
using PassphraseCallback = std::function<std::optional<std::string>(int attempt)>;

/**
 * Loads `path`. On success returns an ssh_key the caller must ssh_key_free(). On failure returns nullptr and
 * sets `error` (e.g. "Wrong passphrase", "Unsupported key format", "Cancelled"). Asks for a passphrase at most
 * 3 times.
 */
ssh_key loadPrivateKey(const std::string& path, const PassphraseCallback& askPassphrase, std::string& error);

/** Detects the key file format without decrypting: 'openssh' | 'pem' | 'ppk2' | 'ppk3' | 'unknown'. */
std::string detectKeyFormat(const std::string& fileContent);

}  // namespace sc
