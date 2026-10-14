#include "bridge/Bridge.h"
#include "core/Crypto.h"
#include "core/Strings.h"
#include "core/Types.h"

#include <catch2/catch_test_macros.hpp>

using namespace st;

TEST_CASE("strings: widen/narrow roundtrip") {
    std::string s = "h\xC3\xA9llo \xE2\x82\xAC \xF0\x9F\x98\x80";
    REQUIRE(narrow(widen(s)) == s);
    REQUIRE(split("a,,b", ',').size() == 3);
    REQUIRE(split("a,,b", ',', true).size() == 2);
    REQUIRE(trim("  x y \t") == "x y");
}

TEST_CASE("crypto: base64 + fingerprint") {
    std::string out;
    REQUIRE(crypto::base64Encode("hello") == "aGVsbG8=");
    REQUIRE(crypto::base64Decode("aGVsbG8", out));
    REQUIRE(out == "hello");
    REQUIRE(crypto::fingerprintSha256("abc").rfind("SHA256:", 0) == 0);
    std::string blob = std::string("\0\0\0\x0bssh-ed25519", 15) + std::string(36, 'x');
    REQUIRE(crypto::keyTypeOfBlob(blob) == "ssh-ed25519");
}

TEST_CASE("types: session defaults and lenient parsing") {
    SessionConfig s = sessionFromJson(Json{{"host", "example.com"}, {"protocol", "telnet"}, {"folder", "/a//b/"}});
    REQUIRE(s.host == "example.com");
    REQUIRE(s.protocol == Protocol::Telnet);
    REQUIRE(s.folder == "a/b");
    REQUIRE(s.connection.terminalType == "xterm-256color");
    REQUIRE(s.ssh.auth.tryAgent);
    Json j = s;
    REQUIRE(j["protocol"] == "telnet");
    REQUIRE(j["keyboard"]["backspace"] == "del");
    REQUIRE(j["serial"]["flowControl"] == "xonxoff");
}

namespace {
struct InlineDispatcher : Dispatcher {
    void post(std::function<void()> fn) override { fn(); }
    bool isUiThread() const override { return true; }
};
}  // namespace

TEST_CASE("bridge: invoke, send and events") {
    InlineDispatcher d;
    Bridge b(d);
    std::vector<std::string> out;
    b.setSink([&](const std::string& s) { out.push_back(s); });
    b.handleSync("t:add", [](const Json& a) { return Json(argInt(a, 0) + argInt(a, 1)); });
    b.handleSync("t:fail", [](const Json&) -> Json { throw std::runtime_error("boom"); });
    int sent = 0;
    b.on("t:ping", [&](const Json&) { ++sent; });

    b.onWebMessage(R"({"t":"req","id":1,"ch":"t:add","args":[2,3]})");
    b.onWebMessage(R"({"t":"req","id":2,"ch":"t:fail","args":[]})");
    b.onWebMessage(R"({"t":"req","id":3,"ch":"t:missing","args":[]})");
    b.onWebMessage(R"({"t":"send","ch":"t:ping","args":[]})");
    b.emit("t:evt", Json::array({"x", 1}));

    REQUIRE(out.size() == 4);
    auto r1 = Json::parse(out[0]);
    REQUIRE(r1["ok"] == true);
    REQUIRE(r1["result"] == 5);
    auto r2 = Json::parse(out[1]);
    REQUIRE(r2["ok"] == false);
    REQUIRE(r2["error"] == "boom");
    REQUIRE(Json::parse(out[2])["ok"] == false);
    REQUIRE(sent == 1);
    auto e = Json::parse(out[3]);
    REQUIRE(e["t"] == "evt");
    REQUIRE(e["ch"] == "t:evt");
    REQUIRE(e["args"][1] == 1);
}
