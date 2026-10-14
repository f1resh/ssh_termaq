#include "keys/KeyMaterial.h"

#include "core/Crypto.h"
#include "core/Strings.h"
#include "keys/SshWire.h"

#include <array>

namespace st::keys {

namespace {

constexpr std::string_view kOpenSshMagic{"openssh-key-v1\0", 15};
constexpr std::string_view kOpenSshBegin = "-----BEGIN OPENSSH PRIVATE KEY-----";
constexpr std::string_view kOpenSshEnd = "-----END OPENSSH PRIVATE KEY-----";

struct EcCurve {
    std::string_view type;
    std::string_view name;
    size_t fieldBytes;
};
constexpr std::array<EcCurve, 3> kCurves{{{"ecdsa-sha2-nistp256", "nistp256", 32},
                                          {"ecdsa-sha2-nistp384", "nistp384", 48},
                                          {"ecdsa-sha2-nistp521", "nistp521", 66}}};

const EcCurve* curveForType(std::string_view type) {
    for (const auto& c : kCurves)
        if (c.type == type) return &c;
    return nullptr;
}

/** Edwards key sizes: ed25519 32 bytes, ed448 57 bytes (0 = not an EdDSA type). */
size_t eddsaSize(std::string_view type) {
    if (type == "ssh-ed25519") return 32;
    if (type == "ssh-ed448") return 57;
    return 0;
}

/** Parsed public components (unsigned big-endian integers / raw strings). */
struct PublicParts {
    std::string type;
    std::string a, b, c, d;  // rsa: e,n | dss: p,q,g,y | ecdsa: curve,Q | eddsa: A
};

bool parsePublic(std::string_view blob, PublicParts& out, std::string& error) {
    WireReader r(blob);
    if (!r.string(out.type)) {
        error = "Malformed public key blob";
        return false;
    }
    bool ok = false;
    if (out.type == "ssh-rsa") {
        ok = r.mpintUnsigned(out.a) && r.mpintUnsigned(out.b) && !out.a.empty() && !out.b.empty();
    } else if (out.type == "ssh-dss") {
        ok = r.mpintUnsigned(out.a) && r.mpintUnsigned(out.b) && r.mpintUnsigned(out.c) && r.mpintUnsigned(out.d);
    } else if (const EcCurve* curve = curveForType(out.type)) {
        ok = r.string(out.a) && r.string(out.b) && out.a == curve->name && out.b.size() == 1 + 2 * curve->fieldBytes &&
             out.b[0] == '\x04';
    } else if (size_t n = eddsaSize(out.type)) {
        ok = r.string(out.a) && out.a.size() == n;
    } else {
        error = "Unsupported key type '" + out.type + "'";
        return false;
    }
    if (!ok || !r.atEnd()) {
        error = "Malformed " + out.type + " public key";
        return false;
    }
    return true;
}

size_t ppkPrivateUsedImpl(std::string_view type, std::string_view blob) {
    WireReader r(blob);
    std::string x;
    ScopedWipe wx(x);
    bool ok = false;
    if (type == "ssh-rsa") {
        ok = r.mpintUnsigned(x) && r.mpintUnsigned(x) && r.mpintUnsigned(x) && r.mpintUnsigned(x);
    } else if (type == "ssh-dss" || curveForType(type)) {
        ok = r.mpintUnsigned(x) && !x.empty();
    } else if (size_t n = eddsaSize(type)) {
        ok = r.string(x) && x.size() == n;
    }
    return ok ? blob.size() - r.remaining() : 0;
}

void appendArmored(std::string& out, std::string_view begin, std::string_view end, std::string_view der) {
    std::string b64 = crypto::base64Encode(der);
    out.append(begin).append("\n");
    for (size_t i = 0; i < b64.size(); i += 70) out.append(b64.substr(i, 70)).append("\n");
    out.append(end).append("\n");
    wipe(b64);
}

}  // namespace

KeyMaterial::~KeyMaterial() { wipe(ppkPrivate); }

size_t ppkPrivateLength(std::string_view type, std::string_view blob) { return ppkPrivateUsedImpl(type, blob); }

bool isKnownKeyType(std::string_view type) {
    return type == "ssh-rsa" || type == "ssh-dss" || curveForType(type) != nullptr || eddsaSize(type) != 0;
}

bool libsshSupportsKeyType(std::string_view type) {
    return type == "ssh-rsa" || type == "ssh-ed25519" || curveForType(type) != nullptr;
}

bool validatePublicBlob(std::string_view publicBlob, std::string_view expectedType, std::string& error) {
    PublicParts p;
    if (!parsePublic(publicBlob, p, error)) return false;
    if (!expectedType.empty() && p.type != expectedType) {
        error = "Key type mismatch: header says " + std::string(expectedType) + ", key is " + p.type;
        return false;
    }
    return true;
}

bool validateMaterial(const KeyMaterial& m, std::string& error) {
    if (!validatePublicBlob(m.publicBlob, m.type, error)) return false;
    const size_t used = ppkPrivateUsedImpl(m.type, m.ppkPrivate);
    if (used == 0 || used != m.ppkPrivate.size()) {
        error = "Malformed " + m.type + " private key";
        return false;
    }
    return true;
}

std::string publicKeyLine(std::string_view publicBlob, std::string_view comment) {
    std::string line = crypto::keyTypeOfBlob(publicBlob) + " " + crypto::base64Encode(publicBlob);
    if (!comment.empty()) line.append(" ").append(comment);
    return line;
}

/* ---- openssh-key-v1 ---------------------------------------------------- */

bool parseOpenSshContainer(std::string_view text, OpenSshContainer& out, std::string& error) {
    const size_t b = text.find(kOpenSshBegin);
    const size_t e = text.find(kOpenSshEnd);
    if (b == std::string_view::npos || e == std::string_view::npos || e < b) {
        error = "Not an OpenSSH private key";
        return false;
    }
    std::string der;
    ScopedWipe wd(der);
    if (!crypto::base64Decode(text.substr(b + kOpenSshBegin.size(), e - b - kOpenSshBegin.size()), der) ||
        der.compare(0, kOpenSshMagic.size(), kOpenSshMagic) != 0) {
        error = "Corrupt OpenSSH private key";
        return false;
    }
    WireReader r(std::string_view(der).substr(kOpenSshMagic.size()));
    uint32_t count = 0;
    if (!r.string(out.cipher) || !r.string(out.kdf) || !r.string(out.kdfOptions) || !r.u32(count)) {
        error = "Corrupt OpenSSH private key";
        return false;
    }
    if (count != 1) {
        error = "OpenSSH key files with " + std::to_string(count) + " keys are not supported";
        return false;
    }
    if (!r.string(out.publicBlob) || !r.string(out.privateSection)) {
        error = "Corrupt OpenSSH private key";
        return false;
    }
    return true;
}

bool decodeOpenSshPrivate(const OpenSshContainer& c, KeyMaterial& out, std::string& error) {
    if (c.encrypted()) {
        error = "OpenSSH key is encrypted";
        return false;
    }
    WireReader r(c.privateSection);
    uint32_t check1 = 0, check2 = 0;
    std::string type;
    if (!r.u32(check1) || !r.u32(check2) || check1 != check2 || !r.string(type)) {
        error = "Corrupt OpenSSH private key";
        return false;
    }
    KeyMaterial m;
    m.type = type;
    WireWriter pub, priv;
    pub.string(type);
    bool ok = false;
    std::string f1, f2, f3, f4, f5, f6;
    ScopedWipe w1(f1), w2(f2), w3(f3), w4(f4), w5(f5), w6(f6);
    if (type == "ssh-rsa") {  // n e d iqmp p q
        ok = r.mpintUnsigned(f1) && r.mpintUnsigned(f2) && r.mpintUnsigned(f3) && r.mpintUnsigned(f4) &&
             r.mpintUnsigned(f5) && r.mpintUnsigned(f6);
        if (ok) {
            pub.mpintUnsigned(f2);
            pub.mpintUnsigned(f1);
            priv.mpintUnsigned(f3);
            priv.mpintUnsigned(f5);
            priv.mpintUnsigned(f6);
            priv.mpintUnsigned(f4);
        }
    } else if (type == "ssh-dss") {  // p q g y x
        ok = r.mpintUnsigned(f1) && r.mpintUnsigned(f2) && r.mpintUnsigned(f3) && r.mpintUnsigned(f4) &&
             r.mpintUnsigned(f5);
        if (ok) {
            pub.mpintUnsigned(f1);
            pub.mpintUnsigned(f2);
            pub.mpintUnsigned(f3);
            pub.mpintUnsigned(f4);
            priv.mpintUnsigned(f5);
        }
    } else if (curveForType(type)) {  // curve Q d
        ok = r.string(f1) && r.string(f2) && r.mpintUnsigned(f3);
        if (ok) {
            pub.string(f1);
            pub.string(f2);
            priv.mpintUnsigned(f3);
        }
    } else if (size_t n = eddsaSize(type)) {  // A, seed||A
        ok = r.string(f1) && r.string(f2) && f1.size() == n && f2.size() == 2 * n && f2.compare(n, n, f1) == 0;
        if (ok) {
            pub.string(f1);
            priv.string(std::string_view(f2).substr(0, n));
        }
    } else {
        error = "Unsupported key type '" + type + "'";
        return false;
    }
    if (!ok || !r.string(m.comment)) {
        error = "Corrupt OpenSSH " + type + " private key";
        return false;
    }
    // Padding must be 1, 2, 3, ...
    std::string_view pad = r.rest();
    for (size_t i = 0; i < pad.size(); ++i)
        if (static_cast<unsigned char>(pad[i]) != ((i + 1) & 0xff)) {
            error = "Corrupt OpenSSH private key (bad padding)";
            return false;
        }
    m.publicBlob = pub.take();
    m.ppkPrivate = priv.take();
    if (m.publicBlob != c.publicBlob) {
        error = "OpenSSH private key does not match its public key";
        return false;
    }
    if (!validateMaterial(m, error)) return false;
    out = std::move(m);
    return true;
}

bool encodeOpenSshPrivate(const KeyMaterial& m, std::string& armoredOut, std::string& error) {
    if (!validateMaterial(m, error)) return false;
    PublicParts p;
    if (!parsePublic(m.publicBlob, p, error)) return false;

    WireWriter sec;
    const std::string check = crypto::randomBytes(4);
    sec.raw(check);
    sec.raw(check);
    sec.string(m.type);
    WireReader pr(m.ppkPrivate);
    std::string x1, x2, x3, x4;
    ScopedWipe w1(x1), w2(x2), w3(x3), w4(x4);
    if (m.type == "ssh-rsa") {  // ppk: d p q iqmp -> n e d iqmp p q
        pr.mpintUnsigned(x1);
        pr.mpintUnsigned(x2);
        pr.mpintUnsigned(x3);
        pr.mpintUnsigned(x4);
        sec.mpintUnsigned(p.b);
        sec.mpintUnsigned(p.a);
        sec.mpintUnsigned(x1);
        sec.mpintUnsigned(x4);
        sec.mpintUnsigned(x2);
        sec.mpintUnsigned(x3);
    } else if (m.type == "ssh-dss") {
        pr.mpintUnsigned(x1);
        sec.mpintUnsigned(p.a);
        sec.mpintUnsigned(p.b);
        sec.mpintUnsigned(p.c);
        sec.mpintUnsigned(p.d);
        sec.mpintUnsigned(x1);
    } else if (curveForType(m.type)) {
        pr.mpintUnsigned(x1);
        sec.string(p.a);
        sec.string(p.b);
        sec.mpintUnsigned(x1);
    } else {  // EdDSA
        pr.string(x1);
        sec.string(p.a);
        x2 = x1 + p.a;
        sec.string(x2);
    }
    sec.string(m.comment);
    for (uint8_t i = 1; sec.data().size() % 8 != 0; ++i) sec.u8(i);

    WireWriter der;
    der.raw(kOpenSshMagic);
    der.string("none");
    der.string("none");
    der.string("");
    der.u32(1);
    der.string(m.publicBlob);
    der.string(sec.data());
    armoredOut.clear();
    appendArmored(armoredOut, kOpenSshBegin, kOpenSshEnd, der.data());
    return true;
}

}  // namespace st::keys
