// Drag & drop on macOS, the part that runs without a mouse: the paths of files dropped from Finder, found by the names
// the page has (a File has no path in WebKit).
#include "platform/mac/ui/DragDropMacSupport.h"

#include <catch2/catch_test_macros.hpp>

using namespace st::ui::dnd;

TEST_CASE("dnd (macOS): dropped files are found by the page's names", "[dnd]") {
    const std::vector<std::string> dropped = {"/Users/a/x.txt", "/Users/a/folder", "/tmp/x.txt", "/Volumes/USB/y.bin"};
    CHECK(mac::matchDropped(dropped, {"folder", "x.txt", "y.bin"}) ==
          std::vector<std::string>{"/Users/a/folder", "/Users/a/x.txt", "/Volumes/USB/y.bin"});
    // The same name twice: each path once, in drop order.
    CHECK(mac::matchDropped(dropped, {"x.txt", "x.txt", "x.txt"}) == std::vector<std::string>{"/Users/a/x.txt", "/tmp/x.txt"});
    CHECK(mac::matchDropped(dropped, {"other"}).empty());
    CHECK(mac::matchDropped(dropped, {}) == dropped);
    CHECK(mac::matchDropped({}, {"x.txt"}).empty());
}
