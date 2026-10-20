#include "ssh/X11Forward.h"

#include "core/Strings.h"

#include <openssl/crypto.h>

namespace st::ssh {

namespace {

constexpr const char* kMitCookie = "MIT-MAGIC-COOKIE-1";

std::size_t pad4(std::size_t n) { return (n + 3) & ~static_cast<std::size_t>(3); }

bool isLocalHost(const std::string& host) {
    return iequals(host, "localhost") || host == "127.0.0.1" || host == "::1";
}

/** "N" or "N.S" after the display's colon. */
bool parseNumbers(const std::string& rest, int& display, int& screen) {
    const size_t dot = rest.find('.');
    const std::string d = rest.substr(0, dot), s = dot == std::string::npos ? "" : rest.substr(dot + 1);
    auto digits = [](const std::string& v) {
        return !v.empty() && v.size() <= 5 && v.find_first_not_of("0123456789") == std::string::npos;
    };
    if (!digits(d) || (dot != std::string::npos && !digits(s))) return false;
    display = std::stoi(d);
    screen = s.empty() ? 0 : std::stoi(s);
    return display <= 59535;
}

/** The host name without its domain, lower case ("MacBook-Air.local" -> "macbook-air"). */
std::string shortHostName(const std::string& name) { return toLower(name.substr(0, name.find('.'))); }

}  // namespace

bool parseX11Display(const std::string& textIn, X11Display& out, std::string& error) {
    const std::string text = trim(textIn);
    auto bad = [&](const std::string& why) {
        error = "Invalid X display \"" + textIn + "\"" + (why.empty() ? "" : " (" + why + ")");
        return false;
    };
    X11Display d;
    if (!text.empty() && text[0] == '/') {
        // A socket path; the display number follows its last colon.
        const size_t colon = text.rfind(':');
        if (colon == std::string::npos || !parseNumbers(text.substr(colon + 1), d.display, d.screen))
            return bad("a socket path ends in :N");
        d.unixPath = text.substr(0, colon) + ":" + std::to_string(d.display);
        d.local = true;
        out = d;
        return true;
    }
    std::string host, rest;
    if (!text.empty() && text[0] == '[') {
        const size_t close = text.find(']');
        if (close == std::string::npos || close + 1 >= text.size() || text[close + 1] != ':') return bad("");
        host = text.substr(1, close - 1);
        rest = text.substr(close + 2);
    } else {
        const size_t colon = text.rfind(':');
        if (colon == std::string::npos) return bad("expected host:display");
        host = text.substr(0, colon);
        rest = text.substr(colon + 1);
    }
    if (!parseNumbers(rest, d.display, d.screen)) return bad("");
    d.port = 6000 + d.display;
    if (startsWith(host, "tcp/")) {
        d.host = host.substr(4);
        if (d.host.empty()) return bad("");
        d.local = isLocalHost(d.host);
    } else if (host.empty() || iequals(host, "unix") || isLocalHost(host)) {
        d.unixPath = "/tmp/.X11-unix/X" + std::to_string(d.display);
        d.host = host.empty() || iequals(host, "unix") ? "localhost" : host;
        d.local = true;
    } else {
        d.host = host;
    }
    out = d;
    return true;
}

std::vector<XauthEntry> parseXauthority(const std::string& file) {
    std::vector<XauthEntry> out;
    size_t pos = 0;
    auto u16 = [&](std::uint16_t& v) {
        if (pos + 2 > file.size()) return false;
        v = static_cast<std::uint16_t>((static_cast<unsigned char>(file[pos]) << 8) | static_cast<unsigned char>(file[pos + 1]));
        pos += 2;
        return true;
    };
    auto counted = [&](std::string& v) {
        std::uint16_t n = 0;
        if (!u16(n) || pos + n > file.size()) return false;
        v = file.substr(pos, n);
        pos += n;
        return true;
    };
    while (pos < file.size()) {
        XauthEntry e;
        if (!u16(e.family) || !counted(e.address) || !counted(e.number) || !counted(e.name) || !counted(e.data)) break;
        out.push_back(std::move(e));
    }
    return out;
}

std::optional<std::string> findX11Cookie(const std::vector<XauthEntry>& entries, const X11Display& display,
                                         const std::string& hostName, const std::string& hostAddress) {
    const std::string number = std::to_string(display.display);
    std::optional<std::string> best;
    int bestRank = 0;
    for (const auto& e : entries) {
        if (e.name != kMitCookie || e.data.size() != 16 || e.number != number) continue;
        int rank = 0;
        if (e.family == 256 && display.local)
            rank = !hostName.empty() && shortHostName(e.address) == shortHostName(hostName) ? 3 : 1;  // else another name of ours
        else if ((e.family == 0 || e.family == 6) && !hostAddress.empty() && e.address == hostAddress)
            rank = 3;
        else if (e.family == 65535)
            rank = 2;
        if (rank > bestRank) {
            best = e.data;
            bestRank = rank;
        }
    }
    return best;
}

X11AuthGate::X11AuthGate(std::string fakeCookie, std::string realCookie)
    : fake_(std::move(fakeCookie)), real_(std::move(realCookie)) {}

X11AuthGate::State X11AuthGate::feed(const char* data, std::size_t len, std::string& toServer, std::string& toClient) {
    if (state_ != State::Waiting) return state_;
    buf_.append(data, len);
    // The setup: byte order ('B' or 'l'), unused, major, minor, name length, data length, unused, name, data (padded).
    if (buf_.size() < 12) return state_;
    const bool msb = buf_[0] == 'B';
    auto u16 = [&](size_t i) {
        const unsigned a = static_cast<unsigned char>(buf_[i]), b = static_cast<unsigned char>(buf_[i + 1]);
        return msb ? (a << 8) | b : (b << 8) | a;
    };
    const unsigned major = u16(2), minor = u16(4);
    if (!msb && buf_[0] != 'l') return refuse("not an X11 connection", true, 11, 0, toClient);
    const size_t nameLen = u16(6), dataLen = u16(8);
    const size_t total = 12 + pad4(nameLen) + pad4(dataLen);
    if (buf_.size() < total) return state_;
    const std::string name = buf_.substr(12, nameLen), cookie = buf_.substr(12 + pad4(nameLen), dataLen);
    if (name != kMitCookie || cookie.size() != fake_.size() || CRYPTO_memcmp(cookie.data(), fake_.data(), fake_.size()) != 0)
        return refuse(name.empty() ? "no authorization" : "wrong authorization", msb, major, minor, toClient);

    auto put16 = [&](std::string& out, unsigned v) {
        const char hi = static_cast<char>((v >> 8) & 0xff), lo = static_cast<char>(v & 0xff);
        out += msb ? hi : lo;
        out += msb ? lo : hi;
    };
    const std::string realName = real_.empty() ? std::string() : std::string(kMitCookie);
    std::string setup;
    setup += buf_[0];
    setup += '\0';
    put16(setup, major);
    put16(setup, minor);
    put16(setup, static_cast<unsigned>(realName.size()));
    put16(setup, static_cast<unsigned>(real_.size()));
    setup.append(2, '\0');
    setup += realName;
    setup.append(pad4(realName.size()) - realName.size(), '\0');
    setup += real_;
    setup.append(pad4(real_.size()) - real_.size(), '\0');
    toServer += setup;
    toServer.append(buf_, total, std::string::npos);  // what the client sent after its setup
    buf_.clear();
    return state_ = State::Open;
}

X11AuthGate::State X11AuthGate::refuse(const std::string& reason, bool msb, unsigned major, unsigned minor,
                                       std::string& toClient) {
    // Failed: 0, reason length, protocol version, length of the padded reason in 4-byte units, the reason.
    auto put16 = [&](unsigned v) {
        const char hi = static_cast<char>((v >> 8) & 0xff), lo = static_cast<char>(v & 0xff);
        toClient += msb ? hi : lo;
        toClient += msb ? lo : hi;
    };
    const std::string text = "SSHTermaq: X11 forwarding refused: " + reason;
    toClient += '\0';
    toClient += static_cast<char>(text.size());
    put16(major);
    put16(minor);
    put16(static_cast<unsigned>(pad4(text.size()) / 4));
    toClient += text;
    toClient.append(pad4(text.size()) - text.size(), '\0');
    reason_ = reason;
    buf_.clear();
    return state_ = State::Refused;
}

}  // namespace st::ssh
