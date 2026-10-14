#pragma once
// PuTTY "ldisc": local echo and local line editing applied to keyboard input before it is sent.
// Works on UTF-8 text from the UI (before charset encoding).
//  - echo on : typed characters are echoed to the terminal (toTerminal)
//  - edit on : input is buffered until Enter; Backspace/DEL erase a char, ^W a word, ^U the line;
//              ^C/^Z/^D etc. are sent immediately; on Enter the buffered line + "\r" is sent
//              (the transport maps CR to its own newline convention).
//  - TriState::Auto uses the transport's modes (defaultLdisc / onLdiscChanged); On/Off force.
//
// Details (PuTTY ldisc.c semantics, adapted to UTF-8 input):
//  edit mode : DEL/^H erase one code point (echo "\b \b" per terminal cell: control chars shown as ^X take 2,
//              East Asian wide chars 2), ^W erases back to the previous word boundary, ^U the whole line,
//              ^V quotes the next character, ^R redraws ("^R\r\n" + line), ^C / ^Z / ^\ discard the line and
//              are sent immediately, ^D sends the pending line without newline (or ^D itself on an empty line),
//              CR (or a lone LF; LF directly after CR is swallowed) sends line + newline() and echoes CRLF.
//              Other control characters are buffered literally and echoed in caret notation (^[).
//  plain mode: input is sent unchanged; with echo on, printable characters and TAB are echoed, CR / lone LF
//              echo as CRLF, escape sequences (cursor keys etc.) and other control characters are not echoed.
//              Text still buffered from edit mode is flushed (sent) first.
// Thread-safe: input()/setTransportModes() may be called from different threads; sinks are invoked outside the
// internal lock, from the thread calling input().
#include "core/Types.h"
#include "transport/Transport.h"

#include <functional>
#include <mutex>
#include <string>

namespace st {

class LineDiscipline {
public:
    struct Sinks {
        std::function<void(const std::string& utf8)> toRemote;
        std::function<void(const std::string& utf8)> toTerminal;
    };
    LineDiscipline(TriState echo, TriState edit, LdiscModes transportModes, Sinks sinks);

    void setTransportModes(LdiscModes modes);
    void input(const std::string& utf8);
    bool echoing() const;
    bool editing() const;

    /* ---- additions (net module) -------------------------------------------------------------------- */

    /** Line terminator sent on Enter in edit mode (default "\r"; PuTTY uses "\r\n" for the Raw protocol). */
    void setNewline(std::string newline);
    /** Forced modes (session settings changed at runtime). */
    void setForced(TriState echo, TriState edit);
    /** Text currently buffered by local line editing (tests / diagnostics). */
    std::string pendingLine() const;

private:
    bool echoingLocked() const;
    bool editingLocked() const;
    void processEdit(const std::string& cp, std::string& remote, std::string& term);
    void processPlain(const std::string& cp, std::string& remote, std::string& term);
    void eraseLast(std::string& term, bool echo);

    TriState echo_;
    TriState edit_;
    LdiscModes modes_;
    Sinks sinks_;
    std::string line_;

    mutable std::mutex mu_;
    std::string newline_ = "\r";
    bool quoteNext_ = false;
    bool lastWasCr_ = false;
    /** Escape-sequence skipper for plain-mode echo: 0 none, 1 after ESC, 2 in CSI, 3 after SS3 introducer. */
    int escState_ = 0;
    /** Incomplete UTF-8 sequence carried over between input() calls. */
    std::string partial_;
};

}  // namespace st
