// Unit tests for the embedded web UI (app/UiResources): index parsing, request paths, content types, and loading
// the files embedded into this test executable (Windows: resources from ui_test.rc).
#include "app/UiResources.h"

#include <catch2/catch_test_macros.hpp>

using namespace st;

namespace {
constexpr std::string_view kOrigin = "https://app.sshtermaq.local";
}

TEST_CASE("ui: index lines map paths to resource ids", "[app][ui]") {
    const auto ids = UiResources::parseIndex(
        "# comment\r\n"
        "2\tindex.html\r\n"
        "\n"
        "3\tassets/index-1a2b.js\n"
        "1\treserved-for-the-index.txt\n"   // id 1 is the index itself
        "x\tnot-a-number.txt\n"
        "4\n"                                // no path
        "\tno-id.txt\n"
        "70000\ttoo-big.txt\n"
        "5\tlast line without newline");
    REQUIRE(ids.size() == 3);
    CHECK(ids.at("index.html") == 2);
    CHECK(ids.at("assets/index-1a2b.js") == 3);
    CHECK(ids.at("last line without newline") == 5);
    CHECK(UiResources::parseIndex("").empty());
}

TEST_CASE("ui: request URIs become paths relative to the UI root", "[app][ui]") {
    CHECK(uiPathFromUri("https://app.sshtermaq.local/index.html", kOrigin) == "index.html");
    CHECK(uiPathFromUri("https://app.sshtermaq.local/", kOrigin) == "index.html");
    CHECK(uiPathFromUri("https://app.sshtermaq.local", kOrigin) == "index.html");
    CHECK(uiPathFromUri("https://app.sshtermaq.local/assets/index-1a2b.js", kOrigin) == "assets/index-1a2b.js");
    CHECK(uiPathFromUri("https://app.sshtermaq.local/index.html?x=1#top", kOrigin) == "index.html");
    CHECK(uiPathFromUri("https://app.sshtermaq.local?x=1", kOrigin) == "index.html");
    CHECK(uiPathFromUri("https://app.sshtermaq.local/assets/", kOrigin) == "assets/index.html");
    CHECK(uiPathFromUri("https://app.sshtermaq.local//assets/a.js", kOrigin) == "assets/a.js");
    CHECK(uiPathFromUri("https://app.sshtermaq.local/a%20b%2Ec.js", kOrigin) == "a b.c.js");
    CHECK(uiPathFromUri("HTTPS://App.SSHTermaq.Local/index.html", kOrigin) == "index.html");

    CHECK_FALSE(uiPathFromUri("https://app.sshtermaq.local.example/index.html", kOrigin));
    CHECK_FALSE(uiPathFromUri("https://example.com/index.html", kOrigin));
    CHECK_FALSE(uiPathFromUri("http://app.sshtermaq.local/index.html", kOrigin));
    CHECK_FALSE(uiPathFromUri("https://app.sshtermaq.local/bad%2", kOrigin));
    CHECK_FALSE(uiPathFromUri("https://app.sshtermaq.local/bad%zz", kOrigin));
    CHECK_FALSE(uiPathFromUri("https://app.sshtermaq.local/index.html", ""));
}

TEST_CASE("ui: content types by extension", "[app][ui]") {
    CHECK(uiContentType("index.html") == "text/html; charset=utf-8");
    CHECK(uiContentType("assets/index-1a2b.js") == "text/javascript; charset=utf-8");
    CHECK(uiContentType("assets/index-1a2b.CSS") == "text/css; charset=utf-8");
    CHECK(uiContentType("fonts/mono.woff2") == "font/woff2");
    CHECK(uiContentType("logo.svg") == "image/svg+xml");
    CHECK(uiContentType("README") == "application/octet-stream");
    CHECK(uiContentType("dir.v2/README") == "application/octet-stream");
    CHECK(uiContentType("archive.xyz") == "application/octet-stream");
}

TEST_CASE("ui: files embedded as resources are found by path", "[app][ui]") {
    const UiResources ui = UiResources::load();
    REQUIRE_FALSE(ui.empty());
    CHECK(ui.size() == 2);  // the index also lists "missing.js", which has no resource

    const auto html = ui.find("index.html");
    REQUIRE(html);
    CHECK(*html == "<!doctype html><title>embedded</title>");  // the data files have no line breaks (EOL-proof)
    const auto js = ui.find("assets/app.js");
    REQUIRE(js);
    CHECK(*js == "console.log(\"embedded\");");

    CHECK_FALSE(ui.find("missing.js"));
    CHECK_FALSE(ui.find("assets/APP.js"));  // exact match only
    CHECK_FALSE(ui.find("../index.html"));
}
