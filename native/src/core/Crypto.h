#pragma once
// Small crypto/encoding helpers on top of OpenSSL.
#include <string>
#include <string_view>

namespace sc::crypto {

std::string sha256(std::string_view data);
std::string sha1(std::string_view data);
std::string md5(std::string_view data);
std::string randomBytes(size_t n);

std::string base64Encode(std::string_view data, bool pad = true);
/** Accepts padded/unpadded input, ignores whitespace. Returns false on invalid characters. */
bool base64Decode(std::string_view b64, std::string& out);

/** OpenSSH-style fingerprint of an SSH wire-format public key blob: "SHA256:<unpadded base64>". */
std::string fingerprintSha256(std::string_view keyBlob);
/** Legacy fingerprint: "MD5:aa:bb:..." */
std::string fingerprintMd5(std::string_view keyBlob);

/** Reads the leading SSH string (uint32 length + bytes) of a key blob = the key type, e.g. "ssh-ed25519". */
std::string keyTypeOfBlob(std::string_view keyBlob);

}  // namespace sc::crypto
