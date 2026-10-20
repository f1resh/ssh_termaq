// Unit tests for the version and build id (app/BuildInfo, generated st_build_info.h).
#include "app/BuildInfo.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <regex>

using namespace st;

TEST_CASE("buildinfo: version, build number and commit come from the build", "[app][buildinfo]") {
    const BuildInfo& b = buildInfo();
    REQUIRE(std::regex_match(b.version, std::regex(R"(\d+\.\d+\.\d+)")));
    REQUIRE(b.number >= 0);
    REQUIRE(b.number <= 65535);
    // Built from this repository: a short hash, or "unknown" when git is not available.
    REQUIRE(std::regex_match(b.commit, std::regex(R"([0-9a-f]{7,}(-dirty)?|unknown)")));
}

TEST_CASE("buildinfo: the build time is the link time of this executable", "[app][buildinfo]") {
    const std::string& t = buildInfo().time;
    REQUIRE(std::regex_match(t, std::regex(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)")));
    REQUIRE(t >= "2025-01-01");
    const auto now = std::chrono::system_clock::now() + std::chrono::hours(24);
    REQUIRE(t <= isoUtc(std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count()));
}

TEST_CASE("buildinfo: UTC times and the one-line description", "[app][buildinfo]") {
    REQUIRE(isoUtc(1792059409) == "2026-10-15T10:16:49Z");
    REQUIRE(isoUtc(0).empty());

    BuildInfo b{"1.0.0", 10, "35de5d8-dirty", "2026-10-15T10:16:49Z"};
    REQUIRE(describeBuild(b) == "1.0.0 (build 10, 35de5d8-dirty, 2026-10-15 10:16:49 UTC)");
    b.time.clear();
    REQUIRE(describeBuild(b) == "1.0.0 (build 10, 35de5d8-dirty)");
}
