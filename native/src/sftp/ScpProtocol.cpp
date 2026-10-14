#include "sftp/ScpProtocol.h"

#include <array>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <vector>

namespace sc::scp {

namespace {

bool parseU64(std::string_view s, uint64_t& out) {
    if (s.empty()) return false;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && p == s.data() + s.size();
}

bool parseI64(std::string_view s, int64_t& out) {
    if (s.empty()) return false;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && p == s.data() + s.size();
}

bool parseOctal(std::string_view s, uint32_t& out) {
    if (s.empty()) return false;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out, 8);
    return ec == std::errc() && p == s.data() + s.size();
}

bool isDigits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

int monthIndex(std::string_view s) {
    static const std::array<std::string_view, 12> kMonths = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (size_t i = 0; i < kMonths.size(); ++i)
        if (s == kMonths[i]) return static_cast<int>(i) + 1;
    return 0;
}

bool isWeekday(std::string_view s) {
    static const std::array<std::string_view, 7> kDays = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    for (auto d : kDays)
        if (s == d) return true;
    return false;
}

/** "HH:MM" or "HH:MM:SS[.frac]" */
bool parseClock(std::string_view s, int& h, int& m, int& sec, bool& hasSeconds) {
    h = m = sec = 0;
    hasSeconds = false;
    const size_t c1 = s.find(':');
    if (c1 == std::string_view::npos) return false;
    const size_t c2 = s.find(':', c1 + 1);
    const std::string_view hs = s.substr(0, c1);
    const std::string_view ms = s.substr(c1 + 1, c2 == std::string_view::npos ? std::string_view::npos : c2 - c1 - 1);
    if (!isDigits(hs) || !isDigits(ms)) return false;
    h = std::stoi(std::string(hs));
    m = std::stoi(std::string(ms));
    if (c2 != std::string_view::npos) {
        std::string_view ss = s.substr(c2 + 1);
        const size_t dot = ss.find('.');
        if (dot != std::string_view::npos) ss = ss.substr(0, dot);
        if (!isDigits(ss)) return false;
        sec = std::stoi(std::string(ss));
        hasSeconds = true;
    }
    return h < 24 && m < 60 && sec < 61;
}

/** "YYYY-MM-DD" */
bool parseIsoDate(std::string_view s, int& y, int& mo, int& d) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    if (!isDigits(s.substr(0, 4)) || !isDigits(s.substr(5, 2)) || !isDigits(s.substr(8, 2))) return false;
    y = std::stoi(std::string(s.substr(0, 4)));
    mo = std::stoi(std::string(s.substr(5, 2)));
    d = std::stoi(std::string(s.substr(8, 2)));
    return mo >= 1 && mo <= 12 && d >= 1 && d <= 31;
}

bool isTzToken(std::string_view s) {
    return s.size() == 5 && (s[0] == '+' || s[0] == '-') && isDigits(s.substr(1));
}

struct Token {
    std::string_view text;
    size_t pos = 0;
};

std::vector<Token> tokenize(std::string_view line) {
    std::vector<Token> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size()) break;
        const size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
        out.push_back(Token{line.substr(start, i - start), start});
    }
    return out;
}

/** Parsed date at token index i; `next` = first token after it. */
struct DateMatch {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    bool hasYear = false, precise = false, hasTz = false;
    int tz = 0;
    size_t next = 0;
};

bool matchDate(const std::vector<Token>& t, size_t i, DateMatch& d) {
    const size_t n = t.size();
    auto at = [&](size_t k) { return k < n ? t[k].text : std::string_view(); };
    bool sec = false;
    // GNU --full-time / long-iso: 2024-01-31 12:34:56.123456789 +0100
    if (parseIsoDate(at(i), d.year, d.month, d.day)) {
        if (!parseClock(at(i + 1), d.hour, d.minute, d.second, sec)) return false;
        d.hasYear = true;
        d.precise = sec;
        d.next = i + 2;
        if (isTzToken(at(i + 2))) {
            d.hasTz = true;
            d.tz = parseTzOffset(at(i + 2));
            d.next = i + 3;
        }
        return d.next < n;
    }
    // BusyBox -e: Wed Jan 31 12:34:56 2024
    if (isWeekday(at(i)) && monthIndex(at(i + 1)) && isDigits(at(i + 2)) &&
        parseClock(at(i + 3), d.hour, d.minute, d.second, sec) && isDigits(at(i + 4)) && at(i + 4).size() == 4) {
        d.month = monthIndex(at(i + 1));
        d.day = std::stoi(std::string(at(i + 2)));
        d.year = std::stoi(std::string(at(i + 4)));
        d.hasYear = true;
        d.precise = sec;
        d.next = i + 5;
        return d.next < n;
    }
    if (!monthIndex(at(i)) || !isDigits(at(i + 1))) return false;
    d.month = monthIndex(at(i));
    d.day = std::stoi(std::string(at(i + 1)));
    // BSD -T: Jan 31 12:34:56 2024
    if (parseClock(at(i + 2), d.hour, d.minute, d.second, sec) && sec && isDigits(at(i + 3)) && at(i + 3).size() == 4) {
        d.year = std::stoi(std::string(at(i + 3)));
        d.hasYear = true;
        d.precise = true;
        d.next = i + 4;
        return d.next < n;
    }
    // plain: Jan 31 12:34  |  Jan 31  2024
    if (parseClock(at(i + 2), d.hour, d.minute, d.second, sec)) {
        d.hasYear = false;
        d.next = i + 3;
        return d.next < n;
    }
    if (isDigits(at(i + 2)) && at(i + 2).size() == 4) {
        d.year = std::stoi(std::string(at(i + 2)));
        d.hasYear = true;
        d.next = i + 3;
        return d.next < n;
    }
    return false;
}

bool isPermissions(std::string_view s) {
    if (s.size() < 10) return false;
    if (std::string_view("-dlcbpsD").find(s[0]) == std::string_view::npos) return false;
    for (size_t i = 1; i < 10; ++i)
        if (std::string_view("rwxsStTl-").find(s[i]) == std::string_view::npos) return false;
    return true;
}

}  // namespace

/* ---- records -------------------------------------------------------------- */

std::optional<Record> parseRecord(std::string_view line, std::string& error) {
    error.clear();
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty()) {
        error = "empty SCP record";
        return std::nullopt;
    }
    Record r;
    r.type = line[0];
    const std::string_view body = line.substr(1);
    switch (r.type) {
        case 'E':
            return r;
        case 'T': {
            // T<mtime> <mtime-usec> <atime> <atime-usec>
            std::vector<std::string_view> parts;
            size_t i = 0;
            while (i <= body.size()) {
                const size_t sp = body.find(' ', i);
                parts.push_back(body.substr(i, sp == std::string_view::npos ? std::string_view::npos : sp - i));
                if (sp == std::string_view::npos) break;
                i = sp + 1;
            }
            if (parts.size() != 4 || !parseI64(parts[0], r.mtime) || !parseI64(parts[2], r.atime)) {
                error = "malformed SCP time record";
                return std::nullopt;
            }
            return r;
        }
        case 'C':
        case 'D': {
            const size_t sp1 = body.find(' ');
            const size_t sp2 = sp1 == std::string_view::npos ? sp1 : body.find(' ', sp1 + 1);
            if (sp2 == std::string_view::npos || !parseOctal(body.substr(0, sp1), r.mode) ||
                !parseU64(body.substr(sp1 + 1, sp2 - sp1 - 1), r.size)) {
                error = "malformed SCP file record";
                return std::nullopt;
            }
            r.name = std::string(body.substr(sp2 + 1));
            if (r.name.empty() || r.name == "." || r.name == ".." || r.name.find('/') != std::string::npos) {
                error = "unsafe file name in SCP record";
                return std::nullopt;
            }
            return r;
        }
        default:
            error = "unexpected SCP record";
            return std::nullopt;
    }
}

std::string fileRecord(uint32_t mode, uint64_t size, std::string_view name) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "C%04o %llu ", mode & 07777, static_cast<unsigned long long>(size));
    return std::string(buf) + std::string(name) + "\n";
}

std::string timeRecord(int64_t mtime, int64_t atime) {
    return "T" + std::to_string(mtime) + " 0 " + std::to_string(atime) + " 0\n";
}

std::string shellQuote(std::string_view s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += '\'';
    return out;
}

/* ---- ls -l ----------------------------------------------------------------- */

uint32_t modeFromPermissions(std::string_view p) {
    if (p.size() < 10) return 0;
    uint32_t mode = 0;
    switch (p[0]) {
        case 'd': mode = 0040000; break;
        case 'l': mode = 0120000; break;
        case 'c': mode = 0020000; break;
        case 'b': mode = 0060000; break;
        case 'p': mode = 0010000; break;
        case 's': mode = 0140000; break;
        default: mode = 0100000; break;
    }
    static const uint32_t kBits[9] = {0400, 0200, 0100, 040, 020, 010, 04, 02, 01};
    for (int i = 0; i < 9; ++i) {
        const char c = p[1 + i];
        if (c == 'r' || c == 'w' || c == 'x') mode |= kBits[i];
        if (i == 2 && (c == 's' || c == 'S')) mode |= 04000 | (c == 's' ? 0100 : 0);
        if (i == 5 && (c == 's' || c == 'S' || c == 'l')) mode |= 02000 | (c == 's' ? 010 : 0);
        if (i == 8 && (c == 't' || c == 'T')) mode |= 01000 | (c == 't' ? 01 : 0);
    }
    return mode;
}

int parseTzOffset(std::string_view s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.remove_suffix(1);
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    if (!isTzToken(s)) return 0;
    const int h = std::stoi(std::string(s.substr(1, 2)));
    const int m = std::stoi(std::string(s.substr(3, 2)));
    const int v = h * 3600 + m * 60;
    return s[0] == '-' ? -v : v;
}

int64_t civilToUnix(int y, int m, int d, int hh, int mm, int ss) {
    // Howard Hinnant's days_from_civil.
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(d) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + static_cast<int64_t>(doe) - 719468;
    return days * 86400 + hh * 3600 + mm * 60 + ss;
}

std::optional<LsEntry> parseLsLine(std::string_view line, int tzOffsetSeconds, int64_t nowUtc) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    const auto t = tokenize(line);
    if (t.size() < 6 || !isPermissions(t[0].text)) return std::nullopt;  // also skips "total 12"

    // The date is found by pattern (owner / group columns vary between ls flavours); the size precedes it.
    DateMatch d;
    size_t dateAt = 0;
    for (size_t i = 3; i < t.size() && i <= 8; ++i) {
        DateMatch m;
        if (matchDate(t, i, m)) {
            d = m;
            dateAt = i;
            break;
        }
    }
    if (!dateAt) return std::nullopt;

    LsEntry e;
    const std::string_view perms = t[0].text;
    e.type = perms[0] == 'D' ? 'd' : perms[0];
    e.mode = modeFromPermissions(perms);
    const std::string_view sizeTok = t[dateAt - 1].text;
    const bool device = dateAt >= 2 && !t[dateAt - 2].text.empty() && t[dateAt - 2].text.back() == ',';
    if (!device) {
        if (!parseU64(sizeTok, e.size)) return std::nullopt;
    }
    // owner / group are the tokens between the link count and the size
    const size_t sizeIdx = device ? dateAt - 2 : dateAt - 1;
    if (sizeIdx >= 3) {
        e.owner = std::string(t[2].text);
        if (sizeIdx >= 4) e.group = std::string(t[3].text);
    }

    if (!d.hasYear) {
        // "Mon dd hh:mm": within the last ~6 months. Take the server's current year, or the previous one when the
        // date would lie in the future.
        const int64_t localNow = nowUtc + tzOffsetSeconds;
        int64_t days = localNow / 86400;
        // civil from days (inverse of civilToUnix)
        days += 719468;
        const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
        const unsigned doe = static_cast<unsigned>(days - era * 146097);
        const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        int year = static_cast<int>(yoe) + static_cast<int>(era * 400);
        const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const unsigned mp = (5 * doy + 2) / 153;
        const int month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
        if (month <= 2) ++year;
        d.year = year;
        if (civilToUnix(d.year, d.month, d.day, d.hour, d.minute, 0) > localNow + 86400) --d.year;
    }
    const int64_t local = civilToUnix(d.year, d.month, d.day, d.hour, d.minute, d.second);
    e.mtime = local - (d.hasTz ? d.tz : tzOffsetSeconds);
    e.preciseTime = d.precise;

    std::string_view name = line.substr(t[d.next].pos);
    if (e.type == 'l') {
        const size_t arrow = name.find(" -> ");
        if (arrow != std::string_view::npos) {
            e.linkTarget = std::string(name.substr(arrow + 4));
            name = name.substr(0, arrow);
        }
    }
    e.name = std::string(name);
    if (e.name.empty()) return std::nullopt;
    return e;
}

}  // namespace sc::scp
