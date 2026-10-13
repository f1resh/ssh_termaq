#include "core/Log.h"

#include "core/Strings.h"
#include "core/Win.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>

namespace sc::applog {

namespace {
std::mutex g_mutex;
std::ofstream g_file;

void write(const char* level, const std::string& msg) {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    char ts[32];
    std::snprintf(ts, sizeof ts, "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec);
    std::string line = std::string(ts) + " [" + level + "] " + msg + "\n";
    std::lock_guard lock(g_mutex);
    OutputDebugStringW(widen(line).c_str());
    if (g_file.is_open()) {
        g_file << line;
        g_file.flush();
    }
}
}  // namespace

void init(const std::filesystem::path& file) {
    std::lock_guard lock(g_mutex);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    // Keep the log small: truncate when > 2 MB.
    auto mode = std::ios::app;
    if (std::filesystem::exists(file, ec) && std::filesystem::file_size(file, ec) > 2 * 1024 * 1024) mode = std::ios::trunc;
    g_file.open(file, std::ios::out | mode);
}

void info(const std::string& msg) { write("INFO", msg); }
void warn(const std::string& msg) { write("WARN", msg); }
void error(const std::string& msg) { write("ERROR", msg); }
void debug(const std::string& msg) {
#ifndef NDEBUG
    write("DEBUG", msg);
#else
    (void)msg;
#endif
}

}  // namespace sc::applog
