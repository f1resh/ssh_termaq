// The real Keychain behind platform/sys/Secret.h. Hidden: it creates the "SSHTermaq credentials key" item in the
// login Keychain (the same item the app uses), so it only runs when asked for:
//   st_test_platform "[.keychain]"
#include "platform/sys/Secret.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace st;

TEST_CASE("secret (macOS): round trip through the login Keychain's key", "[.keychain]") {
    sys::useTestSecretKey("");
    const std::string blob = sys::protectSecret("keychain round trip");
    REQUIRE(sys::unprotectSecret(blob) == std::optional<std::string>("keychain round trip"));
    sys::useTestSecretKey(std::string(32, 'k'));
    REQUIRE_FALSE(sys::unprotectSecret(blob).has_value());  // another key: no
    sys::useTestSecretKey("");
}
