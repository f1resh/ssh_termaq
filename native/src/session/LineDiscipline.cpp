// PuTTY-compatible local echo / local line editing (see LineDiscipline.h for the exact rules).
#include "session/LineDiscipline.h"

namespace st {

namespace {

constexpr char ctrl(char c) { return static_cast<char>(c & 0x1F); }
constexpr char kDel = 0x7F;
constexpr char kEsc = 0x1B;

/** Length of the UTF-8 sequence introduced by lead byte `b` (1 for ASCII and for invalid bytes). */
size_t utf8SeqLen(unsigned char b) {
    if (b < 0xC2) return 1;  // ASCII, continuation byte or overlong lead: treat as a unit
    if (b < 0xE0) return 2;
    if (b < 0xF0) return 3;
    if (b < 0xF5) return 4;
    return 1;
}

bool isContinuation(unsigned char b) { return (b & 0xC0) == 0x80; }

char32_t decodeCodePoint(const std::string& cp) {
    const auto b0 = static_cast<unsigned char>(cp[0]);
    if (cp.size() == 1) return b0;
    char32_t v = 0;
    if (cp.size() == 2) v = b0 & 0x1F;
    else if (cp.size() == 3) v = b0 & 0x0F;
    else v = b0 & 0x07;
    for (size_t i = 1; i < cp.size(); ++i) v = (v << 6) | (static_cast<unsigned char>(cp[i]) & 0x3F);
    return v;
}

bool inRange(char32_t c, char32_t lo, char32_t hi) { return c >= lo && c <= hi; }

/** Terminal cells used to display code point `c` as echoed by the line editor. */
int cellWidth(char32_t c) {
    if (c < 0x20 || c == 0x7F) return 2;  // caret notation ^X
    if (c < 0x300) return 1;
    // Combining marks and zero-width characters.
    if (inRange(c, 0x0300, 0x036F) || inRange(c, 0x1AB0, 0x1AFF) || inRange(c, 0x1DC0, 0x1DFF) ||
        inRange(c, 0x20D0, 0x20FF) || inRange(c, 0xFE20, 0xFE2F) || inRange(c, 0x200B, 0x200F) || c == 0xFEFF)
        return 0;
    // East Asian Wide / Fullwidth (main blocks) and emoji.
    if (inRange(c, 0x1100, 0x115F) || inRange(c, 0x2E80, 0x303E) || inRange(c, 0x3041, 0x33FF) ||
        inRange(c, 0x3400, 0x4DBF) || inRange(c, 0x4E00, 0x9FFF) || inRange(c, 0xA000, 0xA4CF) ||
        inRange(c, 0xAC00, 0xD7A3) || inRange(c, 0xF900, 0xFAFF) || inRange(c, 0xFE30, 0xFE4F) ||
        inRange(c, 0xFF00, 0xFF60) || inRange(c, 0xFFE0, 0xFFE6) || inRange(c, 0x1F300, 0x1F64F) ||
        inRange(c, 0x1F900, 0x1F9FF) || inRange(c, 0x20000, 0x2FFFD) || inRange(c, 0x30000, 0x3FFFD))
        return 2;
    return 1;
}

/** Echo form of one code point in the line editor (control characters in caret notation). */
std::string editEcho(const std::string& cp) {
    const auto b = static_cast<unsigned char>(cp[0]);
    if (cp.size() == 1 && (b < 0x20 || b == 0x7F)) return std::string{'^', static_cast<char>(b == 0x7F ? '?' : b + 0x40)};
    return cp;
}

/** Start index of the last code point in `s` (s non-empty). */
size_t lastCodePointStart(const std::string& s) {
    size_t i = s.size() - 1;
    // Step back over at most 3 continuation bytes to the lead byte.
    for (int n = 0; n < 3 && i > 0 && isContinuation(static_cast<unsigned char>(s[i])); ++n) --i;
    if (utf8SeqLen(static_cast<unsigned char>(s[i])) != s.size() - i) return s.size() - 1;  // malformed: one byte
    return i;
}

bool isSpaceCp(const std::string& cp) { return cp.size() == 1 && (cp[0] == ' ' || cp[0] == '\t'); }

std::string backspaces(int cells) {
    std::string out;
    for (int i = 0; i < cells; ++i) out += "\b \b";
    return out;
}

}  // namespace

LineDiscipline::LineDiscipline(TriState echo, TriState edit, LdiscModes transportModes, Sinks sinks)
    : echo_(echo), edit_(edit), modes_(transportModes), sinks_(std::move(sinks)) {}

void LineDiscipline::setTransportModes(LdiscModes modes) {
    std::lock_guard lock(mu_);
    modes_ = modes;
}

void LineDiscipline::setForced(TriState echo, TriState edit) {
    std::lock_guard lock(mu_);
    echo_ = echo;
    edit_ = edit;
}

void LineDiscipline::setNewline(std::string newline) {
    std::lock_guard lock(mu_);
    newline_ = std::move(newline);
}

std::string LineDiscipline::pendingLine() const {
    std::lock_guard lock(mu_);
    return line_;
}

bool LineDiscipline::echoing() const {
    std::lock_guard lock(mu_);
    return echoingLocked();
}

bool LineDiscipline::editing() const {
    std::lock_guard lock(mu_);
    return editingLocked();
}

bool LineDiscipline::echoingLocked() const { return echo_ == TriState::On || (echo_ == TriState::Auto && modes_.echo); }
bool LineDiscipline::editingLocked() const { return edit_ == TriState::On || (edit_ == TriState::Auto && modes_.edit); }

void LineDiscipline::input(const std::string& utf8) {
    std::string remote, term;
    {
        std::lock_guard lock(mu_);
        std::string data = partial_ + utf8;
        partial_.clear();
        size_t i = 0;
        while (i < data.size()) {
            size_t len = utf8SeqLen(static_cast<unsigned char>(data[i]));
            if (i + len > data.size()) {
                // Incomplete multi-byte sequence at the end: keep it for the next call when it looks valid so far.
                bool valid = true;
                for (size_t k = i + 1; k < data.size(); ++k) valid = valid && isContinuation(static_cast<unsigned char>(data[k]));
                if (valid) {
                    partial_ = data.substr(i);
                    break;
                }
                len = 1;
            }
            for (size_t k = 1; k < len; ++k) {
                if (!isContinuation(static_cast<unsigned char>(data[i + k]))) {
                    len = 1;  // malformed: pass the lead byte through on its own
                    break;
                }
            }
            const std::string cp = data.substr(i, len);
            i += len;
            if (editingLocked())
                processEdit(cp, remote, term);
            else
                processPlain(cp, remote, term);
        }
    }
    if (!remote.empty() && sinks_.toRemote) sinks_.toRemote(remote);
    if (!term.empty() && sinks_.toTerminal) sinks_.toTerminal(term);
}

void LineDiscipline::eraseLast(std::string& term, bool echo) {
    if (line_.empty()) return;
    const size_t start = lastCodePointStart(line_);
    const std::string cp = line_.substr(start);
    line_.erase(start);
    if (echo) term += backspaces(cellWidth(decodeCodePoint(cp)));
}

void LineDiscipline::processEdit(const std::string& cp, std::string& remote, std::string& term) {
    const bool echo = echoingLocked();
    const char c = cp.size() == 1 ? cp[0] : '\0';
    escState_ = 0;

    if (quoteNext_) {
        quoteNext_ = false;
        lastWasCr_ = false;
        line_ += cp;
        if (echo) term += editEcho(cp);
        return;
    }
    if (cp.size() == 1 && c == '\n' && lastWasCr_) {
        lastWasCr_ = false;  // CR LF from a paste: the CR already ended the line
        return;
    }
    lastWasCr_ = false;
    if (cp.size() != 1) {
        line_ += cp;
        if (echo) term += cp;
        return;
    }
    switch (c) {
        case ctrl('H'):
        case kDel:
            eraseLast(term, echo);
            return;
        case ctrl('W'): {
            // Delete back to the previous space/non-space boundary (PuTTY: trailing spaces plus one word).
            while (!line_.empty()) {
                const std::string last = line_.substr(lastCodePointStart(line_));
                eraseLast(term, echo);
                if (!line_.empty() && isSpaceCp(line_.substr(lastCodePointStart(line_))) && !isSpaceCp(last)) break;
            }
            return;
        }
        case ctrl('U'):
            while (!line_.empty()) eraseLast(term, echo);
            return;
        case ctrl('C'):
        case ctrl('Z'):
        case ctrl('\\'):
            while (!line_.empty()) eraseLast(term, echo);
            remote += c;
            return;
        case ctrl('R'):
            if (echo) {
                term += "^R\r\n";
                size_t i = 0;
                while (i < line_.size()) {
                    size_t len = utf8SeqLen(static_cast<unsigned char>(line_[i]));
                    if (i + len > line_.size()) len = 1;
                    term += editEcho(line_.substr(i, len));
                    i += len;
                }
            }
            return;
        case ctrl('V'):
            quoteNext_ = true;
            return;
        case ctrl('D'):
            if (line_.empty()) {
                remote += c;
            } else {
                remote += line_;
                line_.clear();
            }
            return;
        case '\r':
        case '\n':
            remote += line_;
            remote += newline_;
            line_.clear();
            if (echo) term += "\r\n";
            lastWasCr_ = c == '\r';
            return;
        default:
            line_ += c;
            if (echo) term += editEcho(cp);
            return;
    }
}

void LineDiscipline::processPlain(const std::string& cp, std::string& remote, std::string& term) {
    const bool echo = echoingLocked();
    quoteNext_ = false;
    if (!line_.empty()) {
        // Leaving edit mode with a partial line: send it and remove its echo (PuTTY behaviour).
        remote += line_;
        while (!line_.empty()) eraseLast(term, echo);
    }
    remote += cp;
    if (!echo) {
        lastWasCr_ = cp == "\r";
        return;
    }
    const auto b = static_cast<unsigned char>(cp[0]);
    const bool wasCr = lastWasCr_;
    lastWasCr_ = false;
    if (escState_ != 0) {
        // Skip escape sequences: ESC x, ESC [ ... final (0x40-0x7E), ESC O x.
        if (escState_ == 1) {
            escState_ = (b == '[') ? 2 : (b == 'O') ? 3 : 0;
        } else if (escState_ == 2) {
            if (b >= 0x40 && b <= 0x7E) escState_ = 0;
        } else {
            escState_ = 0;
        }
        return;
    }
    if (cp.size() > 1) {
        term += cp;
        return;
    }
    if (b == static_cast<unsigned char>(kEsc)) {
        escState_ = 1;
    } else if (b == '\r') {
        term += "\r\n";
        lastWasCr_ = true;
    } else if (b == '\n') {
        if (!wasCr) term += "\r\n";
    } else if (b == '\t' || (b >= 0x20 && b < 0x7F) || b >= 0x80) {
        term += cp;
    }
}

}  // namespace st
