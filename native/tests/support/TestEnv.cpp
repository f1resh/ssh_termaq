// What every test executable linking the test support shares, on every OS: TempDir, and secrets protected with a
// fixed test key instead of the user's real key store (macOS: the login Keychain; no effect on Windows).
#include "TestServers.h"

#include "core/Uuid.h"
#include "platform/sys/Secret.h"

namespace st::test {

namespace {

// Runs when a test executable starts, as this file also holds TempDir, which every test touching the store uses.
const bool kTestSecretKey = [] {
    sys::useTestSecretKey(std::string(32, 't'));
    return true;
}();

}  // namespace

TempDir::TempDir() : path_(std::filesystem::temp_directory_path() / ("st-test-" + st::newUuid())) {
    std::filesystem::create_directories(path_);
}

TempDir::~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
}

}  // namespace st::test
