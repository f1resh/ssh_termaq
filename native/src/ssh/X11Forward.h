#pragma once
// X11 forwarding, the parts that are plain logic: where the local X server is (a Unix socket or TCP, from a display
// name - also macOS XQuartz's launchd socket path), its real cookie from an Xauthority file, and the gate every
// forwarded X connection passes. The server only ever gets a random fake cookie; a client must present it in its
// connection setup, which is then passed on with the display's real cookie (or none) in its place - as PuTTY and
// OpenSSH do. Anyone on the server without the fake cookie gets the X11 "failed" reply.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace st::ssh {

struct X11Display {
    std::string unixPath;  // Unix socket to try first ("" for none)
    std::string host;      // TCP host to try then ("" for none)
    int port = 6000;       // 6000 + display number
    int display = 0;
    int screen = 0;
    bool local = false;    // this machine's X server (its Xauthority entries are by host name)
};

/**
 * Parses a display name: ":0", ":0.1", "unix:0", "localhost:10", "host:0", "[::1]:0", "tcp/host:0", or a Unix socket
 * path ending in ":N" (XQuartz: "/private/tmp/com.apple.launchd.x/org.xquartz:0"). Displays on this machine offer
 * their socket /tmp/.X11-unix/XN first, then TCP to localhost:6000+N (Windows has only the latter); "tcp/" means TCP
 * only. An empty name is the caller's to resolve first ($DISPLAY, else ":0").
 */
bool parseX11Display(const std::string& text, X11Display& out, std::string& error);

/** One entry of an Xauthority file. */
struct XauthEntry {
    std::uint16_t family = 0;  // 0 IPv4, 6 IPv6, 256 local (address = host name), 65535 any
    std::string address, number, name, data;
};
std::vector<XauthEntry> parseXauthority(const std::string& file);

/**
 * The display's MIT-MAGIC-COOKIE-1 (16 raw bytes) among Xauthority entries, nullopt when there is none. Local
 * displays match their host-name entries (`hostName`, compared without the domain); TCP displays the entries for the
 * host's address bytes (`hostAddress`, 4 or 16 bytes); entries for any address match too.
 */
std::optional<std::string> findX11Cookie(const std::vector<XauthEntry>& entries, const X11Display& display,
                                         const std::string& hostName, const std::string& hostAddress);

/** The setup of one forwarded X connection, checked and rewritten before anything reaches the X server. */
class X11AuthGate {
public:
    enum class State { Waiting, Open, Refused };

    /** `fakeCookie`: the 16 raw bytes the server was given; `realCookie`: the display's 16 raw bytes, "" for none. */
    X11AuthGate(std::string fakeCookie, std::string realCookie);

    /**
     * Takes the client's bytes until its connection setup is complete. Open: `toServer` gets the setup with the real
     * cookie (and whatever followed it). Refused: `toClient` gets the X11 "failed" reply, reason() says why.
     */
    State feed(const char* data, std::size_t len, std::string& toServer, std::string& toClient);
    const std::string& reason() const { return reason_; }

private:
    State refuse(const std::string& reason, bool msb, unsigned major, unsigned minor, std::string& toClient);

    std::string fake_, real_, buf_, reason_;
    State state_ = State::Waiting;
};

}  // namespace st::ssh
