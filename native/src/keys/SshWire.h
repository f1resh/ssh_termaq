#pragma once
// SSH wire-format (RFC 4251 section 5) reader/writer used by the key code: uint32, string, mpint.
// Also small helpers for handling secret material (wiping buffers).
#include <cstdint>
#include <string>
#include <string_view>

namespace st::keys {

/** Bounds-checked sequential reader. Every accessor returns false (and leaves the output untouched) on underflow. */
class WireReader {
public:
    explicit WireReader(std::string_view data) : data_(data) {}

    bool u8(uint8_t& v);
    bool u32(uint32_t& v);
    /** uint32 length + bytes. */
    bool string(std::string_view& out);
    bool string(std::string& out);
    /** Reads an mpint and returns its magnitude as unsigned big-endian bytes (leading zeros stripped). Rejects negatives. */
    bool mpintUnsigned(std::string& out);
    bool bytes(size_t n, std::string_view& out);

    size_t remaining() const { return data_.size() - pos_; }
    bool atEnd() const { return pos_ == data_.size(); }
    std::string_view rest() const { return data_.substr(pos_); }

private:
    std::string_view data_;
    size_t pos_ = 0;
};

/** Appending writer. */
class WireWriter {
public:
    WireWriter() = default;
    ~WireWriter();
    WireWriter(const WireWriter&) = delete;
    WireWriter& operator=(const WireWriter&) = delete;

    void u8(uint8_t v);
    void u32(uint32_t v);
    void string(std::string_view s);
    /** Writes an unsigned big-endian magnitude as an SSH mpint (adds a 0x00 prefix when the top bit is set). */
    void mpintUnsigned(std::string_view bigEndian);
    void raw(std::string_view s);

    const std::string& data() const { return buf_; }
    /** Moves the buffer out (the writer is left empty). */
    std::string take();

private:
    std::string buf_;  // wiped on destruction (may hold private key material)
};

/** Overwrites the string's bytes with zeros (OPENSSL_cleanse) and clears it. */
void wipe(std::string& s);

/** RAII: wipes the referenced string when the scope ends. */
class ScopedWipe {
public:
    explicit ScopedWipe(std::string& s) : s_(s) {}
    ~ScopedWipe() { wipe(s_); }
    ScopedWipe(const ScopedWipe&) = delete;
    ScopedWipe& operator=(const ScopedWipe&) = delete;

private:
    std::string& s_;
};

/** Strips leading zero bytes of an unsigned big-endian number (keeps at least nothing for zero). */
std::string stripLeadingZeros(std::string_view bigEndian);
/** Left-pads an unsigned big-endian number with zeros to `size` bytes (returns empty if it does not fit). */
std::string padLeft(std::string_view bigEndian, size_t size);

}  // namespace st::keys
