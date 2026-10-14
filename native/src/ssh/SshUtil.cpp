#include "ssh/SshUtil.h"

#include "core/Log.h"
#include "core/Strings.h"
#include "net/Socket.h"

#include <algorithm>
#include <cstring>

namespace st::ssh {

/* ---- UniqueSocket / WakePair ----------------------------------------------------------------------- */

void UniqueSocket::reset(SOCKET s) {
    if (s_ != INVALID_SOCKET && s_ != s) ::closesocket(s_);
    s_ = s;
}

bool WakePair::open(std::string& error) {
    SOCKET a = INVALID_SOCKET, b = INVALID_SOCKET;
    if (!makeSocketPair(a, b, error)) return false;
    setNonBlocking(a, true);
    setNonBlocking(b, true);
    std::lock_guard lock(mutex_);
    recv_ = a;
    send_ = b;
    return true;
}

void WakePair::signal() {
    std::lock_guard lock(mutex_);
    if (send_ == INVALID_SOCKET) return;
    const char byte = 1;
    // A full buffer (WSAEWOULDBLOCK) means a wake-up is already pending, which is all we need.
    ::send(send_, &byte, 1, 0);
}

void WakePair::drain() {
    if (recv_ == INVALID_SOCKET) return;
    char buf[256];
    while (::recv(recv_, buf, sizeof buf, 0) > 0) {
    }
}

void WakePair::close() {
    std::lock_guard lock(mutex_);
    closeSocket(send_);
    closeSocket(recv_);
}

/* ---- init / logging -------------------------------------------------------------------------------- */

void ensureInit() {
    static std::once_flag once;
    std::call_once(once, [] { ssh_init(); });
}

struct ThreadLogScope::State {
    LogFn fn;
    bool debug = false;
};

namespace {

thread_local ThreadLogScope::State* t_logState = nullptr;

void libsshLogCallback(int priority, const char* /*function*/, const char* buffer, void* /*userdata*/) {
    std::string msg = buffer ? buffer : "";
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    ThreadLogScope::State* st = t_logState;
    if (st && st->debug && st->fn) {
        st->fn(LogLevel::Debug, msg);
        return;
    }
    if (priority <= SSH_LOG_WARN) applog::debug("libssh: " + msg);
}

}  // namespace

ThreadLogScope::ThreadLogScope(LogFn fn, bool debug) : state_(std::make_unique<State>()) {
    state_->fn = std::move(fn);
    state_->debug = debug;
    prev_ = t_logState;
    prevLevel_ = ssh_get_log_level();
    level_ = debug ? SSH_LOG_PACKET : SSH_LOG_WARN;
    t_logState = state_.get();
    ssh_set_log_callback(&libsshLogCallback);
    ssh_set_log_level(level_);
}

ThreadLogScope::~ThreadLogScope() {
    t_logState = prev_;
    ssh_set_log_level(prevLevel_);
}

/* ---- algorithms ------------------------------------------------------------------------------------ */

std::vector<std::string> filterAlgorithms(const std::vector<std::string>& wanted, ssh_kex_types_e type,
                                          std::vector<std::string>& dropped) {
    const char* supportedList = ssh_get_supported_methods(type);
    const std::vector<std::string> supported = split(supportedList ? supportedList : "", ',', true);
    std::vector<std::string> out;
    for (const auto& raw : wanted) {
        const std::string name = trim(raw);
        if (name.empty()) continue;
        if (std::find(out.begin(), out.end(), name) != out.end()) continue;
        if (std::find(supported.begin(), supported.end(), name) != supported.end())
            out.push_back(name);
        else
            dropped.push_back(name);
    }
    return out;
}

std::vector<std::string> hostKeyAlgorithmsForKeyType(const std::string& keyType) {
    if (keyType == "ssh-rsa") return {"rsa-sha2-512", "rsa-sha2-256", "ssh-rsa"};
    if (keyType == "ssh-rsa-cert-v01@openssh.com")
        return {"rsa-sha2-512-cert-v01@openssh.com", "rsa-sha2-256-cert-v01@openssh.com", "ssh-rsa-cert-v01@openssh.com"};
    return {keyType};
}

std::vector<std::string> defaultHostKeyAlgorithms() {
    static const std::vector<std::string> preferred = {
        "ssh-ed25519",         "ecdsa-sha2-nistp521", "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp256",
        "sk-ssh-ed25519@openssh.com", "sk-ecdsa-sha2-nistp256@openssh.com", "rsa-sha2-512", "rsa-sha2-256"};
    std::vector<std::string> dropped;
    return filterAlgorithms(preferred, SSH_HOSTKEYS, dropped);
}

/* ---- PTY modes ------------------------------------------------------------------------------------- */

std::string encodePtyModes(const SessionConfig& s, int* ospeed, int* ispeed) {
    constexpr unsigned char kVerase = 3, kIutf8 = 42, kIspeed = 128, kOspeed = 129, kEnd = 0;
    std::string modes;
    auto put = [&](unsigned char op, uint32_t value) {
        modes.push_back(char(op));
        putU32(modes, value);
    };
    put(kVerase, s.keyboard.backspace == BackspaceKey::CtrlH ? 8u : 127u);
    put(kIutf8, iequals(trim(s.translation.charset), "UTF-8") ? 1u : 0u);

    // "output,input" (PuTTY order); a single number applies to both.
    int out = 38400, in = 38400;
    const auto parts = split(s.connection.terminalSpeed, ',', true);
    auto parse = [](const std::string& t, int fallback) {
        try {
            const int v = std::stoi(trim(t));
            return v > 0 ? v : fallback;
        } catch (...) {
            return fallback;
        }
    };
    if (!parts.empty()) out = parse(parts[0], out);
    in = parts.size() > 1 ? parse(parts[1], out) : out;
    put(kOspeed, uint32_t(out));
    put(kIspeed, uint32_t(in));
    modes.push_back(char(kEnd));
    if (ospeed) *ospeed = out;
    if (ispeed) *ispeed = in;
    return modes;
}

/* ---- misc ------------------------------------------------------------------------------------------ */

std::string sshError(void* obj, const std::string& fallback) {
    const char* e = obj ? ssh_get_error(obj) : nullptr;
    std::string msg = e ? trim(e) : std::string();
    return msg.empty() ? fallback : msg;
}

std::string toCrlf(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r') {
            out += "\r\n";
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else if (c == '\n') {
            out += "\r\n";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

void wipe(std::string& secret) {
    if (!secret.empty()) SecureZeroMemory(secret.data(), secret.size());
    secret.clear();
}

bool parseX11Display(const std::string& textIn, X11Display& out, std::string& error) {
    std::string text = trim(textIn);
    if (text.empty()) text = ":0";
    std::string host, rest;
    if (text[0] == '[') {
        const size_t close = text.find(']');
        if (close == std::string::npos || close + 1 >= text.size() || text[close + 1] != ':') {
            error = "Invalid X display \"" + textIn + "\"";
            return false;
        }
        host = text.substr(1, close - 1);
        rest = text.substr(close + 2);
    } else {
        const size_t colon = text.rfind(':');
        if (colon == std::string::npos) {
            error = "Invalid X display \"" + textIn + "\" (expected host:display)";
            return false;
        }
        host = text.substr(0, colon);
        rest = text.substr(colon + 1);
    }
    if (startsWith(host, "tcp/")) host = host.substr(4);
    if (host.empty() || iequals(host, "unix")) host = "localhost";
    int display = 0, screen = 0;
    try {
        const size_t dot = rest.find('.');
        display = std::stoi(rest.substr(0, dot));
        if (dot != std::string::npos && dot + 1 < rest.size()) screen = std::stoi(rest.substr(dot + 1));
    } catch (...) {
        error = "Invalid X display \"" + textIn + "\"";
        return false;
    }
    if (display < 0 || display > 59535 || screen < 0) {
        error = "Invalid X display number in \"" + textIn + "\"";
        return false;
    }
    out.host = host;
    out.display = display;
    out.screen = screen;
    out.port = 6000 + display;
    return true;
}

}  // namespace st::ssh
