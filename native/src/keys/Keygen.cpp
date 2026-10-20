#include "keys/Keygen.h"

#include "core/Crypto.h"
#include "keys/KeyFile.h"
#include "keys/Ppk.h"
#include "keys/SshWire.h"

#include <stdexcept>

namespace st::keys {

namespace {

std::string exportEncryptedOpenSsh(ssh_key key, const std::string& passphrase) {
    char* b64 = nullptr;
    if (ssh_pki_export_privkey_base64(key, passphrase.c_str(), nullptr, nullptr, &b64) != SSH_OK || !b64)
        throw std::runtime_error("Could not export the private key");
    std::string out = b64;
    ssh_string_free_char(b64);
    if (!out.empty() && out.back() != '\n') out.push_back('\n');
    return out;
}

std::string openSshText(const KeyMaterial& m, const std::string& passphrase) {
    if (passphrase.empty()) {
        std::string out, err;
        if (!encodeOpenSshPrivate(m, out, err)) throw std::runtime_error(err);
        return out;
    }
    std::string err;
    SshKeyPtr key = sshKeyFromMaterial(m, err);
    if (!key) throw std::runtime_error(err.empty() ? "Unsupported key type" : err);
    return exportEncryptedOpenSsh(key.get(), passphrase);
}

std::string ppkText(const KeyMaterial& m, const std::string& passphrase) {
    PpkWriteOptions o;
    o.version = 3;
    o.passphrase = passphrase;
    std::string out, err;
    if (!writePpk(m, o, out, err)) throw std::runtime_error(err);
    return out;
}

}  // namespace

KeygenResult generateKey(const KeygenRequest& req) {
    enum ssh_keytypes_e type = SSH_KEYTYPE_ED25519;
    int bits = 0;
    if (req.type == "rsa") {
        type = SSH_KEYTYPE_RSA;
        bits = req.bits ? req.bits : 3072;
        if (bits != 2048 && bits != 3072 && bits != 4096) throw std::invalid_argument("RSA keys must be 2048, 3072 or 4096 bits");
    } else if (req.type == "ecdsa") {
        bits = req.bits ? req.bits : 256;
        if (bits == 256) type = SSH_KEYTYPE_ECDSA_P256;
        else if (bits == 384) type = SSH_KEYTYPE_ECDSA_P384;
        else if (bits == 521) type = SSH_KEYTYPE_ECDSA_P521;
        else throw std::invalid_argument("ECDSA keys must be 256, 384 or 521 bits");
    } else if (req.type != "ed25519") {
        throw std::invalid_argument("Unknown key type: " + req.type);
    }
    // The key type carries the ECDSA curve; RSA takes its size from a PKI context (none: libssh's defaults).
    ssh_pki_ctx ctx = nullptr;
    if (type == SSH_KEYTYPE_RSA) {
        ctx = ssh_pki_ctx_new();
        if (!ctx || ssh_pki_ctx_options_set(ctx, SSH_PKI_OPTION_RSA_KEY_SIZE, &bits) != SSH_OK) {
            ssh_pki_ctx_free(ctx);
            throw std::runtime_error("Key generation failed");
        }
    }
    ssh_key raw = nullptr;
    const int rc = ssh_pki_generate_key(type, ctx, &raw);
    ssh_pki_ctx_free(ctx);
    if (rc != SSH_OK || !raw) throw std::runtime_error("Key generation failed");
    SshKeyPtr key(raw);

    KeyMaterial m;
    std::string err;
    if (!materialFromSshKey(key.get(), m, err)) throw std::runtime_error(err);
    m.comment = req.comment;

    KeygenResult r;
    r.privateKeyOpenSsh = req.passphrase.empty() ? openSshText(m, "") : exportEncryptedOpenSsh(key.get(), req.passphrase);
    r.privateKeyPpk = ppkText(m, req.passphrase);
    r.publicKeyOpenSsh = publicKeyLine(m.publicBlob, m.comment);
    r.fingerprint = crypto::fingerprintSha256(m.publicBlob);
    return r;
}

KeyInfo inspectKey(const std::string& path, const std::string& passphrase) {
    KeyInfo info;
    info.path = path;
    auto content = readKeyFile(path);
    if (!content) throw std::runtime_error("Cannot read key file");
    ScopedWipe wc(*content);
    const KeyFormat fmt = detectFormat(*content);
    info.format = formatName(fmt);
    if (fmt == KeyFormat::Unknown) throw std::runtime_error("Not a recognised private key file");
    info.encrypted = isEncrypted(*content, fmt);

    std::string blob, comment, err;
    if (fmt == KeyFormat::Ppk2 || fmt == KeyFormat::Ppk3) {
        PpkFile f;
        if (!parsePpk(*content, f, err)) throw std::runtime_error(err);
        blob = f.publicBlob;
        comment = f.comment;
    } else if (fmt == KeyFormat::OpenSsh) {
        OpenSshContainer c;
        if (!parseOpenSshContainer(*content, c, err)) throw std::runtime_error(err);
        blob = c.publicBlob;
        if (!c.encrypted()) {
            KeyMaterial m;
            if (decodeOpenSshPrivate(c, m, err)) comment = m.comment;
            ScopedWipe ws(c.privateSection);
        } else if (auto side = sidecarPublicKey(path, &comment)) {
            (void)side;
        }
    } else if (!info.encrypted) {
        SshKeyPtr k = importPrivateKeyText(*content, nullptr);
        if (!k || !publicBlobOfSshKey(k.get(), blob)) throw std::runtime_error("Invalid or unsupported private key");
        sidecarPublicKey(path, &comment);
    } else if (auto side = sidecarPublicKey(path, &comment)) {
        blob = *side;
    }

    // Verify a supplied passphrase (and recover the public part of encrypted PEM keys).
    if (info.encrypted && !passphrase.empty()) {
        LoadedKey loaded;
        int calls = 0;
        auto cb = [&](int) -> std::optional<std::string> {
            if (calls++) return std::nullopt;
            return passphrase;
        };
        if (!loadKeyMaterial(*content, path, cb, 1, loaded, err)) throw std::runtime_error("Wrong passphrase");
        if (blob.empty()) blob = loaded.material.publicBlob;
        if (comment.empty()) comment = loaded.material.comment;
    }

    if (!blob.empty()) {
        info.keyType = crypto::keyTypeOfBlob(blob);
        info.fingerprint = crypto::fingerprintSha256(blob);
        info.publicKeyOpenSsh = publicKeyLine(blob, comment);
    }
    info.comment = comment;
    return info;
}

std::string convertKey(const std::string& path, const std::string& passphrase, const std::string& to) {
    if (to != "openssh" && to != "ppk") throw std::invalid_argument("Unknown target format: " + to);
    auto content = readKeyFile(path);
    if (!content) throw std::runtime_error("Cannot read key file");
    ScopedWipe wc(*content);
    LoadedKey loaded;
    std::string err;
    int calls = 0;
    auto cb = [&](int) -> std::optional<std::string> {
        if (calls++ || passphrase.empty()) return std::nullopt;
        return passphrase;
    };
    if (!loadKeyMaterial(*content, path, cb, 1, loaded, err)) {
        if (err == "Cancelled") throw std::runtime_error("This key is encrypted: enter its passphrase");
        throw std::runtime_error(err);
    }
    return to == "ppk" ? ppkText(loaded.material, loaded.passphrase) : openSshText(loaded.material, loaded.passphrase);
}

}  // namespace st::keys
