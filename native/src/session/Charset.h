#pragma once
// Charset translation between the remote byte stream and UTF-8 (the UI's encoding).
// Uses Win32 code pages (MultiByteToWideChar / WideCharToMultiByte). Charset names are the ones offered in
// ui/src/shared/algorithms.ts (CHARSETS), matched case-insensitively; unknown names fall back to UTF-8.
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sc {

/** Streaming decoder: keeps incomplete multi-byte sequences between calls. Invalid bytes -> U+FFFD. */
class Decoder {
public:
    explicit Decoder(const std::string& charset);
    ~Decoder();
    std::string decode(std::string_view bytes);
    /** Emit anything still buffered (as U+FFFD for incomplete sequences). */
    std::string flush();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/** UTF-8 -> remote charset bytes. Unmappable characters become '?'. */
std::string encodeFromUtf8(const std::string& charset, std::string_view utf8);
/** "Binary string" from the UI (UTF-8 text whose code points are all < 256) -> raw bytes. Code points >= 256 are dropped. */
std::string binaryStringToBytes(std::string_view utf8);
/** Windows code page for a charset name, 0 if unknown. */
unsigned int codePageForCharset(const std::string& charset);
std::vector<std::string> supportedCharsets();

}  // namespace sc
