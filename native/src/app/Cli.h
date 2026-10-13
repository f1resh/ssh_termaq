#pragma once
// PuTTY-compatible command line -> OpenRequest.
//
//   SSHCrawler.exe [options] [user@]host[:port]
//   SSHCrawler.exe -serial COM3 -sercfg 115200,8,n,1,N
//   SSHCrawler.exe -load "session name" | @sessionname
//   SSHCrawler.exe telnet://host[:port]/   (also ssh://[user@]host[:port])
//
// Supported PuTTY options: -ssh -telnet -rlogin -raw -serial, -P port, -l user, -pw password, -pwfile file,
// -i keyfile, -load name, -L/-R/-D (PuTTY/OpenSSH syntax), -A/-a, -X/-x, -C, -4/-6, -N, -m file, -t/-T,
// -sercfg, -v, -agent/-noagent, -loghost, -proxycmd, -sessionlog/-sshlog/-sshrawlog, -logoverwrite/-logappend,
// -2 (accepted). Options that only make sense for console tools (-batch, -nc, -hostkey, ...) are accepted and
// ignored with a warning.
// As in PuTTY, options given explicitly on the command line win over values loaded with -load, whatever their
// order. Without -load, the "Default Settings" template is the base.
// Host-only options: --dev-url URL, --devtools, --data-dir DIR (parsed by parseHostOptions).
#include "core/Types.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace sc {

class Store;

/** Options consumed by the host itself (not PuTTY options). */
struct HostOptions {
    std::string devUrl;                // --dev-url URL (http/https)
    bool devtools = false;             // --devtools
    std::filesystem::path dataDir;     // --data-dir DIR
    std::vector<std::string> rest;     // remaining (PuTTY) arguments, in order
    std::vector<std::string> errors;
};

/** Splits off the host-only options. `args` excludes argv[0]. */
HostOptions parseHostOptions(const std::vector<std::string>& args);

struct CliContext {
    /** "Default Settings" template (base when no -load). */
    SessionConfig defaultTemplate;
    /** Saved session lookup by name (case-insensitive); may be empty. */
    std::function<std::optional<SessionConfig>(const std::string& name)> findSession;
    /** Base for relative -i / -m / -pwfile / log paths (second instances forward their own cwd). */
    std::filesystem::path cwd;
};

struct CliResult {
    /** Set when the command line asks for a connection (host given, or -load of a session with a host). */
    std::optional<OpenRequest> request;
    bool verbose = false;
    std::vector<std::string> warnings;  // ignored/unsupported options
    std::vector<std::string> errors;    // invalid values (the request may still be produced)
};

/** Parses PuTTY options (`args` excludes argv[0] and host-only options). */
CliResult parseCommandLine(const std::vector<std::string>& args, const CliContext& ctx);
/** Convenience: context from a Store (default template + findSessionByName). */
CliResult parseCommandLine(const std::vector<std::string>& args, const Store& store, const std::filesystem::path& cwd);

/** Parses a PuTTY port-forward spec ("[bind:]port:host:port" for L/R, "[bind:]port" for D). Throws invalid_argument. */
PortForward parseForwardSpec(ForwardType type, const std::string& spec);
/** Applies a PuTTY -sercfg string ("19200,8,n,1,X") to `serial`. Throws invalid_argument. */
void applySerialConfig(SerialOptions& serial, const std::string& spec);

/** Splits a Windows command line (CommandLineToArgvW rules) into UTF-8 arguments, excluding argv[0]. */
std::vector<std::string> splitCommandLine(const std::wstring& commandLine);

}  // namespace sc
