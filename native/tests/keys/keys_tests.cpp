// Key generation, PPK v2/v3 read/write, OpenSSH keys, conversion - cross-checked with PuTTY's plink and OpenSSH.
#include "TestServers.h"

#include "core/Crypto.h"
#include "core/Strings.h"
#include "keys/KeyLoader.h"
#include "keys/Keygen.h"
#include "keys/KeyFile.h"
#include "keys/Ppk.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace st;
using namespace st::test;
namespace fs = std::filesystem;

namespace {

const fs::path kPlink = "C:\\Program Files\\PuTTY\\plink.exe";
const fs::path kSshKeygen = "C:\\Windows\\System32\\OpenSSH\\ssh-keygen.exe";

void writeText(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary);
    f << s;
}

std::string pubBlob(ssh_key key) {
    char* b64 = nullptr;
    ssh_pki_export_pubkey_base64(key, &b64);
    std::string blob;
    crypto::base64Decode(b64, blob);
    ssh_string_free_char(b64);
    return blob;
}

/** Loads a key file; returns the public blob or "" (error in `err`). */
std::string load(const fs::path& p, const std::vector<std::string>& passphrases, std::string& err, int* asked = nullptr) {
    size_t i = 0;
    ssh_key key = loadPrivateKey(
        narrow(p.wstring()),
        [&](int) -> std::optional<std::string> {
            if (asked) ++*asked;
            if (i >= passphrases.size()) return std::nullopt;
            return passphrases[i++];
        },
        err);
    if (!key) return {};
    std::string blob = pubBlob(key);
    ssh_key_free(key);
    return blob;
}

std::string blobOfLine(const std::string& line) {
    std::string b;
    crypto::base64Decode(split(line, ' ', true)[1], b);
    return b;
}

int run(const std::string& cmd, std::string* out = nullptr) {
    FILE* p = _popen(("\"" + cmd + " 2>&1\"").c_str(), "r");
    if (!p) return -1;
    char buf[4096];
    size_t n;
    std::string o;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) o.append(buf, n);
    if (out) *out = o;
    return _pclose(p);
}

}  // namespace

TEST_CASE("keygen: ed25519 / rsa / ecdsa produce loadable OpenSSH and PPK files", "[keys]") {
    TempDir dir;
    for (auto [type, bits] : std::vector<std::pair<std::string, int>>{{"ed25519", 0}, {"rsa", 2048}, {"ecdsa", 256}, {"ecdsa", 384}}) {
        INFO(type << " " << bits);
        KeygenRequest req;
        req.type = type;
        req.bits = bits;
        req.comment = "test@" + type;
        KeygenResult r = keys::generateKey(req);
        REQUIRE(r.publicKeyOpenSsh.find("test@" + type) != std::string::npos);
        REQUIRE(r.fingerprint == crypto::fingerprintSha256(blobOfLine(r.publicKeyOpenSsh)));
        REQUIRE(r.privateKeyPpk.rfind("PuTTY-User-Key-File-3:", 0) == 0);
        writeText(dir.path() / "k.ppk", r.privateKeyPpk);
        writeText(dir.path() / "k", r.privateKeyOpenSsh);
        std::string err;
        REQUIRE(load(dir.path() / "k.ppk", {}, err) == blobOfLine(r.publicKeyOpenSsh));
        REQUIRE(load(dir.path() / "k", {}, err) == blobOfLine(r.publicKeyOpenSsh));
        KeyInfo info = keys::inspectKey(narrow((dir.path() / "k.ppk").wstring()), "");
        REQUIRE(info.format == "ppk3");
        REQUIRE(info.comment == "test@" + type);
        REQUIRE(info.fingerprint == r.fingerprint);
    }
}

TEST_CASE("ppk: encrypted v3 asks for the passphrase and rejects wrong ones", "[keys][ppk]") {
    TempDir dir;
    KeygenRequest req;
    req.comment = "enc";
    req.passphrase = "correct horse";
    KeygenResult r = keys::generateKey(req);
    REQUIRE(r.privateKeyPpk.find("Encryption: aes256-cbc") != std::string::npos);
    REQUIRE(r.privateKeyPpk.find("Key-Derivation: Argon2id") != std::string::npos);
    writeText(dir.path() / "e.ppk", r.privateKeyPpk);
    std::string err;
    int asked = 0;
    REQUIRE(load(dir.path() / "e.ppk", {"wrong", "correct horse"}, err, &asked) == blobOfLine(r.publicKeyOpenSsh));
    REQUIRE(asked == 2);
    REQUIRE(load(dir.path() / "e.ppk", {"a", "b", "c"}, err).empty());
    REQUIRE(err == "Wrong passphrase");
    REQUIRE(load(dir.path() / "e.ppk", {}, err).empty());
    REQUIRE(err == "Cancelled");
    // Public part readable without the passphrase.
    KeyInfo info = keys::inspectKey(narrow((dir.path() / "e.ppk").wstring()), "");
    REQUIRE(info.encrypted);
    REQUIRE(info.fingerprint == r.fingerprint);
    REQUIRE_THROWS(keys::inspectKey(narrow((dir.path() / "e.ppk").wstring()), "nope"));
    // The encrypted OpenSSH output works too.
    writeText(dir.path() / "e", r.privateKeyOpenSsh);
    REQUIRE(load(dir.path() / "e", {"correct horse"}, err) == blobOfLine(r.publicKeyOpenSsh));
}

TEST_CASE("ppk: v2 writer round-trips and tampering is detected", "[keys][ppk]") {
    TempDir dir;
    KeygenResult r = keys::generateKey(KeygenRequest{"ed25519", 0, "v2", ""});
    writeText(dir.path() / "k.ppk", r.privateKeyPpk);
    std::string err;
    keys::LoadedKey loaded;
    REQUIRE(keys::loadKeyMaterial(r.privateKeyPpk, "", nullptr, 1, loaded, err));
    for (const std::string pass : {"", "v2 pass"}) {
        keys::PpkWriteOptions o;
        o.version = 2;
        o.passphrase = pass;
        std::string v2;
        REQUIRE(keys::writePpk(loaded.material, o, v2, err));
        REQUIRE(v2.rfind("PuTTY-User-Key-File-2:", 0) == 0);
        writeText(dir.path() / "v2.ppk", v2);
        REQUIRE(load(dir.path() / "v2.ppk", {pass}, err) == blobOfLine(r.publicKeyOpenSsh));
    }
    // Flip a character in the private lines -> MAC failure.
    std::string bad = r.privateKeyPpk;
    auto pos = bad.find("Private-Lines:");
    pos = bad.find('\n', pos) + 5;
    bad[pos] = bad[pos] == 'A' ? 'B' : 'A';
    writeText(dir.path() / "bad.ppk", bad);
    REQUIRE(load(dir.path() / "bad.ppk", {}, err).empty());
}

TEST_CASE("convert: OpenSSH <-> PPK", "[keys]") {
    TempDir dir;
    KeygenResult r = keys::generateKey(KeygenRequest{"ecdsa", 256, "conv", "pw"});
    writeText(dir.path() / "id", r.privateKeyOpenSsh);
    const std::string ppk = keys::convertKey(narrow((dir.path() / "id").wstring()), "pw", "ppk");
    writeText(dir.path() / "id.ppk", ppk);
    std::string err;
    REQUIRE(load(dir.path() / "id.ppk", {"pw"}, err) == blobOfLine(r.publicKeyOpenSsh));
    const std::string back = keys::convertKey(narrow((dir.path() / "id.ppk").wstring()), "pw", "openssh");
    writeText(dir.path() / "back", back);
    REQUIRE(load(dir.path() / "back", {"pw"}, err) == blobOfLine(r.publicKeyOpenSsh));
    REQUIRE_THROWS(keys::convertKey(narrow((dir.path() / "id").wstring()), "", "ppk"));
}

TEST_CASE("OpenSSH keys made by ssh-keygen load (encrypted with bcrypt KDF)", "[keys][openssh]") {
    if (!fs::exists(kSshKeygen)) SKIP("ssh-keygen not installed");
    TempDir dir;
    const fs::path k = dir.path() / "id_test";
    REQUIRE(run("\"" + kSshKeygen.string() + "\" -q -t ed25519 -N \"s3cret pass\" -C fromkeygen -f \"" + k.string() + "\"") == 0);
    std::string err;
    std::ifstream pubf(k.string() + ".pub");
    std::string pubLine;
    std::getline(pubf, pubLine);
    REQUIRE(load(k, {"s3cret pass"}, err) == blobOfLine(pubLine));
    KeyInfo info = keys::inspectKey(narrow(k.wstring()), "");
    REQUIRE(info.format == "openssh");
    REQUIRE(info.encrypted);
    REQUIRE(info.comment == "fromkeygen");  // from the .pub sidecar
}

TEST_CASE("plink authenticates with PPK files written by us", "[keys][plink]") {
    if (!fs::exists(kPlink)) SKIP("plink not installed");
    TempDir dir;
    KeygenResult r = keys::generateKey(KeygenRequest{"ed25519", 0, "plink-test", ""});
    std::string err;
    keys::LoadedKey loaded;
    REQUIRE(keys::loadKeyMaterial(r.privateKeyPpk, "", nullptr, 1, loaded, err));
    keys::PpkWriteOptions o2;
    o2.version = 2;
    std::string v2;
    REQUIRE(keys::writePpk(loaded.material, o2, v2, err));
    writeText(dir.path() / "v3.ppk", r.privateKeyPpk);
    writeText(dir.path() / "v2.ppk", v2);

    TestSshServerOptions so;
    so.allowPassword = false;
    so.allowKbdInt = false;
    so.authorizedKeys = {r.publicKeyOpenSsh};
    TestSshServer server(so);
    for (const char* file : {"v3.ppk", "v2.ppk"}) {
        std::string out;
        const int code = run("\"" + kPlink.string() + "\" -batch -ssh -noagent -P " + std::to_string(server.port()) +
                                 " -l test -i \"" + (dir.path() / file).string() + "\" -hostkey " + server.hostKeyFingerprint() +
                                 " 127.0.0.1 echo key-ok",
                             &out);
        INFO(file << ": " << out);
        REQUIRE(code == 0);
        REQUIRE(out.find("key-ok") != std::string::npos);
    }
    REQUIRE(server.waitForEvent("auth:publickey:ok", 1000));
}

TEST_CASE("detectKeyFormat", "[keys]") {
    REQUIRE(detectKeyFormat("PuTTY-User-Key-File-3: ssh-ed25519\n") == "ppk3");
    REQUIRE(detectKeyFormat("PuTTY-User-Key-File-2: ssh-rsa\n") == "ppk2");
    REQUIRE(detectKeyFormat("-----BEGIN OPENSSH PRIVATE KEY-----\n") == "openssh");
    REQUIRE(detectKeyFormat("-----BEGIN RSA PRIVATE KEY-----\n") == "pem");
    REQUIRE(detectKeyFormat("hello") == "unknown");
}
