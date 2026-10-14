// SessionLog: path placeholders, escape stripping (also across chunk boundaries), modes, append/overwrite/ask.
#include "TestServers.h"
#include "core/Strings.h"
#include "session/Prompter.h"
#include "session/SessionLog.h"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>

using namespace st;
using st::test::TempDir;

namespace {

std::string slurp(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::tm sampleTime() {
    std::tm t{};
    t.tm_year = 2026 - 1900;
    t.tm_mon = 9;  // October
    t.tm_mday = 3;
    t.tm_hour = 4;
    t.tm_min = 5;
    t.tm_sec = 6;
    return t;
}

SessionConfig logSession(LogMode mode, const std::string& path, LogIfExists ifExists = LogIfExists::Append,
                         bool header = false) {
    SessionConfig s;
    s.name = "test";
    s.host = "127.0.0.1";
    s.port = 2222;
    s.logging.mode = mode;
    s.logging.path = path;
    s.logging.ifExists = ifExists;
    s.logging.header = header;
    s.logging.flush = true;
    return s;
}

/** Prompter that answers every prompt with a fixed button and records the prompt. */
struct ChoicePrompter : Prompter {
    std::string choice;
    bool cancel = false;
    int calls = 0;
    Json last;
    PromptResponse prompt(const PromptInput& input) override {
        ++calls;
        last = input.json;
        PromptResponse r;
        r.cancelled = cancel;
        r.choice = choice;
        return r;
    }
};

}  // namespace

TEST_CASE("sessionlog: expandPath placeholders", "[sessionlog]") {
    SessionConfig s;
    s.name = "my:sess/ion*";
    s.host = "host.example";
    s.port = 2222;
    const std::tm t = sampleTime();
    CHECK(SessionLog::expandPath("&Y&M&D-&T-&H-&P-&S.log", s, t) == "20261003-040506-host.example-2222-my_sess_ion_.log");
    CHECK(SessionLog::expandPath("&y&m&d&t&h&p&s", s, t) == "20261003040506host.example2222my_sess_ion_");
    CHECK(SessionLog::expandPath("a&&b&x&", s, t) == "a&b&x&");
    CHECK(SessionLog::expandPath("logs\\&H\\out.log", s, t) == "logs\\host.example\\out.log");

    // Unsaved session: &S falls back to the host; host names are sanitised too.
    SessionConfig q;
    q.host = "fe80::1%eth0";
    q.port = 22;
    CHECK(SessionLog::expandPath("&S|&H", q, t) == "fe80__1%eth0|fe80__1%eth0");

    // Serial: &H is the line, &P the speed.
    SessionConfig ser;
    ser.protocol = Protocol::Serial;
    ser.serial.line = "COM3";
    ser.serial.baudRate = 115200;
    CHECK(SessionLog::expandPath("&H-&P", ser, t) == "COM3-115200");
}

TEST_CASE("sessionlog: stripEscapes", "[sessionlog]") {
    CHECK(SessionLog::stripEscapes("a\x1b[31mb\x1b[0m") == "ab");
    CHECK(SessionLog::stripEscapes("\x1b]0;title\x07x") == "x");
    CHECK(SessionLog::stripEscapes("\x1b]2;t\x1b\\y") == "y");
    CHECK(SessionLog::stripEscapes("\x1bPq#0;2;0;0;0\x1b\\z") == "z");
    CHECK(SessionLog::stripEscapes("\xC2\x9B" "1mq") == "q");                // C1 CSI
    CHECK(SessionLog::stripEscapes("\xC2\x9D" "0;t\xC2\x9Cw") == "w");     // C1 OSC ... ST
    CHECK(SessionLog::stripEscapes("\x1b(Bk\x1b=\x1b>") == "k");             // designate charset, keypad modes
    CHECK(SessionLog::stripEscapes("\x1b[?1049h\x1b[2J\x1b[1;1Hok") == "ok");
    CHECK(SessionLog::stripEscapes("l1\r\n\tl2\x07\x08\x7f") == "l1\r\n\tl2");
    CHECK(SessionLog::stripEscapes("caf\xC3\xA9 \xC2\xA0") == "caf\xC3\xA9 \xC2\xA0");  // non-C1 2-byte chars kept
    CHECK(SessionLog::stripEscapes("\x1b[1\x18x") == "x");                     // CAN aborts a sequence
}

TEST_CASE("sessionlog: printable mode strips sequences across chunk boundaries", "[sessionlog]") {
    TempDir dir;
    SessionLog log(logSession(LogMode::Printable, "p.log"), dir.path());
    std::string err;
    REQUIRE(log.open(nullptr, err));
    REQUIRE(log.isOpen());
    CHECK(log.path() == dir.path() / "p.log");
    const char* chunks[] = {"a\x1b", "[3", "1mb\x1b]0;ti", "tle", "\x07" "c\x1b", "]2;x\x1b", "\\d\xC2", "\x9B" "0me\r\n"};
    for (const char* c : chunks) log.output(c, c);
    log.event("not written in printable mode");
    log.localOutput("\x1b[1m[local]\x1b[0m");
    log.close();
    CHECK(slurp(dir.path() / "p.log") == "abcde\r\n[local]");
}

TEST_CASE("sessionlog: all and ssh-debug modes", "[sessionlog]") {
    TempDir dir;
    {
        SessionLog log(logSession(LogMode::All, "all.log"), dir.path());
        std::string err;
        REQUIRE(log.open(nullptr, err));
        log.output("\x1b[31mraw\xff", "ignored");
        log.event("not written in all mode");
        log.localOutput("local");
    }
    CHECK(slurp(dir.path() / "all.log") == "\x1b[31mraw\xff" "local");
    {
        SessionLog log(logSession(LogMode::SshDebug, "dbg.log"), dir.path());
        std::string err;
        REQUIRE(log.open(nullptr, err));
        log.output("bytes", "bytes");
        log.event("Connecting to 127.0.0.1 port 2222");
        log.localOutput("local output is not logged in ssh-debug mode");
    }
    CHECK(slurp(dir.path() / "dbg.log") == "bytesEvent Log: Connecting to 127.0.0.1 port 2222\r\n");
}

TEST_CASE("sessionlog: off mode, header, directories", "[sessionlog]") {
    TempDir dir;
    {
        SessionLog log(logSession(LogMode::Off, "off.log"), dir.path());
        std::string err;
        CHECK(log.open(nullptr, err));
        CHECK_FALSE(log.isOpen());
        log.output("x", "x");
    }
    CHECK_FALSE(std::filesystem::exists(dir.path() / "off.log"));

    SessionLog log(logSession(LogMode::Printable, "sub/&H/&S-&P.log", LogIfExists::Append, true), dir.path());
    std::string err;
    REQUIRE(log.open(nullptr, err));
    const auto expected = dir.path() / "sub" / "127.0.0.1" / "test-2222.log";
    CHECK(log.path() == expected);
    log.output("hello", "hello");
    log.close();
    const std::string content = slurp(expected);
    CHECK(startsWith(content, "=~=~=~=~=~=~=~=~=~=~=~= SSHTerm log "));
    CHECK(endsWith(content, " =~=~=~=~=~=~=~=~=~=~=~=\r\nhello"));

    // Absolute template paths are used as is.
    const std::string abs = narrow((dir.path() / "abs" / "x.log").wstring());
    SessionLog absLog(logSession(LogMode::All, abs), dir.path() / "unused");
    REQUIRE(absLog.open(nullptr, err));
    CHECK(absLog.path() == dir.path() / "abs" / "x.log");
}

TEST_CASE("sessionlog: append, overwrite and ask", "[sessionlog]") {
    TempDir dir;
    const auto file = dir.path() / "same.log";
    auto writeOnce = [&](LogIfExists mode, const std::string& text, Prompter* prompter = nullptr) {
        SessionLog log(logSession(LogMode::All, "same.log", mode), dir.path());
        std::string err;
        REQUIRE(log.open(prompter, err));
        log.output(text, text);
        return log.isOpen();
    };
    CHECK(writeOnce(LogIfExists::Overwrite, "one"));
    CHECK(writeOnce(LogIfExists::Append, "two"));
    CHECK(slurp(file) == "onetwo");
    CHECK(writeOnce(LogIfExists::Overwrite, "three"));
    CHECK(slurp(file) == "three");

    ChoicePrompter p;
    p.choice = "Append";
    CHECK(writeOnce(LogIfExists::Ask, "four", &p));
    CHECK(slurp(file) == "threefour");
    CHECK(p.calls == 1);
    CHECK(p.last["kind"] == "confirm");
    CHECK(p.last["buttons"] == Json::array({"Append", "Overwrite", "Disable logging"}));

    p.choice = "Overwrite";
    CHECK(writeOnce(LogIfExists::Ask, "five", &p));
    CHECK(slurp(file) == "five");

    p.choice = "Disable logging";
    CHECK_FALSE(writeOnce(LogIfExists::Ask, "six", &p));
    p.choice.clear();
    p.cancel = true;
    CHECK_FALSE(writeOnce(LogIfExists::Ask, "seven", &p));
    CHECK(slurp(file) == "five");

    // No prompter: append. A new file is never asked about.
    CHECK(writeOnce(LogIfExists::Ask, "eight"));
    CHECK(slurp(file) == "fiveeight");
    ChoicePrompter fresh;
    SessionLog log(logSession(LogMode::All, "new.log", LogIfExists::Ask), dir.path());
    std::string err;
    REQUIRE(log.open(&fresh, err));
    CHECK(fresh.calls == 0);
}

TEST_CASE("sessionlog: a file that is in use reports an error", "[sessionlog]") {
    TempDir dir;
    SessionLog first(logSession(LogMode::All, "busy.log"), dir.path());
    std::string err;
    REQUIRE(first.open(nullptr, err));
    SessionLog second(logSession(LogMode::All, "busy.log"), dir.path());
    CHECK_FALSE(second.open(nullptr, err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(second.isOpen());
}
