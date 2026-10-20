// Command lines forwarded by a second instance (WM_COPYDATA) are split with the Windows rules.
#include "platform/win/ui/HostWindowWin.h"

#include <catch2/catch_test_macros.hpp>

using namespace st;

TEST_CASE("cli: splitCommandLine follows Windows quoting rules", "[app][cli]") {
    auto a = ui::splitCommandLine(L"SSHTermaq.exe -load \"my session\" -pw \"a\\\"b\" h\u00e9.test");
    REQUIRE(a == std::vector<std::string>{"-load", "my session", "-pw", "a\"b", "h\xC3\xA9.test"});
    REQUIRE(ui::splitCommandLine(L"").empty());
    REQUIRE(ui::splitCommandLine(L"app.exe").empty());
}
