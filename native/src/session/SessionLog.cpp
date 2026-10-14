// PuTTY-style session logging (owner: session module).
//
// File handling: the log is opened with _wfsopen(..., _SH_DENYWR) so other programs can read (tail) it while
// the session runs, but two sessions cannot interleave writes into the same file (the second one fails to open
// and the session continues without a log, like PuTTY does when the file cannot be created).
// All public methods are thread-safe (one internal mutex): output() runs on the transport thread while event()
// may be called from any thread.
#include "session/SessionLog.h"

#include "core/Strings.h"
#include "session/Prompter.h"

#include <share.h>

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <mutex>
#include <system_error>

namespace st {

namespace {

struct FileCloser {
    void operator()(std::FILE* f) const noexcept {
        if (f) std::fclose(f);
    }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

/** Characters that are not allowed in Windows file names (and control characters) become '_'. */
std::string sanitizeFileComponent(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' ||
            c == '*')
            out += '_';
        else
            out += c;
    }
    // Trailing dots/spaces are silently dropped by Win32; make them visible instead.
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.back() = '_';
    return out;
}

std::string twoDigits(int v) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02d", v);
    return buf;
}

std::tm localNow() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    return tm;
}

/**
 * Stateful escape-sequence stripper (survives chunk boundaries). Works on UTF-8 text: C1 controls appear as
 * C2 80..C2 9F and are recognised (CSI U+009B, OSC U+009D, DCS U+0090, SOS/PM/APC, ST U+009C).
 */
class EscapeStripper {
public:
    std::string feed(std::string_view in) {
        std::string out;
        out.reserve(in.size());
        for (size_t i = 0; i < in.size(); ++i) step(static_cast<unsigned char>(in[i]), out);
        return out;
    }

private:
    enum class State { Ground, Escape, EscIntermediate, Csi, String, StringEsc };

    static constexpr unsigned char ESC = 0x1B, BEL = 0x07, CAN = 0x18, SUB = 0x1A;

    /** C1 control (0x80..0x9F) arrived (from a UTF-8 C2 xx pair). */
    void c1(unsigned char c, std::string& out) {
        switch (c) {
            case 0x9B: state_ = State::Csi; break;
            case 0x9D:  // OSC
            case 0x90:  // DCS
            case 0x98:  // SOS
            case 0x9E:  // PM
            case 0x9F:  // APC
                state_ = State::String;
                break;
            case 0x9C: state_ = State::Ground; break;  // ST
            default:
                // Other C1 controls (IND, NEL, HTS, ...) are not printable; NEL is a line break.
                if (c == 0x85) out += "\r\n";
                if (state_ != State::String) state_ = State::Ground;
                break;
        }
    }

    void step(unsigned char b, std::string& out) {
        // UTF-8 encoded C1 controls: hold 0xC2 until we see the next byte.
        if (pendingC2_) {
            pendingC2_ = false;
            if (b >= 0x80 && b <= 0x9F) {
                c1(b, out);
                return;
            }
            if (state_ == State::Ground) out += static_cast<char>(0xC2);
            // fall through and process b normally
        }
        if (b == 0xC2) {
            pendingC2_ = true;
            return;
        }
        switch (state_) {
            case State::Ground:
                if (b == ESC) {
                    state_ = State::Escape;
                } else if (b == '\r' || b == '\n' || b == '\t' || (b >= 0x20 && b != 0x7F)) {
                    out += static_cast<char>(b);
                }
                // other C0 controls and DEL are dropped
                return;
            case State::Escape:
                if (b == '[') {
                    state_ = State::Csi;
                } else if (b == ']' || b == 'P' || b == 'X' || b == '^' || b == '_') {
                    state_ = State::String;
                } else if (b >= 0x20 && b <= 0x2F) {
                    state_ = State::EscIntermediate;
                } else if (b == ESC) {
                    // ESC ESC: stay
                } else if (b == CAN || b == SUB) {
                    state_ = State::Ground;
                } else if (b < 0x20) {
                    // C0 controls inside an escape are executed by terminals; drop them, keep waiting.
                } else {
                    state_ = State::Ground;  // final byte (0x30..0x7E) or garbage
                    if (b >= 0x80) step(b, out);
                }
                return;
            case State::EscIntermediate:
                if (b >= 0x20 && b <= 0x2F) return;
                if (b == ESC) {
                    state_ = State::Escape;
                    return;
                }
                if (b < 0x20 && b != CAN && b != SUB) return;
                state_ = State::Ground;
                if (b >= 0x80) step(b, out);
                return;
            case State::Csi:
                if (b >= 0x20 && b <= 0x3F) return;  // parameters / intermediates
                if (b >= 0x40 && b <= 0x7E) {
                    state_ = State::Ground;  // final byte
                    return;
                }
                if (b == ESC) {
                    state_ = State::Escape;
                    return;
                }
                if (b == CAN || b == SUB) {
                    state_ = State::Ground;
                    return;
                }
                if (b < 0x20) return;  // executed C0 control: drop
                state_ = State::Ground;  // DEL / non-ASCII aborts the sequence
                if (b >= 0x80) step(b, out);
                return;
            case State::String:
                if (b == BEL || b == CAN || b == SUB) {
                    state_ = State::Ground;
                } else if (b == ESC) {
                    state_ = State::StringEsc;
                }
                return;
            case State::StringEsc:
                if (b == '\\') {
                    state_ = State::Ground;  // ST
                    return;
                }
                // The string was terminated by a new escape sequence: process b as its first byte.
                state_ = State::Escape;
                step(b, out);
                return;
        }
    }

    State state_ = State::Ground;
    bool pendingC2_ = false;
};

}  // namespace

/* ------------------------------------------------------------------------------------------------------------ */

struct SessionLog::Impl {
    SessionConfig session;
    std::filesystem::path logDirectory;
    mutable std::mutex mutex;
    FilePtr file;
    std::filesystem::path path;
    EscapeStripper stripper;

    void writeLocked(std::string_view data) {
        if (!file || data.empty()) return;
        if (std::fwrite(data.data(), 1, data.size(), file.get()) != data.size()) {
            // Disk full / file vanished: stop logging rather than failing on every write.
            file.reset();
            return;
        }
        if (session.logging.flush) std::fflush(file.get());
    }
};

SessionLog::SessionLog(const SessionConfig& session, std::filesystem::path logDirectory) : impl_(std::make_unique<Impl>()) {
    impl_->session = session;
    impl_->logDirectory = std::move(logDirectory);
}

SessionLog::~SessionLog() { close(); }

bool SessionLog::open(Prompter* prompter, std::string& error) {
    const LoggingOptions& opt = impl_->session.logging;
    if (opt.mode == LogMode::Off) return true;

    const std::tm now = localNow();
    const std::string tmpl = trim(opt.path).empty() ? std::string(LoggingOptions{}.path) : opt.path;
    std::filesystem::path p(widen(expandPath(tmpl, impl_->session, now)));
    if (p.is_relative()) p = impl_->logDirectory / p;
    p = p.lexically_normal();

    std::error_code ec;
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
        if (ec) {
            error = "Cannot create log directory \"" + narrow(p.parent_path().wstring()) + "\": " + ec.message();
            return false;
        }
    }

    bool append = true;
    if (std::filesystem::exists(p, ec)) {
        switch (opt.ifExists) {
            case LogIfExists::Overwrite: append = false; break;
            case LogIfExists::Append: append = true; break;
            case LogIfExists::Ask: {
                if (!prompter) break;  // nobody to ask: append (PuTTY's safe choice)
                const std::string msg = "The session log file \"" + narrow(p.wstring()) +
                                        "\" already exists. You can overwrite it with a new session log, append your "
                                        "session log to the end of the old file, or disable session logging for this "
                                        "session.";
                const PromptResponse r =
                    prompter->prompt(prompts::confirm("Session log", msg, {"Append", "Overwrite", "Disable logging"}));
                if (r.cancelled || r.choice == "Disable logging" || r.choice.empty()) return true;  // logging disabled
                append = r.choice != "Overwrite";
                break;
            }
        }
    }

    std::lock_guard lock(impl_->mutex);
    impl_->file.reset(_wfsopen(p.c_str(), append ? L"ab" : L"wb", _SH_DENYWR));
    if (!impl_->file) {
        const int err = errno;
        error = "Cannot open log file \"" + narrow(p.wstring()) + "\": " +
                (err == EACCES ? std::string("access denied or the file is in use") : std::generic_category().message(err));
        return false;
    }
    impl_->path = p;
    if (opt.header) {
        char stamp[64];
        std::snprintf(stamp, sizeof stamp, "%04d.%02d.%02d %02d:%02d:%02d", now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
                      now.tm_hour, now.tm_min, now.tm_sec);
        impl_->writeLocked(std::string("=~=~=~=~=~=~=~=~=~=~=~= SSHTerm log ") + stamp +
                           " =~=~=~=~=~=~=~=~=~=~=~=\r\n");
    }
    return true;
}

bool SessionLog::isOpen() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->file != nullptr;
}

std::filesystem::path SessionLog::path() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->path;
}

void SessionLog::output(std::string_view rawBytes, std::string_view decodedUtf8) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->file) return;
    switch (impl_->session.logging.mode) {
        case LogMode::Off: return;
        case LogMode::Printable: impl_->writeLocked(impl_->stripper.feed(decodedUtf8)); return;
        case LogMode::All:
        case LogMode::SshDebug: impl_->writeLocked(rawBytes); return;
    }
}

void SessionLog::localOutput(std::string_view utf8) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->file) return;
    const LogMode mode = impl_->session.logging.mode;
    if (mode != LogMode::Printable && mode != LogMode::All) return;
    impl_->writeLocked(stripEscapes(utf8));
}

void SessionLog::event(const std::string& line) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->file || impl_->session.logging.mode != LogMode::SshDebug) return;
    std::string text = "Event Log: ";
    text += stripEscapes(line);
    text += "\r\n";
    impl_->writeLocked(text);
}

void SessionLog::close() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->file) std::fflush(impl_->file.get());
    impl_->file.reset();
}

std::string SessionLog::expandPath(const std::string& tmpl, const SessionConfig& s, const std::tm& when) {
    std::string out;
    out.reserve(tmpl.size() + 32);
    for (size_t i = 0; i < tmpl.size(); ++i) {
        const char c = tmpl[i];
        if (c != '&' || i + 1 >= tmpl.size()) {
            out += c;
            continue;
        }
        const char k = tmpl[++i];
        switch (k) {
            case 'Y':
            case 'y': out += std::to_string(when.tm_year + 1900); break;
            case 'M':
            case 'm': out += twoDigits(when.tm_mon + 1); break;
            case 'D':
            case 'd': out += twoDigits(when.tm_mday); break;
            case 'T':
            case 't': out += twoDigits(when.tm_hour) + twoDigits(when.tm_min) + twoDigits(when.tm_sec); break;
            case 'H':
            case 'h': {
                const std::string host = s.protocol == Protocol::Serial ? s.serial.line : s.host;
                out += sanitizeFileComponent(host);
                break;
            }
            case 'P':
            case 'p': out += std::to_string(s.protocol == Protocol::Serial ? s.serial.baudRate : s.port); break;
            case 'S':
            case 's': {
                std::string name = s.name;
                if (trim(name).empty()) name = s.protocol == Protocol::Serial ? s.serial.line : s.host;
                out += sanitizeFileComponent(name);
                break;
            }
            case '&': out += '&'; break;
            default:  // unknown placeholder: kept verbatim (PuTTY behaviour)
                out += '&';
                out += k;
                break;
        }
    }
    return out;
}

std::string SessionLog::stripEscapes(std::string_view text) {
    EscapeStripper s;
    std::string out = s.feed(text);
    return out;
}

}  // namespace st
