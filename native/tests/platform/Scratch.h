#pragma once
// A fresh folder below the system temp folder for one test, removed afterwards (platform tests do not use
// native/tests/support, so they build on every OS first).
#include <filesystem>
#include <random>
#include <string>

namespace st::test {

class Scratch {
public:
    Scratch() {
        std::random_device rd;
        dir_ = std::filesystem::temp_directory_path() /
               ("st-platform-" + std::to_string(rd()) + "-" + std::to_string(rd()));
        std::filesystem::create_directories(dir_);
    }
    ~Scratch() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    const std::filesystem::path& path() const { return dir_; }

private:
    std::filesystem::path dir_;
};

}  // namespace st::test
