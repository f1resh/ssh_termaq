// STUB — to be implemented (owner: session module).
#include "session/Charset.h"

namespace sc {

struct Decoder::Impl {};
Decoder::Decoder(const std::string&) : impl_(std::make_unique<Impl>()) {}
Decoder::~Decoder() = default;
std::string Decoder::decode(std::string_view bytes) { return std::string(bytes); }
std::string Decoder::flush() { return {}; }
std::string encodeFromUtf8(const std::string&, std::string_view utf8) { return std::string(utf8); }
std::string binaryStringToBytes(std::string_view utf8) { return std::string(utf8); }
unsigned int codePageForCharset(const std::string&) { return 65001; }
std::vector<std::string> supportedCharsets() { return {"UTF-8"}; }

}  // namespace sc
