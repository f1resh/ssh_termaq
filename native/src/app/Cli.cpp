#include "app/Cli.h"

#include "core/Strings.h"
#include "core/Uuid.h"
#include "platform/sys/Fs.h"
#include "store/Store.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace fs = std::filesystem;

namespace st {

namespace {

constexpr size_t kMaxCommandFile = 1024 * 1024;  // -m: refuse absurdly large "command" files

bool allDigits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

int parsePort(const std::string& s, bool allowZero, const char* what) {
    if (!allDigits(s) || s.size() > 5) throw std::invalid_argument(std::string("invalid ") + what + " \"" + s + "\"");
    int v = std::stoi(s);
    if (v > 65535 || (v == 0 && !allowZero)) throw std::invalid_argument(std::string("invalid ") + what + " \"" + s + "\"");
    return v;
}

/** Splits on ':' outside [...] and strips the brackets ("[::1]:80:h:22" -> "::1","80","h","22"). */
std::vector<std::string> splitColonsBracketed(const std::string& spec) {
    std::vector<std::string> out;
    std::string cur;
    bool inBrackets = false;
    for (char c : spec) {
        if (c == '[' && !inBrackets) {
            inBrackets = true;
        } else if (c == ']' && inBrackets) {
            inBrackets = false;
        } else if (c == ':' && !inBrackets) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (inBrackets) throw std::invalid_argument("unbalanced '[' in \"" + spec + "\"");
    out.push_back(cur);
    return out;
}

fs::path resolvePath(const std::string& p, const fs::path& cwd) {
    fs::path path = sys::toPath(p);
    if (path.is_relative() && !cwd.empty()) path = cwd / path;
    return path.lexically_normal();
}

std::string readTextFile(const std::string& p, const fs::path& cwd, const char* what) {
    auto content = sys::readFile(resolvePath(p, cwd));
    if (!content) throw std::invalid_argument(std::string("cannot read ") + what + " \"" + p + "\"");
    if (content->size() > kMaxCommandFile) throw std::invalid_argument(std::string(what) + " \"" + p + "\" is too large");
    if (startsWith(*content, "\xEF\xBB\xBF")) content->erase(0, 3);
    return *content;
}

std::string normalizeSerialLine(std::string line) {
    line = trim(line);
    if (line.size() > 3 && iequals(line.substr(0, 3), "com") && allDigits(std::string_view(line).substr(3)))
        return "COM" + line.substr(3);
    return line;
}

struct HostSpec {
    std::string user;
    std::string host;
    std::optional<int> port;
    std::optional<Protocol> protocol;  // from a URL scheme
    std::optional<FileProtocol> fileProtocol;  // sftp:// / scp:// open the file manager
    std::string remotePath;                    // path part of sftp:// / scp:// URLs
};

/** "%20" -> " " (invalid escapes are kept as they are). */
std::string percentDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

/** "[user@]host[:port]", "[v6]:port", bare IPv6, "telnet://host:port/", "ssh://user@host:port",
 *  "sftp://user@host:port/path", "scp://host/path". */
HostSpec parseHostSpec(std::string spec) {
    HostSpec out;
    auto scheme = spec.find("://");
    if (scheme != std::string::npos) {
        std::string s = toLower(spec.substr(0, scheme));
        if (s == "telnet")
            out.protocol = Protocol::Telnet;
        else if (s == "ssh")
            out.protocol = Protocol::Ssh;
        else if (s == "rlogin")
            out.protocol = Protocol::Rlogin;
        else if (s == "sftp" || s == "scp") {
            out.protocol = Protocol::Ssh;
            out.fileProtocol = s == "sftp" ? FileProtocol::Sftp : FileProtocol::Scp;
        } else
            throw std::invalid_argument("unsupported URL scheme \"" + s + "\"");
        spec = spec.substr(scheme + 3);
        auto slash = spec.find('/');
        if (slash != std::string::npos) {
            if (out.fileProtocol) out.remotePath = percentDecode(spec.substr(slash));
            spec.resize(slash);
        }
        if (out.fileProtocol) out.user = percentDecode(out.user);
    }
    auto at = spec.rfind('@');
    if (at != std::string::npos) {
        out.user = spec.substr(0, at);
        spec = spec.substr(at + 1);
    }
    if (!spec.empty() && spec.front() == '[') {
        auto close = spec.find(']');
        if (close == std::string::npos) throw std::invalid_argument("unbalanced '[' in host name");
        out.host = spec.substr(1, close - 1);
        std::string rest = spec.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') throw std::invalid_argument("invalid host name");
            out.port = parsePort(rest.substr(1), false, "port");
        }
    } else if (std::count(spec.begin(), spec.end(), ':') == 1) {
        auto colon = spec.find(':');
        out.host = spec.substr(0, colon);
        std::string port = spec.substr(colon + 1);
        if (!port.empty()) out.port = parsePort(port, false, "port");
    } else {
        out.host = spec;  // plain host or bare IPv6 literal
    }
    out.host = trim(out.host);
    if (out.host.empty()) throw std::invalid_argument("empty host name");
    return out;
}

/** Options that take a value and are accepted but ignored (console-tool / unsupported features). */
bool isIgnoredWithValue(const std::string& a) {
    static const char* const kList[] = {"-hostkey", "-nc", "-cert", "-sshacl", "-xrm", "-geometry", "-fn", "-fb", "-bg",
                                        "-fg", "-bfg", "-bbg", "-cfg", "-cbg", "-sl", "-title", "-log"};
    for (const char* k : kList)
        if (a == k) return true;
    return false;
}

bool isIgnoredFlag(const std::string& a) {
    static const char* const kList[] = {"-batch",          "-restrict-acl",           "-restrict_acl",    "-restrictacl",
                                        "-share",          "-noshare",                "-shareexists",     "-legacy-stdio-prompts",
                                        "-legacy-charset-handling", "-no-trivial-auth", "-no-antispoof", "-pgpfp",
                                        "-cleanup",        "-cleanup-during-uninstall", "-host-ca",       "-noagentauth",
                                        "-agentauth",      "-q",                      "-s"};
    for (const char* k : kList)
        if (a == k) return true;
    return false;
}

}  // namespace

/* ------------------------------------------------------------------------ */

HostOptions parseHostOptions(const std::vector<std::string>& args) {
    HostOptions out;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto valueOf = [&](const std::string& name, std::string& value) -> bool {
            if (a == name) {
                if (i + 1 >= args.size()) {
                    out.errors.push_back("option " + name + " requires an argument");
                    return false;
                }
                value = args[++i];
                return true;
            }
            if (startsWith(a, name + "=")) {
                value = a.substr(name.size() + 1);
                return true;
            }
            return false;
        };
        std::string value;
        if (a == "--devtools") {
            out.devtools = true;
        } else if (a == "--version" || a == "-V") {
            out.version = true;
        } else if (a.rfind("--dev-url", 0) == 0 && (a == "--dev-url" || a[9] == '=')) {
            if (valueOf("--dev-url", value)) {
                std::string lower = toLower(value);
                if (startsWith(lower, "http://") || startsWith(lower, "https://"))
                    out.devUrl = value;
                else
                    out.errors.push_back("--dev-url must be an http(s) URL");
            }
        } else if (a.rfind("--automation", 0) == 0 && (a == "--automation" || a[12] == '=')) {
            if (valueOf("--automation", value)) {
                if (trim(value).empty())
                    out.errors.push_back("--automation requires a socket path");
                else
                    out.automation = value;
            }
        } else if (a.rfind("--data-dir", 0) == 0 && (a == "--data-dir" || a[10] == '=')) {
            if (valueOf("--data-dir", value)) {
                if (trim(value).empty())
                    out.errors.push_back("--data-dir requires a directory");
                else
                    out.dataDir = sys::toPath(value);
            }
        } else {
            out.rest.push_back(a);
        }
    }
    return out;
}

PortForward parseForwardSpec(ForwardType type, const std::string& rawSpec) {
    const std::string spec = trim(rawSpec);
    if (spec.empty()) throw std::invalid_argument("empty port forwarding specification");
    std::vector<std::string> t = splitColonsBracketed(spec);
    PortForward f;
    f.id = newUuid();
    f.type = type;
    f.enabled = true;
    if (type == ForwardType::Dynamic) {
        if (t.size() == 1) {
            f.sourcePort = parsePort(t[0], false, "listening port");
        } else if (t.size() == 2) {
            f.bindAddress = trim(t[0]);
            f.sourcePort = parsePort(t[1], false, "listening port");
        } else {
            throw std::invalid_argument("invalid dynamic forwarding \"" + spec + "\" (expected [address:]port)");
        }
        f.destHost.clear();
        f.destPort = 0;
        return f;
    }
    size_t base = 0;
    if (t.size() == 4) {
        f.bindAddress = trim(t[0]);
        base = 1;
    } else if (t.size() != 3) {
        throw std::invalid_argument("invalid port forwarding \"" + spec + "\" (expected [address:]port:host:port)");
    }
    f.sourcePort = parsePort(t[base], type == ForwardType::Remote, "listening port");
    f.destHost = trim(t[base + 1]);
    if (f.destHost.empty()) throw std::invalid_argument("missing destination host in \"" + spec + "\"");
    f.destPort = parsePort(t[base + 2], false, "destination port");
    return f;
}

void applySerialConfig(SerialOptions& serial, const std::string& spec) {
    for (const std::string& raw : split(spec, ',', true)) {
        const std::string p = trim(raw);
        if (p.empty()) continue;
        if (p == "1") {
            serial.stopBits = 1;
        } else if (p == "1.5") {
            serial.stopBits = 1.5;
        } else if (p == "2") {
            serial.stopBits = 2;
        } else if (p.size() == 1 && p[0] >= '5' && p[0] <= '9') {
            serial.dataBits = p[0] - '0';
        } else if (p == "n") {
            serial.parity = Parity::None;
        } else if (p == "o") {
            serial.parity = Parity::Odd;
        } else if (p == "e") {
            serial.parity = Parity::Even;
        } else if (p == "m") {
            serial.parity = Parity::Mark;
        } else if (p == "s") {
            serial.parity = Parity::Space;
        } else if (p == "N") {
            serial.flowControl = FlowControl::None;
        } else if (p == "X") {
            serial.flowControl = FlowControl::XonXoff;
        } else if (p == "R") {
            serial.flowControl = FlowControl::RtsCts;
        } else if (p == "D") {
            serial.flowControl = FlowControl::DsrDtr;
        } else if (allDigits(p) && p.size() <= 9 && std::stol(p) > 0) {
            serial.baudRate = static_cast<int>(std::stol(p));
        } else {
            throw std::invalid_argument("invalid -sercfg item \"" + p + "\"");
        }
    }
}

CliResult parseCommandLine(const std::vector<std::string>& args, const CliContext& ctx) {
    CliResult res;

    // Pass 1: -load / @session establish the base configuration (explicit options are applied on top later).
    std::optional<SessionConfig> loaded;
    bool loadRequested = false;
    auto load = [&](const std::string& name) {
        loadRequested = true;
        loaded.reset();
        if (ctx.findSession) loaded = ctx.findSession(name);
        if (!loaded) res.errors.push_back("saved session \"" + name + "\" not found");
    };
    static const char* const kWithValue[] = {"-P", "-l", "-pw", "-pwfile", "-i", "-load", "-L", "-R", "-D", "-m", "-sercfg",
                                             "-loghost", "-proxycmd", "-sessionlog", "-sshlog", "-sshrawlog"};
    auto takesValue = [&](const std::string& a) {
        for (const char* k : kWithValue)
            if (a == k) return true;
        return isIgnoredWithValue(a);
    };
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (i == 0 && a.size() > 1 && a[0] == '@') {
            load(a.substr(1));
        } else if (a == "-load" && i + 1 < args.size()) {
            load(args[++i]);
        } else if (takesValue(a)) {
            ++i;
        }
    }

    SessionConfig s;
    if (loaded) {
        s = *loaded;
    } else {
        s = ctx.defaultTemplate;
        s.id = newUuid();
        s.name.clear();
        s.folder.clear();
        s.createdAt = s.updatedAt = s.lastUsedAt = 0;
    }
    const Protocol baseProtocol = s.protocol;

    std::optional<Protocol> protocol;
    std::optional<int> explicitPort;
    std::optional<std::string> explicitUser;
    std::optional<std::string> positional;
    std::string password;
    bool verbose = false;
    bool fileView = false;   // sftp:// / scp:// URL
    std::string remotePath;  // its path part

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&]() -> std::optional<std::string> {
            if (i + 1 >= args.size()) {
                res.errors.push_back("option " + a + " requires an argument");
                return std::nullopt;
            }
            return args[++i];
        };
        try {
            if (i == 0 && a.size() > 1 && a[0] == '@') {
                continue;  // handled in pass 1
            } else if (a == "-load") {
                value();
            } else if (a == "-ssh" || a == "-2") {
                if (a == "-ssh") protocol = Protocol::Ssh;
            } else if (a == "-telnet") {
                protocol = Protocol::Telnet;
            } else if (a == "-rlogin") {
                protocol = Protocol::Rlogin;
            } else if (a == "-raw") {
                protocol = Protocol::Raw;
            } else if (a == "-serial") {
                protocol = Protocol::Serial;
            } else if (a == "-ssh-connection" || a == "-supdup") {
                res.errors.push_back("protocol " + a.substr(1) + " is not supported");
            } else if (a == "-1") {
                res.warnings.push_back("SSH-1 is not supported; using SSH-2");
            } else if (a == "-P") {
                if (auto v = value()) explicitPort = parsePort(trim(*v), false, "port");
            } else if (a == "-l") {
                if (auto v = value()) explicitUser = *v;
            } else if (a == "-pw") {
                if (auto v = value()) password = *v;
            } else if (a == "-pwfile") {
                if (auto v = value()) {
                    std::string text = readTextFile(*v, ctx.cwd, "password file");
                    auto eol = text.find_first_of("\r\n");
                    password = eol == std::string::npos ? text : text.substr(0, eol);
                }
            } else if (a == "-i") {
                if (auto v = value()) s.ssh.auth.privateKeyPath = sys::fromPath(resolvePath(*v, ctx.cwd));
            } else if (a == "-L" || a == "-R" || a == "-D") {
                if (auto v = value()) {
                    ForwardType t = a == "-L" ? ForwardType::Local : a == "-R" ? ForwardType::Remote : ForwardType::Dynamic;
                    s.ssh.tunnels.push_back(parseForwardSpec(t, *v));
                }
            } else if (a.size() > 2 && (startsWith(a, "-L") || startsWith(a, "-R") || startsWith(a, "-D")) &&
                       (std::isdigit(static_cast<unsigned char>(a[2])) || a[2] == '[')) {
                // OpenSSH-style attached form: -L8080:host:80
                ForwardType t = a[1] == 'L' ? ForwardType::Local : a[1] == 'R' ? ForwardType::Remote : ForwardType::Dynamic;
                s.ssh.tunnels.push_back(parseForwardSpec(t, a.substr(2)));
            } else if (a == "-A") {
                s.ssh.auth.agentForwarding = true;
            } else if (a == "-a") {
                s.ssh.auth.agentForwarding = false;
            } else if (a == "-X") {
                s.ssh.x11.enabled = true;
            } else if (a == "-x") {
                s.ssh.x11.enabled = false;
            } else if (a == "-C") {
                s.ssh.compression = true;
            } else if (a == "-4") {
                s.connection.addressFamily = AddressFamily::IPv4;
            } else if (a == "-6") {
                s.connection.addressFamily = AddressFamily::IPv6;
            } else if (a == "-N") {
                s.ssh.noShell = true;
            } else if (a == "-t") {
                s.ssh.allocatePty = true;
            } else if (a == "-T") {
                s.ssh.allocatePty = false;
            } else if (a == "-v") {
                verbose = true;
            } else if (a == "-agent") {
                s.ssh.auth.tryAgent = true;
            } else if (a == "-noagent") {
                s.ssh.auth.tryAgent = false;
            } else if (a == "-m") {
                if (auto v = value()) s.ssh.remoteCommand = readTextFile(*v, ctx.cwd, "command file");
            } else if (a == "-sercfg") {
                if (auto v = value()) applySerialConfig(s.serial, *v);
            } else if (a == "-loghost") {
                if (auto v = value()) s.connection.logicalHostname = trim(*v);
            } else if (a == "-proxycmd") {
                if (auto v = value()) {
                    s.proxy.type = ProxyType::Command;
                    s.proxy.localCommand = *v;
                }
            } else if (a == "-sessionlog" || a == "-sshlog" || a == "-sshrawlog") {
                if (auto v = value()) {
                    s.logging.mode = a == "-sessionlog" ? LogMode::All : LogMode::SshDebug;
                    s.logging.path = sys::fromPath(resolvePath(*v, ctx.cwd));
                }
            } else if (a == "-logoverwrite") {
                s.logging.ifExists = LogIfExists::Overwrite;
            } else if (a == "-logappend") {
                s.logging.ifExists = LogIfExists::Append;
            } else if (isIgnoredWithValue(a)) {
                value();
                res.warnings.push_back("option " + a + " is not supported and was ignored");
            } else if (isIgnoredFlag(a)) {
                res.warnings.push_back("option " + a + " is not supported and was ignored");
            } else if (a.size() > 1 && a[0] == '-') {
                res.warnings.push_back("unknown option " + a + " was ignored");
            } else if (!positional) {
                positional = a;
            } else {
                res.warnings.push_back("extra argument \"" + a + "\" was ignored");
            }
        } catch (const std::exception& e) {
            res.errors.push_back(a + ": " + e.what());
        }
    }

    // Host / serial line.
    if (positional) {
        try {
            const Protocol finalProtocol = protocol.value_or(s.protocol);
            if (finalProtocol == Protocol::Serial) {
                s.serial.line = normalizeSerialLine(*positional);
                if (s.serial.line.empty()) throw std::invalid_argument("empty serial line");
            } else {
                HostSpec h = parseHostSpec(*positional);
                s.host = h.host;
                if (!h.user.empty()) s.username = percentDecode(h.user);
                if (h.port && !explicitPort) explicitPort = h.port;
                if (h.protocol && !protocol) protocol = h.protocol;
                if (h.fileProtocol) {
                    s.ssh.fileProtocol = *h.fileProtocol;
                    fileView = true;
                    remotePath = h.remotePath;
                }
            }
        } catch (const std::exception& e) {
            res.errors.push_back("\"" + *positional + "\": " + e.what());
            positional.reset();
        }
    }

    if (protocol && *protocol != baseProtocol) {
        s.protocol = *protocol;
        s.port = defaultPort(*protocol);
    } else if (protocol) {
        s.protocol = *protocol;
    }
    if (explicitPort) s.port = *explicitPort;
    if (explicitUser) s.username = *explicitUser;
    if (s.protocol == Protocol::Serial && !explicitPort) s.port = 0;
    if (verbose) {
        res.verbose = true;
        if (s.protocol == Protocol::Ssh && s.logging.mode == LogMode::Off) s.logging.mode = LogMode::SshDebug;
    }
    if (!password.empty() && s.protocol != Protocol::Ssh)
        res.warnings.push_back("-pw only works with SSH (ignored for this protocol)");

    const bool hasTarget = s.protocol == Protocol::Serial ? !s.serial.line.empty() : !s.host.empty();
    if ((positional || loaded) && hasTarget) {
        OpenRequest req;
        req.session = std::move(s);
        if (loaded) req.savedSessionId = loaded->id;
        if (fileView && req.session.protocol == Protocol::Ssh) {
            req.view = "files";
            req.remotePath = remotePath;
        }
        if (req.session.protocol == Protocol::Ssh) req.password = password;
        res.request = std::move(req);
    } else if (loadRequested && loaded && !hasTarget) {
        res.warnings.push_back("saved session has no host name");
    }
    return res;
}

CliResult parseCommandLine(const std::vector<std::string>& args, const Store& store, const fs::path& cwd) {
    CliContext ctx;
    ctx.defaultTemplate = store.defaultSessionTemplate();
    ctx.findSession = [&store](const std::string& name) { return store.findSessionByName(name); };
    ctx.cwd = cwd;
    return parseCommandLine(args, ctx);
}

}  // namespace st
