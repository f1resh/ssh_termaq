// macOS specifics of platform/sys/Serial.h: the ports listed are call-out devices that exist, each once.
#include "platform/sys/Serial.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <set>
#include <string>

using namespace st;

TEST_CASE("serial (macOS): ports are listed as /dev/cu.* devices", "[platform][serial]") {
    std::set<std::string> seen;
    for (const auto& p : sys::serial::listPorts()) {
        INFO(p.path << " (" << p.friendlyName << ", " << p.manufacturer << ")");
        CHECK(p.path.rfind("/dev/cu.", 0) == 0);
        CHECK(std::filesystem::exists(p.path));
        CHECK_FALSE(p.friendlyName.empty());
        CHECK(seen.insert(p.path).second);
    }
}
