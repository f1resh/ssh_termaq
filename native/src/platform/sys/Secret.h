#pragma once
// Secrets encrypted for the current OS user, as opaque binary blobs that only decrypt for that user on that system.
// Windows: DPAPI (CryptProtectData, current user scope, a constant application entropy, no UI), so blobs written by
// other applications cannot be decrypted here by accident and a blob copied to another account or machine does not
// decrypt at all. macOS: AES-256-GCM with a random key kept in the user's login Keychain (created on first use).
// Blobs of one OS do not decrypt on the other.
#include <optional>
#include <string>
#include <string_view>

namespace st::sys {

/** Encrypts `plain` (not empty). Throws std::runtime_error on failure. */
std::string protectSecret(std::string_view plain);
/** Decrypts a blob made by protectSecret(); nullopt when it cannot be decrypted here (other user, OS or corrupt). */
std::optional<std::string> unprotectSecret(std::string_view blob);
/**
 * Tests only: use this 32-byte key instead of the key the OS keeps for the user (macOS: the Keychain item), so tests
 * never touch the user's real key store; "" goes back to it. No effect where the OS needs no key (Windows: DPAPI).
 */
void useTestSecretKey(std::string key);

}  // namespace st::sys
