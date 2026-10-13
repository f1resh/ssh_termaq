#pragma once
// PuTTY "ldisc": local echo and local line editing applied to keyboard input before it is sent.
// Works on UTF-8 text from the UI (before charset encoding).
//  - echo on : typed characters are echoed to the terminal (toTerminal)
//  - edit on : input is buffered until Enter; Backspace/DEL erase a char, ^W a word, ^U the line;
//              ^C/^Z/^D etc. are sent immediately; on Enter the buffered line + "\r" is sent
//              (the transport maps CR to its own newline convention).
//  - TriState::Auto uses the transport's modes (defaultLdisc / onLdiscChanged); On/Off force.
#include "core/Types.h"
#include "transport/Transport.h"

#include <functional>
#include <string>

namespace sc {

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

private:
    TriState echo_;
    TriState edit_;
    LdiscModes modes_;
    Sinks sinks_;
    std::string line_;
};

}  // namespace sc
