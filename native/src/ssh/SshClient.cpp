#include "ssh/SshClient.h"

#include "core/Crypto.h"
#include "core/Strings.h"
#include "keys/KeyLoader.h"
#include "session/Prompter.h"
#include "ssh/SshAgent.h"
#include "store/Credentials.h"
#include "store/KnownHosts.h"

#include <algorithm>

namespace sc::ssh {

namespace {

std::string keyBlobOf(ssh_key key) {
    char* b64 = nullptr;
    if (ssh_pki_export_pubkey_base64(key, &b64) != SSH_OK || !b64) return {};
    std::string out;
    crypto::base64Decode(b64, out);
    ssh_string_free_char(b64);
    return out;
}

std::string joinList(const std::vector<std::string>& v) { return join(v, ","); }

}  // namespace

SshConnector::SshConnector(const TransportContext& ctx, const SessionConfig& session, std::string logPrefix,
                           const std::atomic<bool>& cancel)
    : ctx_(ctx), cfg_(session), prefix_(std::move(logPrefix)), cancel_(cancel), user_(session.username) {}

SshConnector::~SshConnector() = default;

void SshConnector::log(LogLevel level, const std::string& msg) const {
    if (ctx_.listener) ctx_.listener->onLog(level, prefix_ + msg);
}

void SshConnector::local(const std::string& text) const {
    if (ctx_.listener) ctx_.listener->onLocalOutput(toCrlf(text));
}

bool SshConnector::applyOptions(ssh_session s, SOCKET sock, int logLevel, std::string& error) {
    auto set = [&](ssh_options_e opt, const void* v, const char* what) {
        if (ssh_options_set(s, opt, v) != SSH_OK) {
            error = std::string("Invalid SSH option (") + what + "): " + sshError(s, "rejected");
            return false;
        }
        return true;
    };
    const bool no = false;
    unsigned int port = static_cast<unsigned int>(cfg_.port > 0 ? cfg_.port : 22);
    long timeout = std::max(1, cfg_.connection.connectTimeoutSeconds);
    int nodelay = cfg_.connection.tcpNoDelay ? 1 : 0;
    socket_t fd = static_cast<socket_t>(sock);
    if (!set(SSH_OPTIONS_PROCESS_CONFIG, &no, "process config")) return false;
    if (!set(SSH_OPTIONS_HOST, cfg_.host.c_str(), "host")) return false;
    if (!set(SSH_OPTIONS_PORT, &port, "port")) return false;
    if (!set(SSH_OPTIONS_FD, &fd, "socket")) return false;
    if (!set(SSH_OPTIONS_TIMEOUT, &timeout, "timeout")) return false;
    set(SSH_OPTIONS_NODELAY, &nodelay, "nodelay");
    set(SSH_OPTIONS_LOG_VERBOSITY, &logLevel, "log level");
    if (!user_.empty() && !set(SSH_OPTIONS_USER, user_.c_str(), "user")) return false;
    set(SSH_OPTIONS_COMPRESSION, cfg_.ssh.compression ? "yes" : "no", "compression");

    std::vector<std::string> dropped;
    auto apply = [&](const std::vector<std::string>& wanted, ssh_kex_types_e type, ssh_options_e o1, ssh_options_e o2,
                     const char* what) {
        if (wanted.empty()) return true;
        dropped.clear();
        auto names = filterAlgorithms(wanted, type, dropped);
        for (const auto& d : dropped) log(LogLevel::Warn, std::string("Ignoring unsupported ") + what + " " + d);
        if (names.empty()) return true;
        const std::string list = joinList(names);
        if (!set(o1, list.c_str(), what)) return false;
        if (o2 != o1 && !set(o2, list.c_str(), what)) return false;
        return true;
    };
    // Default key exchange: libssh's defaults minus the ML-KEM hybrids, whose client side fails to build its
    // init message in this libssh/OpenSSL combination ("Failed to construct client init buffer").
    static const std::vector<std::string> kDefaultKex = {
        "curve25519-sha256",         "curve25519-sha256@libssh.org",         "sntrup761x25519-sha512",
        "sntrup761x25519-sha512@openssh.com", "ecdh-sha2-nistp256",         "ecdh-sha2-nistp384",
        "ecdh-sha2-nistp521",        "diffie-hellman-group-exchange-sha256", "diffie-hellman-group16-sha512",
        "diffie-hellman-group18-sha512", "diffie-hellman-group14-sha256"};
    if (!apply(cfg_.ssh.kex.empty() ? kDefaultKex : cfg_.ssh.kex, SSH_KEX, SSH_OPTIONS_KEY_EXCHANGE,
               SSH_OPTIONS_KEY_EXCHANGE, "key exchange"))
        return false;
    if (!apply(cfg_.ssh.ciphers, SSH_CRYPT_C_S, SSH_OPTIONS_CIPHERS_C_S, SSH_OPTIONS_CIPHERS_S_C, "cipher")) return false;
    if (!apply(cfg_.ssh.macs, SSH_MAC_C_S, SSH_OPTIONS_HMAC_C_S, SSH_OPTIONS_HMAC_S_C, "MAC")) return false;

    // Host key algorithms: user preference (or defaults), with algorithms matching already-trusted keys first so an
    // existing host never prompts just because the server prefers a different key type (PuTTY does the same).
    std::vector<std::string> hk = cfg_.ssh.hostKeyAlgorithms.empty() ? defaultHostKeyAlgorithms() : [&] {
        dropped.clear();
        auto v = filterAlgorithms(cfg_.ssh.hostKeyAlgorithms, SSH_HOSTKEYS, dropped);
        for (const auto& d : dropped) log(LogLevel::Warn, "Ignoring unsupported host key algorithm " + d);
        return v;
    }();
    if (ctx_.knownHosts) {
        const std::string host = cfg_.connection.logicalHostname.empty() ? cfg_.host : cfg_.connection.logicalHostname;
        std::vector<std::string> known;
        for (const auto& k : ctx_.knownHosts->find(host, cfg_.port))
            for (const auto& a : hostKeyAlgorithmsForKeyType(k.keyType))
                if (std::find(hk.begin(), hk.end(), a) != hk.end() && std::find(known.begin(), known.end(), a) == known.end())
                    known.push_back(a);
        if (!known.empty()) {
            std::vector<std::string> ordered = known;
            for (const auto& a : hk)
                if (std::find(ordered.begin(), ordered.end(), a) == ordered.end()) ordered.push_back(a);
            hk = std::move(ordered);
        }
    }
    if (!hk.empty()) {
        const std::string list = joinList(hk);
        if (!set(SSH_OPTIONS_HOSTKEYS, list.c_str(), "host key algorithms")) return false;
    }
    return true;
}

ConnectResult SshConnector::run(SOCKET sock, int logLevel) {
    ensureInit();
    ConnectResult r;
    SessionPtr session(ssh_new());
    if (!session) {
        closesocket(sock);
        r.error = "Out of memory";
        return r;
    }
    ssh_session s = session.get();
    if (!applyOptions(s, sock, logLevel, r.error)) {
        // libssh owns the fd once SSH_OPTIONS_FD succeeded; ssh_free closes it.
        return r;
    }
    if (ssh_connect(s) != SSH_OK) {
        r.cancelled = cancel_.load();
        r.error = r.cancelled ? "Cancelled" : sshError(s, "Key exchange failed");
        return r;
    }
    const char* banner = ssh_get_serverbanner(s);
    const std::string remote = banner ? banner : "";
    log(LogLevel::Info, "Remote version: " + remote);
    log(LogLevel::Info, "Using SSH protocol version 2");
    const char* kex = ssh_get_kex_algo(s);
    const char* cin = ssh_get_cipher_in(s);
    const char* cout = ssh_get_cipher_out(s);
    const char* hin = ssh_get_hmac_in(s);
    if (kex) log(LogLevel::Info, std::string("Doing ") + kex + " key exchange");
    if (cout) log(LogLevel::Info, std::string("Initialised ") + cout + " outbound encryption");
    if (cin) log(LogLevel::Info, std::string("Initialised ") + cin + " inbound encryption");
    if (hin && cin && std::string(cin).find("gcm") == std::string::npos && std::string(cin).find("poly1305") == std::string::npos)
        log(LogLevel::Info, std::string("Initialised ") + hin + " MAC");
    serverInfo_ = remote + " | " + (cout ? cout : "") + " | " + (kex ? kex : "");

    if (!verifyHostKey(s, r.error, r.cancelled)) return r;

    if (user_.empty()) {
        if (!ctx_.prompter) {
            r.error = "No user name specified";
            return r;
        }
        PromptResponse pr = ctx_.prompter->prompt(prompts::username("Login as", cfg_.host));
        if (pr.cancelled || pr.values.empty() || trim(pr.values[0]).empty()) {
            r.cancelled = true;
            r.error = "Authentication cancelled";
            return r;
        }
        user_ = trim(pr.values[0]);
        ssh_options_set(s, SSH_OPTIONS_USER, user_.c_str());
    }
    log(LogLevel::Info, "Using username \"" + user_ + "\".");

    if (!authenticate(s, r.error, r.cancelled)) return r;
    agentRelay_.reset();  // authentication is over; forwarding uses its own agent connection
    r.session = std::move(session);
    return r;
}

bool SshConnector::verifyHostKey(ssh_session s, std::string& error, bool& cancelled) {
    ssh_key raw = nullptr;
    if (ssh_get_server_publickey(s, &raw) != SSH_OK || !raw) {
        error = "Could not get the server's host key";
        return false;
    }
    KeyPtr key(raw);
    const std::string blob = keyBlobOf(key.get());
    const std::string type = crypto::keyTypeOfBlob(blob);
    const std::string fp = crypto::fingerprintSha256(blob);
    const std::string fpMd5 = crypto::fingerprintMd5(blob);
    log(LogLevel::Info, "Host key fingerprint is: " + type + " " + fp);

    const std::string host = cfg_.connection.logicalHostname.empty() ? cfg_.host : cfg_.connection.logicalHostname;
    const int port = cfg_.port > 0 ? cfg_.port : 22;
    if (!ctx_.knownHosts) return true;
    const HostKeyCheck check = ctx_.knownHosts->check(host, port, type, blob);
    if (check == HostKeyCheck::Match) return true;

    std::vector<std::string> previous;
    for (const auto& k : ctx_.knownHosts->find(host, port))
        if (k.keyType == type || check == HostKeyCheck::Unknown) previous.push_back(k.keyType + " " + k.fingerprint);
    if (check == HostKeyCheck::Mismatch)
        log(LogLevel::Warn, "WARNING - POTENTIAL SECURITY BREACH: the host key does not match the one cached for this host");
    if (!ctx_.prompter) {
        error = "Host key not verified";
        return false;
    }
    PromptResponse pr =
        ctx_.prompter->prompt(prompts::hostkey(host, port, type, fp, fpMd5, check == HostKeyCheck::Mismatch, previous));
    if (pr.cancelled || pr.choice == "reject" || pr.choice.empty()) {
        cancelled = true;
        error = "Host key not accepted";
        log(LogLevel::Info, error);
        return false;
    }
    if (pr.choice == "accept-save") {
        KnownHost kh;
        kh.host = host;
        kh.port = port;
        kh.keyType = type;
        kh.key = crypto::base64Encode(blob);
        kh.fingerprint = fp;
        kh.source = "user";
        ctx_.knownHosts->add(kh);
        log(LogLevel::Info, "Host key stored in the cache");
    } else {
        log(LogLevel::Info, "Host key accepted for this session only");
    }
    return true;
}

std::optional<std::string> SshConnector::takeStoredPassword() {
    if (storedPasswordUsed_) return std::nullopt;
    storedPasswordUsed_ = true;
    if (!ctx_.oneTimePassword.empty() && cfg_.id == ctx_.session.id) return ctx_.oneTimePassword;
    if (ctx_.credentials && !cfg_.id.empty()) return ctx_.credentials->get(Credentials::passwordKey(cfg_.id));
    return std::nullopt;
}

bool SshConnector::authenticate(ssh_session s, std::string& error, bool& cancelled) {
    int rc = ssh_userauth_none(s, nullptr);
    if (rc == SSH_AUTH_ERROR) {
        error = sshError(s, "Authentication failed");
        return false;
    }
    auto showBanner = [&] {
        if (bannerShown_ || !cfg_.ssh.auth.showBanner) return;
        bannerShown_ = true;
        if (char* b = ssh_get_issue_banner(s)) {
            std::string text = b;
            ssh_string_free_char(b);
            if (!text.empty()) local(text.back() == '\n' ? text : text + "\n");
        }
    };
    showBanner();
    if (rc == SSH_AUTH_SUCCESS) {
        log(LogLevel::Info, "Access granted (no authentication required)");
        return true;
    }

    bool agentTried = false, keyTried = false, kbdTried = false, passwordTried = false;
    for (int round = 0; round < 12; ++round) {
        if (cancel_) {
            cancelled = true;
            error = "Cancelled";
            return false;
        }
        const int methods = ssh_userauth_list(s, nullptr);
        rc = SSH_AUTH_DENIED;
        if (!agentTried && cfg_.ssh.auth.tryAgent && (methods & SSH_AUTH_METHOD_PUBLICKEY)) {
            agentTried = true;
            rc = tryAgent(s);
        } else if (!keyTried && !cfg_.ssh.auth.privateKeyPath.empty() && (methods & SSH_AUTH_METHOD_PUBLICKEY)) {
            keyTried = true;
            rc = tryKeyFile(s, cancelled);
        } else if (!kbdTried && cfg_.ssh.auth.keyboardInteractive && (methods & SSH_AUTH_METHOD_INTERACTIVE)) {
            kbdTried = true;
            rc = tryKbdInt(s, cancelled);
        } else if (!passwordTried && cfg_.ssh.auth.password && (methods & SSH_AUTH_METHOD_PASSWORD)) {
            passwordTried = true;
            rc = tryPassword(s, cancelled);
        } else {
            break;
        }
        showBanner();
        if (cancelled) {
            error = "Authentication cancelled";
            return false;
        }
        if (rc == SSH_AUTH_SUCCESS) {
            log(LogLevel::Info, "Access granted");
            return true;
        }
        if (rc == SSH_AUTH_PARTIAL) {
            log(LogLevel::Info, "Further authentication required");
            // A second factor may use a method we already tried.
            agentTried = keyTried = kbdTried = passwordTried = false;
            continue;
        }
        if (rc == SSH_AUTH_ERROR) {
            error = sshError(s, "Authentication failed");
            return false;
        }
    }
    error = "No supported authentication methods left to try";
    local("Server refused our authentication: no supported methods left.\n");
    log(LogLevel::Error, error);
    return false;
}

int SshConnector::tryAgent(ssh_session s) {
    std::string reason;
    auto client = AgentClient::connect(cfg_.ssh.auth.agent, reason);
    if (!client) {
        log(LogLevel::Info, reason);
        return SSH_AUTH_DENIED;
    }
    const std::string name = client->name();
    const int count = client->identityCount();
    log(LogLevel::Info, name + " is running. Requesting keys.");
    if (count <= 0) {
        log(LogLevel::Info, name + (count == 0 ? " has no keys" : " did not answer the key list request"));
        return SSH_AUTH_DENIED;
    }
    log(LogLevel::Info, name + " has " + std::to_string(count) + " SSH-2 key" + (count == 1 ? "" : "s"));
    agentRelay_ = std::make_unique<AgentRelay>(std::move(client));
    std::string err;
    SOCKET sk = agentRelay_->start(err);
    if (sk == INVALID_SOCKET || ssh_set_agent_socket(s, static_cast<socket_t>(sk)) != SSH_OK) {
        if (sk != INVALID_SOCKET) closesocket(sk);
        log(LogLevel::Warn, "Could not connect libssh to the agent: " + (err.empty() ? sshError(s, "error") : err));
        agentRelay_.reset();
        return SSH_AUTH_DENIED;
    }
    const int rc = ssh_userauth_agent(s, nullptr);
    if (rc == SSH_AUTH_SUCCESS) log(LogLevel::Info, "Authenticated using a key from " + name);
    else if (rc == SSH_AUTH_DENIED) log(LogLevel::Info, "Server refused all keys offered by " + name);
    return rc;
}

int SshConnector::tryKeyFile(ssh_session s, bool& cancelled) {
    const std::string& path = cfg_.ssh.auth.privateKeyPath;
    log(LogLevel::Info, "Reading key file \"" + path + "\"");
    std::string error;
    ssh_key key = loadPrivateKey(
        path,
        [&](int attempt) -> std::optional<std::string> {
            if (!ctx_.prompter) return std::nullopt;
            PromptResponse pr = ctx_.prompter->prompt(
                prompts::passphrase("Passphrase for key", path, attempt > 1 ? "Wrong passphrase, try again." : ""));
            if (pr.cancelled || pr.values.empty()) return std::nullopt;
            return pr.values[0];
        },
        error);
    if (!key) {
        if (error == "Cancelled") {
            cancelled = true;
            return SSH_AUTH_DENIED;
        }
        log(LogLevel::Warn, "Unable to use key file \"" + path + "\": " + error);
        local("Unable to use key file \"" + path + "\" (" + error + ")\n");
        return SSH_AUTH_DENIED;
    }
    KeyPtr owned(key);
    log(LogLevel::Info, "Trying public key authentication with \"" + path + "\"");
    const int rc = ssh_userauth_publickey(s, nullptr, owned.get());
    if (rc == SSH_AUTH_DENIED) {
        log(LogLevel::Info, "Server refused our key");
        local("Server refused our key\n");
    }
    return rc;
}

int SshConnector::tryKbdInt(ssh_session s, bool& cancelled) {
    log(LogLevel::Info, "Attempting keyboard-interactive authentication");
    int rc = ssh_userauth_kbdint(s, nullptr, nullptr);
    int rounds = 0;
    while (rc == SSH_AUTH_INFO && rounds++ < 16) {
        const std::string name = ssh_userauth_kbdint_getname(s) ? ssh_userauth_kbdint_getname(s) : "";
        const std::string instruction = ssh_userauth_kbdint_getinstruction(s) ? ssh_userauth_kbdint_getinstruction(s) : "";
        const int n = ssh_userauth_kbdint_getnprompts(s);
        std::vector<prompts::KbdPrompt> items;
        for (int i = 0; i < n; ++i) {
            char echo = 0;
            const char* p = ssh_userauth_kbdint_getprompt(s, static_cast<unsigned>(i), &echo);
            items.push_back({p ? p : "", echo != 0});
        }
        std::vector<std::string> answers;
        bool remember = false;
        if (n == 1 && !items[0].echo) {
            if (auto stored = takeStoredPassword()) answers.push_back(*stored);
        }
        if (n > 0 && answers.empty()) {
            if (!ctx_.prompter) return SSH_AUTH_DENIED;
            PromptResponse pr = ctx_.prompter->prompt(prompts::kbdint(name.empty() ? "Keyboard-interactive authentication" : name,
                                                                      name, instruction, items, !cfg_.id.empty()));
            if (pr.cancelled) {
                cancelled = true;
                return SSH_AUTH_DENIED;
            }
            answers = pr.values;
            remember = pr.remember;
        }
        for (int i = 0; i < n; ++i) {
            const std::string a = i < static_cast<int>(answers.size()) ? answers[static_cast<size_t>(i)] : "";
            ssh_userauth_kbdint_setanswer(s, static_cast<unsigned>(i), a.c_str());
        }
        rc = ssh_userauth_kbdint(s, nullptr, nullptr);
        if (rc == SSH_AUTH_SUCCESS && remember && n == 1 && !items[0].echo && ctx_.credentials && !cfg_.id.empty() &&
            !answers.empty()) {
            ctx_.credentials->set(Credentials::passwordKey(cfg_.id), answers[0]);
            log(LogLevel::Info, "Password saved");
        }
        for (auto& a : answers) wipe(a);
    }
    if (rc == SSH_AUTH_DENIED) local("Access denied\n");
    return rc;
}

int SshConnector::tryPassword(ssh_session s, bool& cancelled) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::optional<std::string> pw = takeStoredPassword();
        bool remember = false;
        if (!pw) {
            if (!ctx_.prompter) return SSH_AUTH_DENIED;
            PromptResponse pr = ctx_.prompter->prompt(
                prompts::password(user_ + "@" + cfg_.host + "'s password", attempt > 0 ? "Access denied" : "", user_, !cfg_.id.empty()));
            if (pr.cancelled || pr.values.empty()) {
                cancelled = true;
                return SSH_AUTH_DENIED;
            }
            pw = pr.values[0];
            remember = pr.remember;
        }
        log(LogLevel::Info, "Sent password");
        const int rc = ssh_userauth_password(s, nullptr, pw->c_str());
        if (rc == SSH_AUTH_SUCCESS && remember && ctx_.credentials && !cfg_.id.empty()) {
            ctx_.credentials->set(Credentials::passwordKey(cfg_.id), *pw);
            log(LogLevel::Info, "Password saved");
        }
        wipe(*pw);
        if (rc != SSH_AUTH_DENIED) return rc;
        log(LogLevel::Info, "Password authentication failed");
        local("Access denied\n");
    }
    return SSH_AUTH_DENIED;
}

}  // namespace sc::ssh
