#pragma once
// PuTTY-style session logging to a file.
//  - LogMode::Printable : decoded text with escape/control sequences stripped (CR/LF/TAB kept)
//  - LogMode::All       : raw bytes exactly as received
//  - LogMode::SshDebug  : raw output + event/debug lines prefixed with "Event Log: "
// Path placeholders: &Y &M &D &T(HHMMSS) &H(host) &P(port) &S(session name, sanitized) && ; relative paths
// resolve against the log directory; parent directories are created.
#include "core/Types.h"

#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace sc {

class Prompter;

class SessionLog {
public:
    SessionLog(const SessionConfig& session, std::filesystem::path logDirectory);
    ~SessionLog();
    /** Opens the file (asks via prompter when ifExists == Ask and the file exists; prompter may be null => append).
     *  Returns false with `error` set on failure. No-op returning true when mode == Off. */
    bool open(Prompter* prompter, std::string& error);
    bool isOpen() const;
    std::filesystem::path path() const;
    void output(std::string_view rawBytes, std::string_view decodedUtf8);
    void event(const std::string& line);
    /** Text shown locally in the terminal (banner, local echo...): logged as printable text in Printable/All modes. */
    void localOutput(std::string_view utf8);
    void close();

    static std::string expandPath(const std::string& tmpl, const SessionConfig& s, const std::tm& when);
    /** Strip ANSI/VT escape sequences and non-printing control characters (keeps \r \n \t). */
    static std::string stripEscapes(std::string_view text);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sc
