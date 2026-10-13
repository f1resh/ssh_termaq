#include "keys/SshWire.h"

#include <openssl/crypto.h>

namespace sc::keys {

bool WireReader::u8(uint8_t& v) {
    if (remaining() < 1) return false;
    v = static_cast<uint8_t>(data_[pos_++]);
    return true;
}

bool WireReader::u32(uint32_t& v) {
    if (remaining() < 4) return false;
    const auto* p = reinterpret_cast<const unsigned char*>(data_.data() + pos_);
    v = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    pos_ += 4;
    return true;
}

bool WireReader::bytes(size_t n, std::string_view& out) {
    if (remaining() < n) return false;
    out = data_.substr(pos_, n);
    pos_ += n;
    return true;
}

bool WireReader::string(std::string_view& out) {
    const size_t start = pos_;
    uint32_t len = 0;
    if (!u32(len)) return false;
    if (remaining() < len) {
        pos_ = start;
        return false;
    }
    out = data_.substr(pos_, len);
    pos_ += len;
    return true;
}

bool WireReader::string(std::string& out) {
    std::string_view v;
    if (!string(v)) return false;
    out.assign(v.data(), v.size());
    return true;
}

bool WireReader::mpintUnsigned(std::string& out) {
    const size_t start = pos_;
    std::string_view v;
    if (!string(v)) return false;
    if (!v.empty() && (static_cast<unsigned char>(v[0]) & 0x80)) {  // negative numbers never occur in keys
        pos_ = start;
        return false;
    }
    out = stripLeadingZeros(v);
    return true;
}

WireWriter::~WireWriter() { wipe(buf_); }

void WireWriter::u8(uint8_t v) { buf_.push_back(static_cast<char>(v)); }

void WireWriter::u32(uint32_t v) {
    buf_.push_back(static_cast<char>((v >> 24) & 0xff));
    buf_.push_back(static_cast<char>((v >> 16) & 0xff));
    buf_.push_back(static_cast<char>((v >> 8) & 0xff));
    buf_.push_back(static_cast<char>(v & 0xff));
}

void WireWriter::string(std::string_view s) {
    u32(static_cast<uint32_t>(s.size()));
    buf_.append(s.data(), s.size());
}

void WireWriter::mpintUnsigned(std::string_view bigEndian) {
    std::string_view v = bigEndian;
    while (!v.empty() && v.front() == '\0') v.remove_prefix(1);
    const bool pad = !v.empty() && (static_cast<unsigned char>(v.front()) & 0x80);
    u32(static_cast<uint32_t>(v.size() + (pad ? 1 : 0)));
    if (pad) buf_.push_back('\0');
    buf_.append(v.data(), v.size());
}

void WireWriter::raw(std::string_view s) { buf_.append(s.data(), s.size()); }

std::string WireWriter::take() {
    std::string out = std::move(buf_);
    buf_.clear();
    return out;
}

void wipe(std::string& s) {
    if (!s.empty()) OPENSSL_cleanse(s.data(), s.size());
    s.clear();
}

std::string stripLeadingZeros(std::string_view bigEndian) {
    while (!bigEndian.empty() && bigEndian.front() == '\0') bigEndian.remove_prefix(1);
    return std::string(bigEndian);
}

std::string padLeft(std::string_view bigEndian, size_t size) {
    std::string v = stripLeadingZeros(bigEndian);
    if (v.size() > size) return {};
    return std::string(size - v.size(), '\0') + v;
}

}  // namespace sc::keys
