#include "core/Crypto.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cctype>
#include <stdexcept>

namespace st::crypto {

static std::string digest(const EVP_MD* md, std::string_view data) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (!EVP_Digest(data.data(), data.size(), out, &len, md, nullptr)) throw std::runtime_error("digest failed");
    return std::string(reinterpret_cast<char*>(out), len);
}

std::string sha256(std::string_view data) { return digest(EVP_sha256(), data); }
std::string sha1(std::string_view data) { return digest(EVP_sha1(), data); }
std::string md5(std::string_view data) { return digest(EVP_md5(), data); }

std::string randomBytes(size_t n) {
    std::string out(n, '\0');
    if (n && RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), static_cast<int>(n)) != 1)
        throw std::runtime_error("RAND_bytes failed");
    return out;
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(std::string_view data, bool pad) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < data.size()) {
        uint32_t v = (static_cast<unsigned char>(data[i]) << 16) | (static_cast<unsigned char>(data[i + 1]) << 8) |
                     static_cast<unsigned char>(data[i + 2]);
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);
        out.push_back(kB64[v & 63]);
        i += 3;
    }
    size_t rem = data.size() - i;
    if (rem == 1) {
        uint32_t v = static_cast<unsigned char>(data[i]) << 16;
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        if (pad) out += "==";
    } else if (rem == 2) {
        uint32_t v = (static_cast<unsigned char>(data[i]) << 16) | (static_cast<unsigned char>(data[i + 1]) << 8);
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);
        if (pad) out.push_back('=');
    }
    return out;
}

bool base64Decode(std::string_view b64, std::string& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (char c : b64) {
        if (c == '=') break;
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        int v = val(c);
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return true;
}

std::string fingerprintSha256(std::string_view keyBlob) { return "SHA256:" + base64Encode(sha256(keyBlob), false); }

std::string fingerprintMd5(std::string_view keyBlob) {
    static const char* d = "0123456789abcdef";
    std::string h = md5(keyBlob), out = "MD5:";
    for (size_t i = 0; i < h.size(); ++i) {
        if (i) out.push_back(':');
        unsigned char c = static_cast<unsigned char>(h[i]);
        out.push_back(d[c >> 4]);
        out.push_back(d[c & 15]);
    }
    return out;
}

std::string keyTypeOfBlob(std::string_view keyBlob) {
    if (keyBlob.size() < 4) return {};
    uint32_t len = (static_cast<unsigned char>(keyBlob[0]) << 24) | (static_cast<unsigned char>(keyBlob[1]) << 16) |
                   (static_cast<unsigned char>(keyBlob[2]) << 8) | static_cast<unsigned char>(keyBlob[3]);
    if (len > keyBlob.size() - 4) return {};
    return std::string(keyBlob.substr(4, len));
}

}  // namespace st::crypto
