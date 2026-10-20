#pragma once
// C++ mirror of ui/src/shared/types.ts. JSON field names and enum strings MUST match the TypeScript
// definitions exactly (the UI and the host exchange these as JSON). Member initializers MUST match the
// defaults in ui/src/shared/defaults.ts (createDefaultSession). Missing JSON fields fall back to these
// defaults when parsing (NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT).
#include "core/Json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace st {

/* ------------------------------------------------------------------------ */
/* Enums                                                                     */
/* ------------------------------------------------------------------------ */

enum class Protocol { Ssh, Telnet, Rlogin, Raw, Serial };
NLOHMANN_JSON_SERIALIZE_ENUM(Protocol, {{Protocol::Ssh, "ssh"},
                                        {Protocol::Telnet, "telnet"},
                                        {Protocol::Rlogin, "rlogin"},
                                        {Protocol::Raw, "raw"},
                                        {Protocol::Serial, "serial"}})

enum class TriState { Auto, On, Off };
NLOHMANN_JSON_SERIALIZE_ENUM(TriState, {{TriState::Auto, "auto"}, {TriState::On, "on"}, {TriState::Off, "off"}})

enum class AddressFamily { Auto, IPv4, IPv6 };
NLOHMANN_JSON_SERIALIZE_ENUM(AddressFamily,
                             {{AddressFamily::Auto, "auto"}, {AddressFamily::IPv4, "ipv4"}, {AddressFamily::IPv6, "ipv6"}})

enum class ForwardType { Local, Remote, Dynamic };
NLOHMANN_JSON_SERIALIZE_ENUM(ForwardType,
                             {{ForwardType::Local, "local"}, {ForwardType::Remote, "remote"}, {ForwardType::Dynamic, "dynamic"}})

enum class ProxyType { None, Socks4, Socks5, Http, Telnet, Command, Ssh };
NLOHMANN_JSON_SERIALIZE_ENUM(ProxyType, {{ProxyType::None, "none"},
                                         {ProxyType::Socks4, "socks4"},
                                         {ProxyType::Socks5, "socks5"},
                                         {ProxyType::Http, "http"},
                                         {ProxyType::Telnet, "telnet"},
                                         {ProxyType::Command, "command"},
                                         {ProxyType::Ssh, "ssh"}})

enum class AgentKind { Auto, Pageant, OpenSsh };
NLOHMANN_JSON_SERIALIZE_ENUM(AgentKind, {{AgentKind::Auto, "auto"}, {AgentKind::Pageant, "pageant"}, {AgentKind::OpenSsh, "openssh"}})

enum class Parity { None, Odd, Even, Mark, Space };
NLOHMANN_JSON_SERIALIZE_ENUM(Parity, {{Parity::None, "none"},
                                      {Parity::Odd, "odd"},
                                      {Parity::Even, "even"},
                                      {Parity::Mark, "mark"},
                                      {Parity::Space, "space"}})

enum class FlowControl { None, XonXoff, RtsCts, DsrDtr };
NLOHMANN_JSON_SERIALIZE_ENUM(FlowControl, {{FlowControl::None, "none"},
                                           {FlowControl::XonXoff, "xonxoff"},
                                           {FlowControl::RtsCts, "rtscts"},
                                           {FlowControl::DsrDtr, "dsrdtr"}})

enum class BellMode { None, System, Visual, Sound };
NLOHMANN_JSON_SERIALIZE_ENUM(BellMode, {{BellMode::None, "none"},
                                        {BellMode::System, "system"},
                                        {BellMode::Visual, "visual"},
                                        {BellMode::Sound, "sound"}})

enum class CloseOnExit { Always, Never, Clean };
NLOHMANN_JSON_SERIALIZE_ENUM(CloseOnExit, {{CloseOnExit::Always, "always"}, {CloseOnExit::Never, "never"}, {CloseOnExit::Clean, "clean"}})

enum class FunctionKeyMode { Xterm, EscN, Linux, XtermR6, Vt400, Vt100Plus, Sco };
NLOHMANN_JSON_SERIALIZE_ENUM(FunctionKeyMode, {{FunctionKeyMode::Xterm, "xterm"},
                                               {FunctionKeyMode::EscN, "esc-n"},
                                               {FunctionKeyMode::Linux, "linux"},
                                               {FunctionKeyMode::XtermR6, "xterm-r6"},
                                               {FunctionKeyMode::Vt400, "vt400"},
                                               {FunctionKeyMode::Vt100Plus, "vt100plus"},
                                               {FunctionKeyMode::Sco, "sco"}})

enum class BackspaceKey { Del, CtrlH };
NLOHMANN_JSON_SERIALIZE_ENUM(BackspaceKey, {{BackspaceKey::Del, "del"}, {BackspaceKey::CtrlH, "ctrl-h"}})

enum class HomeEndMode { Xterm, Standard, Rxvt };
NLOHMANN_JSON_SERIALIZE_ENUM(HomeEndMode, {{HomeEndMode::Xterm, "xterm"}, {HomeEndMode::Standard, "standard"}, {HomeEndMode::Rxvt, "rxvt"}})

enum class LogMode { Off, Printable, All, SshDebug };
NLOHMANN_JSON_SERIALIZE_ENUM(LogMode, {{LogMode::Off, "off"},
                                       {LogMode::Printable, "printable"},
                                       {LogMode::All, "all"},
                                       {LogMode::SshDebug, "ssh-debug"}})

enum class LogIfExists { Overwrite, Append, Ask };
NLOHMANN_JSON_SERIALIZE_ENUM(LogIfExists, {{LogIfExists::Overwrite, "overwrite"}, {LogIfExists::Append, "append"}, {LogIfExists::Ask, "ask"}})

enum class ConnStatus { Connecting, Authenticating, Connected, Disconnected, Error };
NLOHMANN_JSON_SERIALIZE_ENUM(ConnStatus, {{ConnStatus::Connecting, "connecting"},
                                          {ConnStatus::Authenticating, "authenticating"},
                                          {ConnStatus::Connected, "connected"},
                                          {ConnStatus::Disconnected, "disconnected"},
                                          {ConnStatus::Error, "error"}})

enum class LogLevel { Info, Warn, Error, Debug };
NLOHMANN_JSON_SERIALIZE_ENUM(LogLevel, {{LogLevel::Info, "info"}, {LogLevel::Warn, "warn"}, {LogLevel::Error, "error"}, {LogLevel::Debug, "debug"}})

enum class FileProtocol { Auto, Sftp, Scp };
NLOHMANN_JSON_SERIALIZE_ENUM(FileProtocol, {{FileProtocol::Auto, "auto"}, {FileProtocol::Sftp, "sftp"}, {FileProtocol::Scp, "scp"}})

enum class HostKeyCheck { Match, Mismatch, Unknown };
NLOHMANN_JSON_SERIALIZE_ENUM(HostKeyCheck, {{HostKeyCheck::Match, "match"}, {HostKeyCheck::Mismatch, "mismatch"}, {HostKeyCheck::Unknown, "unknown"}})

/* ------------------------------------------------------------------------ */
/* Session                                                                   */
/* ------------------------------------------------------------------------ */

struct EnvVar {
    std::string name;
    std::string value;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(EnvVar, name, value)

struct PortForward {
    std::string id;
    ForwardType type = ForwardType::Local;
    bool enabled = true;
    std::string bindAddress;
    int sourcePort = 0;
    std::string destHost = "localhost";
    int destPort = 0;
    AddressFamily addressFamily = AddressFamily::Auto;
    std::string description;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PortForward, id, type, enabled, bindAddress, sourcePort, destHost, destPort,
                                                addressFamily, description)

struct ProxyConfig {
    ProxyType type = ProxyType::None;
    std::string host;
    int port = 1080;
    std::string username;
    std::string password;
    std::string excludeHosts;
    bool proxyLocalhost = false;
    TriState dnsAtProxy = TriState::Auto;
    std::string telnetCommand = "connect %host %port\\n";
    std::string localCommand;
    std::string jumpSessionId;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ProxyConfig, type, host, port, username, password, excludeHosts, proxyLocalhost,
                                                dnsAtProxy, telnetCommand, localCommand, jumpSessionId)

struct SshAuthOptions {
    std::string privateKeyPath;
    bool tryAgent = true;
    AgentKind agent = AgentKind::Auto;
    bool agentForwarding = false;
    bool keyboardInteractive = true;
    bool password = true;
    bool showBanner = true;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SshAuthOptions, privateKeyPath, tryAgent, agent, agentForwarding,
                                                keyboardInteractive, password, showBanner)

struct X11Options {
    bool enabled = false;
    /** "" = automatic: $DISPLAY, else ":0" (ssh/X11Forward.h). */
    std::string display;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(X11Options, enabled, display)

struct SshOptions {
    std::string remoteCommand;
    bool noShell = false;
    bool allocatePty = true;
    bool compression = false;
    std::vector<std::string> kex;
    std::vector<std::string> ciphers;
    std::vector<std::string> hostKeyAlgorithms;
    std::vector<std::string> macs;
    SshAuthOptions auth;
    X11Options x11;
    std::vector<PortForward> tunnels;
    bool localPortsAcceptAll = false;
    bool remotePortsAcceptAll = false;
    std::string sftpStartPath;
    FileProtocol fileProtocol = FileProtocol::Auto;
    std::string localStartPath;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SshOptions, remoteCommand, noShell, allocatePty, compression, kex, ciphers,
                                                hostKeyAlgorithms, macs, auth, x11, tunnels, localPortsAcceptAll,
                                                remotePortsAcceptAll, sftpStartPath, fileProtocol, localStartPath)

struct TelnetOptions {
    bool passive = false;
    bool keyboardSendsSpecials = false;
    bool returnSendsNewline = true;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TelnetOptions, passive, keyboardSendsSpecials, returnSendsNewline)

struct RloginOptions {
    std::string localUsername;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RloginOptions, localUsername)

struct SerialOptions {
    std::string line = "COM1";
    int baudRate = 9600;
    int dataBits = 8;
    double stopBits = 1;  // 1, 1.5 or 2
    Parity parity = Parity::None;
    FlowControl flowControl = FlowControl::XonXoff;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SerialOptions, line, baudRate, dataBits, stopBits, parity, flowControl)

struct TerminalOptions {
    bool autoWrap = true;
    TriState localEcho = TriState::Auto;
    TriState localLineEditing = TriState::Auto;
    bool implicitCrInLf = false;
    bool implicitLfInCr = false;
    std::string answerback = "SSHTermaq";
    BellMode bell = BellMode::Visual;
    std::string bellSoundPath;
    int scrollbackLines = 0;
    bool allowRemoteTitle = true;
    std::string windowTitle;
    CloseOnExit closeOnExit = CloseOnExit::Never;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TerminalOptions, autoWrap, localEcho, localLineEditing, implicitCrInLf,
                                                implicitLfInCr, answerback, bell, bellSoundPath, scrollbackLines,
                                                allowRemoteTitle, windowTitle, closeOnExit)

struct KeyboardOptions {
    BackspaceKey backspace = BackspaceKey::Del;
    HomeEndMode homeEnd = HomeEndMode::Xterm;
    FunctionKeyMode functionKeys = FunctionKeyMode::Xterm;
    bool disableAppCursorKeys = false;
    bool disableAppKeypad = false;
    bool ctrlAltIsAltGr = true;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(KeyboardOptions, backspace, homeEnd, functionKeys, disableAppCursorKeys,
                                                disableAppKeypad, ctrlAltIsAltGr)

struct TranslationOptions {
    std::string charset = "UTF-8";
    bool cjkAmbiguousWide = false;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TranslationOptions, charset, cjkAmbiguousWide)

struct LoggingOptions {
    LogMode mode = LogMode::Off;
    std::string path = "&S-&Y&M&D-&T.log";
    LogIfExists ifExists = LogIfExists::Append;
    bool flush = true;
    bool header = true;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(LoggingOptions, mode, path, ifExists, flush, header)

struct ConnectionOptions {
    int keepaliveSeconds = 30;
    bool tcpNoDelay = true;
    bool tcpKeepalive = false;
    AddressFamily addressFamily = AddressFamily::Auto;
    int connectTimeoutSeconds = 20;
    std::string logicalHostname;
    std::string terminalType = "xterm-256color";
    std::string terminalSpeed = "38400,38400";
    std::vector<EnvVar> environment;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ConnectionOptions, keepaliveSeconds, tcpNoDelay, tcpKeepalive, addressFamily,
                                                connectTimeoutSeconds, logicalHostname, terminalType, terminalSpeed, environment)

/** Colour scheme (UI-owned; C++ only produces it when importing PuTTY colours). */
struct ColorScheme {
    std::string id;
    std::string name;
    std::string variant = "dark";
    std::string foreground, background, cursor, cursorAccent, selectionBackground;
    std::string boldForeground;  // optional ('' => omitted)
    std::string black, red, green, yellow, blue, magenta, cyan, white;
    std::string brightBlack, brightRed, brightGreen, brightYellow, brightBlue, brightMagenta, brightCyan, brightWhite;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ColorScheme, id, name, variant, foreground, background, cursor, cursorAccent,
                                                selectionBackground, boldForeground, black, red, green, yellow, blue, magenta,
                                                cyan, white, brightBlack, brightRed, brightGreen, brightYellow, brightBlue,
                                                brightMagenta, brightCyan, brightWhite)

struct SessionConfig {
    std::string id;
    std::string name;
    std::string folder;
    std::string color;
    std::string notes;
    Protocol protocol = Protocol::Ssh;
    std::string host;
    int port = 22;
    std::string username;
    std::string openAs = "terminal";  // 'terminal' | 'files'
    int64_t createdAt = 0;
    int64_t updatedAt = 0;
    int64_t lastUsedAt = 0;
    ConnectionOptions connection;
    ProxyConfig proxy;
    SshOptions ssh;
    TelnetOptions telnet;
    RloginOptions rlogin;
    SerialOptions serial;
    TerminalOptions terminal;
    KeyboardOptions keyboard;
    /** AppearanceOverrides — UI-owned, passed through untouched (may contain customScheme, fontFamily, ...). */
    Json appearance = Json::object();
    TranslationOptions translation;
    LoggingOptions logging;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SessionConfig, id, name, folder, color, notes, protocol, host, port, username,
                                                openAs, createdAt, updatedAt, lastUsedAt, connection, proxy, ssh, telnet, rlogin, serial,
                                                terminal, keyboard, appearance, translation, logging)

/** Default port for a protocol (ssh 22, telnet 23, rlogin 513, raw/serial 0). */
int defaultPort(Protocol p);
/** New session with defaults, fresh id and timestamps. */
SessionConfig makeDefaultSession();
/** Parse leniently: unknown/missing fields -> defaults. Throws only on wrong JSON types. */
SessionConfig sessionFromJson(const Json& j);
/** "name", else "user@host:port". */
std::string sessionLabel(const SessionConfig& s);
/** Normalize a folder path ("/a//b/" -> "a/b"). */
std::string normalizeFolder(const std::string& folder);
/** Milliseconds since Unix epoch. */
int64_t nowMs();

/* ------------------------------------------------------------------------ */
/* Known hosts                                                               */
/* ------------------------------------------------------------------------ */

struct KnownHost {
    std::string id;
    std::string host;
    int port = 22;
    std::string keyType;
    std::string key;          // base64 wire-format blob
    std::string fingerprint;  // "SHA256:..."
    int64_t addedAt = 0;
    std::string source = "user";  // 'user' | 'putty-import' | 'openssh-import'
    std::string comment;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(KnownHost, id, host, port, keyType, key, fingerprint, addedAt, source, comment)

/* ------------------------------------------------------------------------ */
/* Connections                                                               */
/* ------------------------------------------------------------------------ */

struct ConnOpenRequest {
    std::string connId;
    SessionConfig session;
    int cols = 80;
    int rows = 24;
    int widthPx = 0;
    int heightPx = 0;
    std::string password;  // one-time password; '' = none
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ConnOpenRequest, connId, session, cols, rows, widthPx, heightPx, password)

struct EventLogEntry {
    int64_t time = 0;
    LogLevel level = LogLevel::Info;
    std::string message;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(EventLogEntry, time, level, message)

struct SpecialCommandDef {
    std::string id;
    std::string label;
    std::string group;  // '' => none
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SpecialCommandDef, id, label, group)

/* ------------------------------------------------------------------------ */
/* Prompts                                                                   */
/* ------------------------------------------------------------------------ */

/** A PromptInput (see TS union). Always contains "kind". Build with the helpers in namespace prompts. */
struct PromptInput {
    Json json = Json::object();
};

struct PromptResponse {
    bool cancelled = true;
    std::vector<std::string> values;
    bool remember = false;
    std::string choice;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PromptResponse, cancelled, values, remember, choice)

namespace prompts {
PromptInput username(const std::string& title, const std::string& message = {});
PromptInput password(const std::string& title, const std::string& message, const std::string& username, bool allowSave);
struct KbdPrompt {
    std::string prompt;
    bool echo = false;
};
PromptInput kbdint(const std::string& title, const std::string& name, const std::string& instructions,
                   const std::vector<KbdPrompt>& items, bool allowSave);
PromptInput passphrase(const std::string& title, const std::string& keyPath, const std::string& message = {});
PromptInput hostkey(const std::string& host, int port, const std::string& keyType, const std::string& fingerprint,
                    const std::string& fingerprintMd5, bool mismatch, const std::vector<std::string>& previous);
PromptInput confirm(const std::string& title, const std::string& message, const std::vector<std::string>& buttons,
                    bool danger = false);
}  // namespace prompts

/* ------------------------------------------------------------------------ */
/* Import / export                                                           */
/* ------------------------------------------------------------------------ */

struct ImportCandidate {
    std::string key;
    std::string name;
    std::string source;  // 'putty-registry' | 'putty-regfile' | 'ssh-config' | 'json'
    SessionConfig session;
    std::vector<std::string> warnings;
    bool exists = false;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ImportCandidate, key, name, source, session, warnings, exists)

struct ImportPreview {
    std::string source;
    std::vector<ImportCandidate> candidates;
    std::vector<KnownHost> hostKeys;
    bool hasDefaultSettings = false;  // serialized as presence of "defaultSettings"
    SessionConfig defaultSettings;
    std::vector<std::string> errors;
    std::string origin;
};
Json toJson(const ImportPreview& p);

struct ImportRequest {
    std::vector<ImportCandidate> candidates;
    std::vector<KnownHost> hostKeys;
    std::string conflict = "rename";  // 'skip' | 'overwrite' | 'rename'
    std::string targetFolder;
    bool hasDefaultSettings = false;
    SessionConfig defaultSettings;
};
ImportRequest importRequestFromJson(const Json& j);

struct ImportResult {
    int imported = 0;
    int overwritten = 0;
    int skipped = 0;
    int hostKeysImported = 0;
    std::vector<std::string> errors;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ImportResult, imported, overwritten, skipped, hostKeysImported, errors)

/* ------------------------------------------------------------------------ */
/* SFTP                                                                      */
/* ------------------------------------------------------------------------ */

struct SftpEntry {
    std::string name;
    std::string path;
    std::string type = "file";  // 'file' | 'dir' | 'link' | 'other'
    double size = 0;
    int64_t mtime = 0;  // ms
    uint32_t mode = 0;
    std::string permissions;
    uint32_t uid = 0;
    uint32_t gid = 0;
    bool linkTargetIsDir = false;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SftpEntry, name, path, type, size, mtime, mode, permissions, uid, gid, linkTargetIsDir)

struct RemoteCapabilities {
    bool resume = true;
    bool preciseTimes = true;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RemoteCapabilities, resume, preciseTimes)

struct SftpOpenOptions {
    FileProtocol fileProtocol = FileProtocol::Auto;
    int parallelTransfers = 2;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SftpOpenOptions, fileProtocol, parallelTransfers)

struct TransferProgress {
    std::string transferId;
    std::string sftpId;
    std::string direction;  // 'upload' | 'download'
    std::string name;
    std::string localPath;
    std::string remotePath;
    double transferred = 0;
    double total = 0;
    std::string state = "queued";  // 'queued' | 'running' | 'done' | 'error' | 'cancelled'
    std::string error;
    bool move = false;
    int filesDone = 0;
    int filesTotal = 0;
    std::string currentFile;
    int skipped = 0;
    std::string origin = "transfer";  // 'transfer' | 'edit' | 'sync' | 'drag' | 'open'
    /** Folder transfers: the folder tree is still being listed (filesTotal / total: found so far, currentFile: the
     *  folder being listed); copying starts after it. */
    bool listing = false;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TransferProgress, transferId, sftpId, direction, name, localPath, remotePath,
                                                transferred, total, state, error, move, filesDone, filesTotal, currentFile,
                                                skipped, origin, listing)

struct TransferOptions {
    std::string conflict = "ask";  // 'ask' | 'overwrite' | 'newer' | 'skip' | 'resume' | 'rename'
    bool move = false;
    bool preserveTimestamps = true;
    bool deleteToRecycleBin = true;
    /** Folder uploads leave out .DS_Store and "._*" files (Finder's folder settings, macOS resource-fork copies). */
    bool skipMacFiles = false;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TransferOptions, conflict, move, preserveTimestamps, deleteToRecycleBin,
                                                skipMacFiles)

struct FileStat {
    std::string path;
    double size = 0;
    int64_t mtime = 0;  // ms
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FileStat, path, size, mtime)

struct TransferConflict {
    std::string conflictId;
    std::string transferId;
    std::string sftpId;
    std::string direction;
    std::string name;
    FileStat source;
    FileStat target;
    bool canResume = false;
    std::string reason = "exists";  // 'exists' | 'changed'
    bool multiple = false;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TransferConflict, conflictId, transferId, sftpId, direction, name, source,
                                                target, canResume, reason, multiple)

struct ConflictResolution {
    std::string action = "cancel";  // 'overwrite' | 'newer' | 'skip' | 'resume' | 'rename' | 'cancel'
    std::string newName;
    bool applyToAll = false;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ConflictResolution, action, newName, applyToAll)

struct FindRequest {
    std::string root;
    std::string pattern;
    bool caseSensitive = false;
    bool includeHidden = false;
    int maxResults = 10000;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FindRequest, root, pattern, caseSensitive, includeHidden, maxResults)

struct FindEvent {
    std::string findId;
    std::string sftpId;
    std::vector<SftpEntry> results;
    int scannedDirs = 0;
    std::string currentDir;
    bool done = false;
    bool truncated = false;
    std::string error;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FindEvent, findId, sftpId, results, scannedDirs, currentDir, done, truncated,
                                                error)

struct SyncRequest {
    std::string localDir;
    std::string remoteDir;
    std::string direction = "upload";  // 'upload' | 'download' | 'both'
    bool compareTime = true;
    bool compareSize = true;
    bool deleteExtra = false;
    std::string exclude;
    bool includeHidden = true;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SyncRequest, localDir, remoteDir, direction, compareTime, compareSize,
                                                deleteExtra, exclude, includeHidden)

struct SyncSide {
    double size = 0;
    int64_t mtime = 0;  // ms
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SyncSide, size, mtime)

struct SyncItem {
    std::string relPath;
    bool isDir = false;
    std::string action;  // 'upload' | 'download' | 'deleteLocal' | 'deleteRemote'
    std::string reason;  // 'new' | 'newer' | 'older' | 'size' | 'extra'
    std::optional<SyncSide> local;
    std::optional<SyncSide> remote;
};
void to_json(Json& j, const SyncItem& v);
void from_json(const Json& j, SyncItem& v);

struct EditOptions {
    std::string editor;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(EditOptions, editor)

struct EditEvent {
    std::string editId;
    std::string sftpId;
    std::string remotePath;
    std::string localPath;
    std::string state;  // 'opened' | 'uploading' | 'uploaded' | 'error' | 'closed'
    std::string error;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(EditEvent, editId, sftpId, remotePath, localPath, state, error)

struct DragResult {
    std::string result = "cancelled";  // 'external' | 'internal' | 'cancelled'
    int x = 0;
    int y = 0;
    bool move = false;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DragResult, result, x, y, move)

/* ------------------------------------------------------------------------ */
/* Local files                                                               */
/* ------------------------------------------------------------------------ */

struct LocalEntry {
    std::string name;
    std::string path;
    std::string type = "file";  // 'file' | 'dir' | 'drive' | 'place'
    double size = 0;
    int64_t mtime = 0;  // ms
    bool hidden = false;
    bool system = false;
    bool readonly = false;
    bool link = false;
    // Drives and places only; omitted from JSON when empty / negative.
    std::string label;
    double freeBytes = -1;
    double totalBytes = -1;
    std::string icon;
};
void to_json(Json& j, const LocalEntry& v);

/* ------------------------------------------------------------------------ */
/* Keys                                                                      */
/* ------------------------------------------------------------------------ */

struct KeygenRequest {
    std::string type = "ed25519";  // 'ed25519' | 'rsa' | 'ecdsa'
    int bits = 0;
    std::string comment;
    std::string passphrase;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(KeygenRequest, type, bits, comment, passphrase)

struct KeygenResult {
    std::string privateKeyOpenSsh;
    std::string privateKeyPpk;
    std::string publicKeyOpenSsh;
    std::string fingerprint;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(KeygenResult, privateKeyOpenSsh, privateKeyPpk, publicKeyOpenSsh, fingerprint)

struct KeyInfo {
    std::string path;
    std::string format = "unknown";  // 'openssh' | 'pem' | 'ppk2' | 'ppk3' | 'unknown'
    std::string keyType;
    std::string comment;
    bool encrypted = false;
    std::string fingerprint;
    std::string publicKeyOpenSsh;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(KeyInfo, path, format, keyType, comment, encrypted, fingerprint, publicKeyOpenSsh)

/* ------------------------------------------------------------------------ */
/* App                                                                       */
/* ------------------------------------------------------------------------ */

struct OpenRequest {
    SessionConfig session;
    std::string savedSessionId;  // '' => none
    std::string password;        // '' => none
    std::string view;            // '' (session default) | 'terminal' | 'files'
    std::string remotePath;      // '' => none
};
Json toJson(const OpenRequest& r);

struct SerialPortInfo {
    std::string path;
    std::string friendlyName;
    std::string manufacturer;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SerialPortInfo, path, friendlyName, manufacturer)

struct FileDialogFilter {
    std::string name;
    std::vector<std::string> extensions;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FileDialogFilter, name, extensions)

struct FileDialogOptions {
    std::string title;
    std::string defaultPath;
    std::vector<FileDialogFilter> filters;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FileDialogOptions, title, defaultPath, filters)

}  // namespace st
