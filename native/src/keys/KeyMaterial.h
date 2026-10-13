#pragma once
// Format-neutral private key material and the OpenSSH "openssh-key-v1" container.
//
// KeyMaterial stores a key the way PuTTY does: the SSH wire-format public blob plus PuTTY's private blob
// (ssh-rsa: d,p,q,iqmp | ssh-dss: x | ecdsa: d | ssh-ed25519/ssh-ed448: string(seed)). Conversions to and from
// the OpenSSH private-section layout are pure byte re-ordering, so every key type PuTTY supports
// (rsa, dss, ecdsa nistp256/384/521, ed25519, ed448) round-trips even when libssh itself cannot use it.
#include <string>
#include <string_view>

namespace sc::keys {

struct KeyMaterial {
    std::string type;        // "ssh-ed25519" | "ssh-rsa" | "ecdsa-sha2-nistp256" ... | "ssh-dss" | "ssh-ed448"
    std::string publicBlob;  // SSH wire-format public key blob
    std::string ppkPrivate;  // PuTTY private blob, unpadded (secret; wiped on destruction)
    std::string comment;

    KeyMaterial() = default;
    KeyMaterial(const KeyMaterial&) = default;
    KeyMaterial& operator=(const KeyMaterial&) = default;
    KeyMaterial(KeyMaterial&&) noexcept = default;
    KeyMaterial& operator=(KeyMaterial&&) noexcept = default;
    ~KeyMaterial();
};

/** True for the key types this module understands (PuTTY's set). */
bool isKnownKeyType(std::string_view type);
/** True when libssh 0.12 can load the type (no ssh-dss, no ssh-ed448). */
bool libsshSupportsKeyType(std::string_view type);

/** Checks that `publicBlob` is well-formed for its type (and that it starts with `expectedType` if non-empty). */
bool validatePublicBlob(std::string_view publicBlob, std::string_view expectedType, std::string& error);
/** Checks the PuTTY private blob is well-formed for the material's type (and consistent with the public blob). */
bool validateMaterial(const KeyMaterial& m, std::string& error);

/** Bytes of a (possibly padded) PuTTY private blob that hold the key fields; 0 when malformed. */
size_t ppkPrivateLength(std::string_view type, std::string_view blob);

/** "type base64 comment" (comment omitted when empty). */
std::string publicKeyLine(std::string_view publicBlob, std::string_view comment);

/* ---- openssh-key-v1 ---------------------------------------------------- */

struct OpenSshContainer {
    std::string cipher;          // "none" when unencrypted
    std::string kdf;             // "none" | "bcrypt"
    std::string kdfOptions;
    std::string publicBlob;      // first (only) public key
    std::string privateSection;  // encrypted unless cipher == "none"
    bool encrypted() const { return cipher != "none"; }
};

/** Parses "-----BEGIN OPENSSH PRIVATE KEY-----" armored text (does not decrypt). */
bool parseOpenSshContainer(std::string_view text, OpenSshContainer& out, std::string& error);
/** Extracts the key from an UNENCRYPTED container (checkints verified, public blob cross-checked). */
bool decodeOpenSshPrivate(const OpenSshContainer& c, KeyMaterial& out, std::string& error);
/** Serializes material as an unencrypted, armored openssh-key-v1 private key (importable by libssh/OpenSSH). */
bool encodeOpenSshPrivate(const KeyMaterial& m, std::string& armoredOut, std::string& error);

}  // namespace sc::keys
