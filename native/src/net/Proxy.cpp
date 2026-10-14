// Proxy support (PuTTY "Connection > Proxy"): SOCKS 4/4a, SOCKS 5 (RFC 1928/1929), HTTP CONNECT (Basic auth),
// Telnet proxy command, local proxy command (process stdio relayed over a loopback socket pair) and SSH jump
// hosts (injected opener), plus exclusion lists and PuTTY's command template language.
#include "net/Proxy.h"

#include "core/Crypto.h"
#include "core/Strings.h"
#include "net/Socket.h"
#include "session/Prompter.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace st {

namespace {

using Clock = std::chrono::steady_clock;

std::mutex g_jumpMu;
SshJumpOpener g_jumpOpener;

void logLine(const TransportContext& ctx, LogLevel level, const std::string& msg) {
    if (ctx.listener) ctx.listener->onLog(level, msg);
}

std::string proxyTypeName(ProxyType t) {
    switch (t) {
        case ProxyType::Socks4: return "SOCKS 4";
        case ProxyType::Socks5: return "SOCKS 5";
        case ProxyType::Http: return "HTTP";
        case ProxyType::Telnet: return "Telnet";
        case ProxyType::Command: return "local command";
        case ProxyType::Ssh: return "SSH";
        default: return "no";
    }
}

/** Printable form of a command for the event log (control characters escaped like the template language). */
std::string escapeForLog(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c == '\\') out += "\\\\";
        else if (c < 0x20 || c == 0x7F) {
            static const char hex[] = "0123456789ABCDEF";
            out += "\\x";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

/** Case-insensitive glob match supporting '*' (any run of characters). */
bool globMatch(std::string_view pattern, std::string_view text) {
    size_t p = 0, t = 0, starP = std::string_view::npos, starT = 0;
    auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            starP = p++;
            starT = t;
        } else if (p < pattern.size() && lower(pattern[p]) == lower(text[t])) {
            ++p;
            ++t;
        } else if (starP != std::string_view::npos) {
            p = starP + 1;
            t = ++starT;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool isLoopbackHost(const std::string& hostIn) {
    const std::string host = toLower(stripBrackets(trim(hostIn)));
    if (host == "localhost" || endsWith(host, ".localhost")) return true;
    int family = 0;
    if (!isNumericAddress(host, &family)) return false;
    if (family == AF_INET) {
        in_addr a{};
        inet_pton(AF_INET, host.c_str(), &a);
        return (ntohl(a.s_addr) >> 24) == 127;
    }
    in6_addr a6{};
    if (inet_pton(AF_INET6, host.substr(0, host.find('%')).c_str(), &a6) != 1) return false;
    static const in6_addr loop = IN6ADDR_LOOPBACK_INIT;
    if (memcmp(&a6, &loop, sizeof(a6)) == 0) return true;
    // IPv4-mapped loopback (::ffff:127.x.y.z).
    static const unsigned char mappedPrefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    return memcmp(a6.s6_addr, mappedPrefix, 12) == 0 && a6.s6_addr[12] == 127;
}

/* ------------------------------------------------------------------------ */
/* Blocking request/response I/O with cancel + deadline                       */
/* ------------------------------------------------------------------------ */

class Channel {
public:
    Channel(SOCKET s, const std::atomic<bool>* cancel, Clock::time_point deadline, std::string what)
        : s_(s), cancel_(cancel), deadline_(deadline), what_(std::move(what)) {}

    bool send(std::string_view data, std::string& error) {
        if (!sendAll(s_, data)) {
            error = what_ + ": " + lastWsaErrorMessage();
            return false;
        }
        return true;
    }

    bool recvExact(size_t n, std::string& out, std::string& error) {
        out.clear();
        while (out.size() < n) {
            if (!waitReadable(error)) return false;
            char buf[512];
            const int want = static_cast<int>(std::min<size_t>(sizeof(buf), n - out.size()));
            const int got = recv(s_, buf, want, 0);
            if (got == 0) {
                error = what_ + ": proxy closed the connection unexpectedly";
                return false;
            }
            if (got < 0) {
                error = what_ + ": " + lastWsaErrorMessage();
                return false;
            }
            out.append(buf, static_cast<size_t>(got));
        }
        return true;
    }

    /** One line terminated by LF (CR LF stripped). Reads byte by byte so nothing past the line is consumed. */
    bool recvLine(std::string& line, std::string& error) {
        line.clear();
        for (;;) {
            std::string c;
            if (!recvExact(1, c, error)) return false;
            if (c[0] == '\n') break;
            line += c[0];
            if (line.size() > 16384) {
                error = what_ + ": response line too long";
                return false;
            }
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
    }

private:
    bool waitReadable(std::string& error) {
        for (;;) {
            if (cancel_ && cancel_->load()) {
                error = "Cancelled";
                return false;
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - Clock::now()).count();
            if (left <= 0) {
                error = what_ + ": timed out waiting for the proxy to respond";
                return false;
            }
            const int slice = static_cast<int>(std::min<long long>(left, 100));
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(s_, &rfds);
            timeval tv{0, slice * 1000};
            const int n = select(0, &rfds, nullptr, nullptr, &tv);
            if (n == SOCKET_ERROR) {
                error = what_ + ": " + lastWsaErrorMessage();
                return false;
            }
            if (n > 0) return true;
        }
    }

    SOCKET s_;
    const std::atomic<bool>* cancel_;
    Clock::time_point deadline_;
    std::string what_;
};

/* ------------------------------------------------------------------------ */
/* Credentials                                                               */
/* ------------------------------------------------------------------------ */

bool promptCredentials(const TransportContext& ctx, ProxyConfig& p, bool needUser, bool needPass, const std::string& what,
                       std::string& error) {
    if ((!needUser || !p.username.empty()) && (!needPass || !p.password.empty())) return true;
    if (!ctx.prompter) {
        error = what + " requires a proxy username and password (none configured)";
        return false;
    }
    if (needUser && p.username.empty()) {
        PromptResponse r = ctx.prompter->prompt(prompts::username("Proxy authentication", what));
        if (r.cancelled || r.values.empty() || r.values[0].empty()) {
            error = "Proxy authentication cancelled";
            return false;
        }
        p.username = r.values[0];
    }
    if (needPass && p.password.empty()) {
        PromptResponse r = ctx.prompter->prompt(prompts::password("Proxy authentication", what, p.username, false));
        if (r.cancelled || r.values.empty()) {
            error = "Proxy authentication cancelled";
            return false;
        }
        p.password = r.values[0];
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* SOCKS 4 / 4a                                                              */
/* ------------------------------------------------------------------------ */

bool socks4Handshake(Channel& ch, const ProxyConfig& p, const std::string& host, int port, std::string& error) {
    int family = 0;
    const bool numeric = isNumericAddress(host, &family);
    if (numeric && family != AF_INET) {
        error = "SOCKS 4 proxies cannot connect to IPv6 addresses";
        return false;
    }
    std::string req;
    req += '\x04';
    req += '\x01';
    req += static_cast<char>((port >> 8) & 0xFF);
    req += static_cast<char>(port & 0xFF);
    if (numeric) {
        in_addr a{};
        inet_pton(AF_INET, stripBrackets(host).c_str(), &a);
        req.append(reinterpret_cast<const char*>(&a), 4);  // network order
    } else {
        req.append("\x00\x00\x00\x01", 4);  // SOCKS 4a: invalid IP => host name follows the user id
    }
    req += p.username;
    req += '\0';
    if (!numeric) {
        req += host;
        req += '\0';
    }
    if (!ch.send(req, error)) return false;
    std::string reply;
    if (!ch.recvExact(8, reply, error)) return false;
    if (reply[0] != 0) {
        error = "SOCKS 4 proxy returned an unexpected reply version";
        return false;
    }
    switch (static_cast<unsigned char>(reply[1])) {
        case 90: return true;
        case 92: error = "SOCKS 4 proxy could not contact the client's identd"; return false;
        case 93: error = "SOCKS 4 proxy reported an identd username mismatch"; return false;
        case 91: error = "SOCKS 4 proxy rejected the request or the connection failed"; return false;
        default:
            error = "SOCKS 4 proxy returned unrecognised reply code " + std::to_string(static_cast<unsigned char>(reply[1]));
            return false;
    }
}

/* ------------------------------------------------------------------------ */
/* SOCKS 5                                                                   */
/* ------------------------------------------------------------------------ */

std::string socks5ReplyText(unsigned char code) {
    switch (code) {
        case 1: return "general SOCKS server failure";
        case 2: return "connection not allowed by ruleset";
        case 3: return "network unreachable";
        case 4: return "host unreachable";
        case 5: return "connection refused";
        case 6: return "TTL expired";
        case 7: return "command not supported";
        case 8: return "address type not supported";
        default: return "unrecognised reply code " + std::to_string(code);
    }
}

bool socks5Handshake(const TransportContext& ctx, Channel& ch, ProxyConfig& p, const std::string& host, int port,
                     std::string& error) {
    const bool haveUser = !p.username.empty();
    std::string greet = haveUser ? std::string("\x05\x02\x00\x02", 4) : std::string("\x05\x01\x00", 3);
    if (!ch.send(greet, error)) return false;
    std::string reply;
    if (!ch.recvExact(2, reply, error)) return false;
    if (reply[0] != 5) {
        error = "SOCKS 5 proxy returned an unexpected reply version";
        return false;
    }
    const auto method = static_cast<unsigned char>(reply[1]);
    if (method == 0xFF) {
        error = haveUser ? "SOCKS 5 proxy rejected all offered authentication methods"
                         : "SOCKS 5 proxy requires authentication; set a proxy username and password";
        return false;
    }
    if (method == 2) {
        if (!haveUser) {
            error = "SOCKS 5 proxy selected username/password authentication, which was not offered";
            return false;
        }
        if (!promptCredentials(ctx, p, true, true, "SOCKS 5 proxy " + formatHostPort(p.host, p.port), error)) return false;
        if (p.username.size() > 255 || p.password.size() > 255) {
            error = "SOCKS 5 proxy username or password is longer than 255 bytes";
            return false;
        }
        std::string auth;
        auth += '\x01';
        auth += static_cast<char>(p.username.size());
        auth += p.username;
        auth += static_cast<char>(p.password.size());
        auth += p.password;
        if (!ch.send(auth, error)) return false;
        if (!ch.recvExact(2, reply, error)) return false;
        if (reply[1] != 0) {
            error = "SOCKS 5 proxy refused username/password authentication";
            return false;
        }
    } else if (method != 0) {
        error = "SOCKS 5 proxy selected an unsupported authentication method (" + std::to_string(method) + ")";
        return false;
    }

    std::string req("\x05\x01\x00", 3);
    int family = 0;
    const std::string bare = stripBrackets(host);
    if (isNumericAddress(bare, &family) && family == AF_INET) {
        in_addr a{};
        inet_pton(AF_INET, bare.c_str(), &a);
        req += '\x01';
        req.append(reinterpret_cast<const char*>(&a), 4);
    } else if (isNumericAddress(bare, &family) && family == AF_INET6) {
        in6_addr a6{};
        inet_pton(AF_INET6, bare.substr(0, bare.find('%')).c_str(), &a6);
        req += '\x04';
        req.append(reinterpret_cast<const char*>(&a6), 16);
    } else {
        if (bare.size() > 255) {
            error = "Host name too long for a SOCKS 5 proxy";
            return false;
        }
        req += '\x03';
        req += static_cast<char>(bare.size());
        req += bare;
    }
    req += static_cast<char>((port >> 8) & 0xFF);
    req += static_cast<char>(port & 0xFF);
    if (!ch.send(req, error)) return false;

    if (!ch.recvExact(4, reply, error)) return false;
    if (reply[0] != 5) {
        error = "SOCKS 5 proxy returned an unexpected reply version";
        return false;
    }
    const auto rep = static_cast<unsigned char>(reply[1]);
    if (rep != 0) {
        error = "SOCKS 5 proxy reported: " + socks5ReplyText(rep);
        return false;
    }
    size_t addrLen = 0;
    switch (static_cast<unsigned char>(reply[3])) {
        case 1: addrLen = 4; break;
        case 4: addrLen = 16; break;
        case 3: {
            std::string len;
            if (!ch.recvExact(1, len, error)) return false;
            addrLen = static_cast<unsigned char>(len[0]);
            break;
        }
        default: error = "SOCKS 5 proxy returned an unknown bound address type"; return false;
    }
    std::string rest;
    return ch.recvExact(addrLen + 2, rest, error);
}

/* ------------------------------------------------------------------------ */
/* HTTP CONNECT                                                              */
/* ------------------------------------------------------------------------ */

/** Result: 0 = ok, 1 = failed (error set), 2 = 407 authentication required. */
int httpConnect(Channel& ch, const ProxyConfig& p, const std::string& host, int port, std::string& error) {
    const std::string target = formatHostPort(host, port);
    std::string req = "CONNECT " + target + " HTTP/1.1\r\nHost: " + target + "\r\n";
    if (!p.username.empty() || !p.password.empty())
        req += "Proxy-Authorization: Basic " + crypto::base64Encode(p.username + ":" + p.password) + "\r\n";
    req += "\r\n";
    if (!ch.send(req, error)) return 1;

    std::string status;
    if (!ch.recvLine(status, error)) return 1;
    // "HTTP/1.1 200 Connection established"
    if (!startsWith(status, "HTTP/") || status.find(' ') == std::string::npos) {
        error = "HTTP proxy returned a malformed response";
        return 1;
    }
    std::string rest = trim(status.substr(status.find(' ') + 1));
    int code = 0;
    size_t i = 0;
    while (i < rest.size() && i < 3 && std::isdigit(static_cast<unsigned char>(rest[i]))) code = code * 10 + (rest[i++] - '0');
    if (i != 3) {
        error = "HTTP proxy returned a malformed status line";
        return 1;
    }
    // Drain the response headers (the tunnel starts right after the empty line).
    for (int lines = 0;; ++lines) {
        std::string header;
        if (!ch.recvLine(header, error)) return 1;
        if (header.empty()) break;
        if (lines > 200) {
            error = "HTTP proxy sent too many response headers";
            return 1;
        }
    }
    if (code >= 200 && code < 300) return 0;
    if (code == 407) {
        error = "HTTP proxy authentication " + std::string(p.username.empty() ? "required" : "failed") + " (" + rest + ")";
        return 2;
    }
    error = "HTTP proxy refused the connection: " + rest;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Local proxy command relay                                                 */
/* ------------------------------------------------------------------------ */

struct HandleCloser {
    void operator()(HANDLE h) const {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

/** Shared state of one local-command relay; destroyed when the registry and all relay threads released it. */
struct CommandRelay {
    SOCKET relaySock = INVALID_SOCKET;  // our end of the socket pair (the caller holds the other end)
    UniqueHandle stdinWrite, stdoutRead, stderrRead, process, job;

    std::mutex logMu;
    TransportListener* listener = nullptr;  // null once detached

    std::mutex killMu;
    bool killed = false;

    ~CommandRelay() { closeSocket(relaySock); }

    void log(LogLevel level, const std::string& msg) {
        std::lock_guard lock(logMu);
        if (listener) listener->onLog(level, msg);
    }
    void detach() {
        std::lock_guard lock(logMu);
        listener = nullptr;
    }
    /** Terminates the whole process tree (job object with KILL_ON_JOB_CLOSE). */
    void kill() {
        std::lock_guard lock(killMu);
        if (killed) return;
        killed = true;
        if (process) TerminateProcess(process.get(), 1);
        job.reset();
    }
};

std::mutex g_relayMu;
std::map<SOCKET, std::shared_ptr<CommandRelay>> g_relays;

std::shared_ptr<CommandRelay> takeRelay(SOCKET s) {
    std::lock_guard lock(g_relayMu);
    auto it = g_relays.find(s);
    if (it == g_relays.end()) return nullptr;
    auto r = it->second;
    g_relays.erase(it);
    return r;
}

bool createPipe(UniqueHandle& parentEnd, UniqueHandle& childEnd, bool childReads, std::string& error) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE r = nullptr, w = nullptr;
    if (!CreatePipe(&r, &w, &sa, 0)) {
        error = "Unable to create pipe: " + lastErrorMessage();
        return false;
    }
    parentEnd.reset(childReads ? w : r);
    childEnd.reset(childReads ? r : w);
    SetHandleInformation(parentEnd.get(), HANDLE_FLAG_INHERIT, 0);
    return true;
}

SOCKET startLocalCommand(const TransportContext& ctx, const std::string& command, std::string& error) {
    SOCKET mine = INVALID_SOCKET, relay = INVALID_SOCKET;
    if (!makeSocketPair(mine, relay, error)) return INVALID_SOCKET;
    auto r = std::make_shared<CommandRelay>();
    r->relaySock = relay;
    r->listener = ctx.listener;

    UniqueHandle childIn, childOut, childErr;
    if (!createPipe(r->stdinWrite, childIn, true, error) || !createPipe(r->stdoutRead, childOut, false, error) ||
        !createPipe(r->stderrRead, childErr, false, error)) {
        closeSocket(mine);
        return INVALID_SOCKET;
    }

    // Only the three pipe ends may be inherited (other threads may have inheritable handles open).
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<unsigned char> attrBuf(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    HANDLE inherit[3] = {childIn.get(), childOut.get(), childErr.get()};
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr)) {
        error = "Unable to start proxy command: " + lastErrorMessage();
        closeSocket(mine);
        return INVALID_SOCKET;
    }
    struct AttrGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST a;
        ~AttrGuard() { DeleteProcThreadAttributeList(a); }
    } attrGuard{attrs};

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = childIn.get();
    si.StartupInfo.hStdOutput = childOut.get();
    si.StartupInfo.hStdError = childErr.get();
    si.lpAttributeList = attrs;

    wchar_t comspec[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH);
    std::wstring shell = (n > 0 && n < MAX_PATH) ? std::wstring(comspec, n) : std::wstring(L"cmd.exe");
    std::wstring cmdline = L"\"" + shell + L"\" /d /s /c \"" + widen(command) + L"\"";

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                        &si.StartupInfo, &pi)) {
        error = "Unable to start proxy command: " + lastErrorMessage();
        closeSocket(mine);
        return INVALID_SOCKET;
    }
    r->process.reset(pi.hProcess);
    UniqueHandle thread(pi.hThread);
    r->job.reset(CreateJobObjectW(nullptr, nullptr));
    if (r->job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(r->job.get(), JobObjectExtendedLimitInformation, &li, sizeof(li));
        AssignProcessToJobObject(r->job.get(), pi.hProcess);
    }
    ResumeThread(pi.hThread);
    childIn.reset();
    childOut.reset();
    childErr.reset();

    {
        std::lock_guard lock(g_relayMu);
        g_relays[mine] = r;
    }

    // socket -> process stdin. EOF/error on the socket means the transport closed its end: kill the command.
    std::thread([r, mine] {
        char buf[16384];
        for (;;) {
            const int got = recv(r->relaySock, buf, sizeof(buf), 0);
            if (got <= 0) break;
            const char* p = buf;
            DWORD left = static_cast<DWORD>(got);
            bool ok = true;
            while (left > 0) {
                DWORD written = 0;
                if (!WriteFile(r->stdinWrite.get(), p, left, &written, nullptr)) {
                    ok = false;
                    break;
                }
                p += written;
                left -= written;
            }
            if (!ok) break;
        }
        r->stdinWrite.reset();
        r->kill();
        shutdown(r->relaySock, SD_BOTH);
        std::lock_guard lock(g_relayMu);
        auto it = g_relays.find(mine);
        if (it != g_relays.end() && it->second == r) g_relays.erase(it);
    }).detach();

    // process stdout -> socket. EOF (process exited) half-closes the socket so the transport sees the close.
    std::thread([r] {
        char buf[16384];
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(r->stdoutRead.get(), buf, sizeof(buf), &got, nullptr) || got == 0) break;
            if (!sendAll(r->relaySock, std::string_view(buf, got))) break;
        }
        shutdown(r->relaySock, SD_SEND);
    }).detach();

    // process stderr -> event log, line by line.
    std::thread([r] {
        std::string pending;
        char buf[4096];
        auto flushLine = [&](std::string line) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (!line.empty()) r->log(LogLevel::Warn, "proxy: " + line);
        };
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(r->stderrRead.get(), buf, sizeof(buf), &got, nullptr) || got == 0) break;
            pending.append(buf, got);
            size_t pos;
            while ((pos = pending.find('\n')) != std::string::npos) {
                flushLine(pending.substr(0, pos));
                pending.erase(0, pos + 1);
            }
            if (pending.size() > 4096) {
                flushLine(pending);
                pending.clear();
            }
        }
        flushLine(pending);
    }).detach();

    return mine;
}

/* ------------------------------------------------------------------------ */
/* Helpers for openConnection                                                */
/* ------------------------------------------------------------------------ */

ConnectOptions baseConnectOptions(const TransportContext& ctx, int timeoutMs) {
    const auto& c = ctx.session.connection;
    ConnectOptions o;
    o.family = c.addressFamily;
    o.timeoutMs = timeoutMs;
    o.noDelay = c.tcpNoDelay;
    o.keepAlive = c.tcpKeepalive;
    const TransportContext* pctx = &ctx;
    o.log = [pctx](const std::string& line) { logLine(*pctx, LogLevel::Info, line); };
    return o;
}

bool templateUses(const std::string& tmpl, const char* token) { return tmpl.find(token) != std::string::npos; }

}  // namespace

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

void setSshJumpOpener(SshJumpOpener opener) {
    std::lock_guard lock(g_jumpMu);
    g_jumpOpener = std::move(opener);
}

bool proxyResolvesRemotely(const ProxyConfig& proxy) {
    switch (proxy.dnsAtProxy) {
        case TriState::On: return true;
        case TriState::Off: return false;
        default: return proxy.type != ProxyType::Socks4;
    }
}

bool proxyExcludes(const ProxyConfig& proxy, const std::string& hostIn) {
    const std::string host = stripBrackets(trim(hostIn));
    if (proxy.type == ProxyType::None) return true;
    if (!proxy.proxyLocalhost && isLoopbackHost(host)) return true;
    std::string list = proxy.excludeHosts;
    for (char& c : list)
        if (c == ',' || c == ';' || c == '\t' || c == '\r' || c == '\n') c = ' ';
    for (const std::string& raw : split(list, ' ', true)) {
        const std::string pattern = stripBrackets(trim(raw));
        if (!pattern.empty() && globMatch(pattern, host)) return true;
    }
    return false;
}

std::string expandProxyTemplate(const std::string& tmpl, const ProxyConfig& proxy, const std::string& host, int port) {
    std::string out;
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    size_t i = 0;
    while (i < tmpl.size()) {
        const char c = tmpl[i];
        if (c == '\\' && i + 1 < tmpl.size()) {
            const char e = tmpl[i + 1];
            switch (e) {
                case 'n': out += '\n'; i += 2; continue;
                case 'r': out += '\r'; i += 2; continue;
                case 't': out += '\t'; i += 2; continue;
                case '\\': out += '\\'; i += 2; continue;
                case '%': out += '%'; i += 2; continue;
                case 'x':
                case 'X':
                    if (i + 3 < tmpl.size() && hexVal(tmpl[i + 2]) >= 0 && hexVal(tmpl[i + 3]) >= 0) {
                        out += static_cast<char>(hexVal(tmpl[i + 2]) * 16 + hexVal(tmpl[i + 3]));
                        i += 4;
                        continue;
                    }
                    break;
                default: break;
            }
            out += c;  // unknown escape: copied literally
            ++i;
            continue;
        }
        if (c == '%') {
            const std::string_view rest(tmpl.data() + i + 1, tmpl.size() - i - 1);
            struct Token {
                const char* name;
                std::string value;
            };
            const Token tokens[] = {
                {"%", "%"},
                {"host", stripBrackets(host)},
                {"port", std::to_string(port)},
                {"user", proxy.username},
                {"pass", proxy.password},
                {"proxyhost", stripBrackets(proxy.host)},
                {"proxyport", std::to_string(proxy.port)},
            };
            bool matched = false;
            for (const auto& t : tokens) {
                if (startsWith(rest, t.name)) {
                    out += t.value;
                    i += 1 + strlen(t.name);
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
        }
        out += c;
        ++i;
    }
    return out;
}

SOCKET openConnection(const TransportContext& ctx, const std::string& host, int port, std::string& error,
                      const std::atomic<bool>* cancel) {
    return openConnection(ctx, host, port, error, cancel, OpenOptions{});
}

SOCKET openConnection(const TransportContext& ctx, const std::string& hostIn, int port, std::string& error,
                      const std::atomic<bool>* cancel, const OpenOptions& options) {
    error.clear();
    if (options.viaProxy) *options.viaProxy = false;
    const std::string host = stripBrackets(trim(hostIn));
    if (host.empty()) {
        error = "No host name specified";
        return INVALID_SOCKET;
    }
    if (port <= 0 || port > 65535) {
        error = "Invalid port number " + std::to_string(port);
        return INVALID_SOCKET;
    }
    const int timeoutSec = ctx.session.connection.connectTimeoutSeconds > 0 ? ctx.session.connection.connectTimeoutSeconds : 20;
    const int timeoutMs = timeoutSec * 1000;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    ProxyConfig proxy = ctx.session.proxy;

    auto direct = [&]() -> SOCKET {
        ConnectOptions o = baseConnectOptions(ctx, timeoutMs);
        o.localPortHigh = options.localPortHigh;
        o.localPortLow = options.localPortLow;
        std::string addr;
        SOCKET s = connectTcp(host, port, o, error, cancel, &addr);
        if (s != INVALID_SOCKET && options.connectedTo) *options.connectedTo = addr + " port " + std::to_string(port);
        return s;
    };

    if (proxy.type == ProxyType::None || proxyExcludes(proxy, host)) return direct();

    // Name resolution: local unless the proxy resolves names (PuTTY "Do DNS name lookup at proxy end").
    std::string target = host;
    if (!isNumericAddress(host) && !proxyResolvesRemotely(proxy)) {
        const AddressFamily fam = proxy.type == ProxyType::Socks4 ? AddressFamily::IPv4 : ctx.session.connection.addressFamily;
        logLine(ctx, LogLevel::Info, "Looking up host \"" + host + "\" for proxy connection");
        std::vector<std::string> addrs = resolveHost(host, fam, error, cancel, timeoutMs);
        if (addrs.empty()) return INVALID_SOCKET;
        for (const auto& a : addrs) {
            if (proxyExcludes(proxy, a)) return direct();
        }
        target = addrs.front();
    }

    if (options.viaProxy) *options.viaProxy = true;
    const std::string typeName = proxyTypeName(proxy.type);
    const std::string targetText = formatHostPort(target, port);

    if (proxy.type == ProxyType::Command) {
        const bool needUser = templateUses(proxy.localCommand, "%user");
        const bool needPass = templateUses(proxy.localCommand, "%pass");
        if (!promptCredentials(ctx, proxy, needUser, needPass, "Local proxy command", error)) return INVALID_SOCKET;
        if (trim(proxy.localCommand).empty()) {
            error = "No local proxy command configured";
            return INVALID_SOCKET;
        }
        const std::string cmd = expandProxyTemplate(proxy.localCommand, proxy, target, port);
        ProxyConfig masked = proxy;
        if (!masked.password.empty()) masked.password = "********";
        logLine(ctx, LogLevel::Info, "Starting local proxy command: " + escapeForLog(expandProxyTemplate(proxy.localCommand, masked, target, port)));
        SOCKET s = startLocalCommand(ctx, cmd, error);
        if (s != INVALID_SOCKET && options.connectedTo) *options.connectedTo = targetText + " (local proxy command)";
        return s;
    }

    if (proxy.type == ProxyType::Ssh) {
        SshJumpOpener opener;
        {
            std::lock_guard lock(g_jumpMu);
            opener = g_jumpOpener;
        }
        if (!opener) {
            error = "SSH jump host proxies are not available";
            return INVALID_SOCKET;
        }
        SessionConfig jump;
        if (!proxy.jumpSessionId.empty()) {
            std::optional<SessionConfig> resolved = ctx.resolveSession ? ctx.resolveSession(proxy.jumpSessionId) : std::nullopt;
            if (!resolved) {
                error = "Jump host session not found (it may have been deleted)";
                return INVALID_SOCKET;
            }
            jump = *resolved;
        } else {
            if (trim(proxy.host).empty()) {
                error = "No SSH jump host configured";
                return INVALID_SOCKET;
            }
            jump.id = "jump:" + proxy.host;
            jump.name = proxy.host;
            jump.host = stripBrackets(trim(proxy.host));
            jump.port = proxy.port > 0 ? proxy.port : 22;
            jump.username = proxy.username;
            jump.connection = ctx.session.connection;
            jump.connection.environment.clear();
        }
        jump.protocol = Protocol::Ssh;
        logLine(ctx, LogLevel::Info, "Will use SSH jump host " + formatHostPort(jump.host, jump.port) + " to connect to " + targetText);
        SOCKET s = opener(ctx, jump, target, port, error, cancel);
        if (s == INVALID_SOCKET && error.empty()) error = "Unable to open SSH jump host tunnel";
        if (s != INVALID_SOCKET && options.connectedTo)
            *options.connectedTo = targetText + " (via SSH jump host " + formatHostPort(jump.host, jump.port) + ")";
        return s;
    }

    if (trim(proxy.host).empty()) {
        error = "No proxy host configured";
        return INVALID_SOCKET;
    }
    const std::string proxyText = formatHostPort(proxy.host, proxy.port);
    logLine(ctx, LogLevel::Info, "Will use " + typeName + " proxy at " + proxyText + " to connect to " + targetText);

    for (int attempt = 0; attempt < 2; ++attempt) {
        const int left = static_cast<int>(
            std::max<long long>(1, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count()));
        ConnectOptions o = baseConnectOptions(ctx, left);
        std::string perr;
        SOCKET s = connectTcp(proxy.host, proxy.port, o, perr, cancel);
        if (s == INVALID_SOCKET) {
            error = perr == "Cancelled" ? perr : "Unable to connect to " + typeName + " proxy " + proxyText + ": " + perr;
            return INVALID_SOCKET;
        }
        Channel ch(s, cancel, deadline, typeName + " proxy");
        bool ok = false;
        bool retryWithAuth = false;
        switch (proxy.type) {
            case ProxyType::Socks4: ok = socks4Handshake(ch, proxy, target, port, error); break;
            case ProxyType::Socks5: ok = socks5Handshake(ctx, ch, proxy, target, port, error); break;
            case ProxyType::Http: {
                const int rc = httpConnect(ch, proxy, target, port, error);
                ok = rc == 0;
                if (rc == 2 && attempt == 0 && proxy.username.empty() && proxy.password.empty() && ctx.prompter) {
                    std::string perr2;
                    if (promptCredentials(ctx, proxy, true, true, "HTTP proxy " + proxyText, perr2)) {
                        retryWithAuth = true;
                    } else {
                        error = perr2;
                    }
                }
                break;
            }
            case ProxyType::Telnet: {
                const bool needUser = templateUses(proxy.telnetCommand, "%user");
                const bool needPass = templateUses(proxy.telnetCommand, "%pass");
                if (!promptCredentials(ctx, proxy, needUser, needPass, "Telnet proxy " + proxyText, error)) break;
                ProxyConfig masked = proxy;
                if (!masked.password.empty()) masked.password = "********";
                logLine(ctx, LogLevel::Info,
                        "Sending Telnet proxy command: " + escapeForLog(expandProxyTemplate(proxy.telnetCommand, masked, target, port)));
                ok = ch.send(expandProxyTemplate(proxy.telnetCommand, proxy, target, port), error);
                break;
            }
            default: error = "Unsupported proxy type"; break;
        }
        if (ok) {
            logLine(ctx, LogLevel::Info, typeName + " proxy connection to " + targetText + " established");
            if (options.connectedTo) *options.connectedTo = targetText + " (via " + typeName + " proxy " + proxyText + ")";
            return s;
        }
        closeSocket(s);
        if (!retryWithAuth) break;
        error.clear();
    }
    if (error.empty()) error = typeName + " proxy connection failed";
    return INVALID_SOCKET;
}

void detachConnection(SOCKET s, bool terminate) {
    if (s == INVALID_SOCKET) return;
    if (terminate) {
        if (std::shared_ptr<CommandRelay> relay = takeRelay(s)) {
            relay->detach();
            relay->kill();
        }
        return;
    }
    std::lock_guard lock(g_relayMu);
    auto it = g_relays.find(s);
    if (it != g_relays.end()) it->second->detach();
}

void closeConnection(SOCKET& s) {
    if (s == INVALID_SOCKET) return;
    std::shared_ptr<CommandRelay> relay = takeRelay(s);
    if (relay) relay->detach();
    closeSocket(s);
    if (relay) relay->kill();
}

}  // namespace st
