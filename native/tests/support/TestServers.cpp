// STUB — to be implemented (owner: test-infrastructure module).
#include "TestServers.h"

#include "core/Uuid.h"

#include <stdexcept>

namespace sc::test {

struct TestSshServer::Impl {};
TestSshServer::TestSshServer(TestSshServerOptions) : impl_(std::make_unique<Impl>()) {
    throw std::logic_error("TestSshServer not implemented yet");
}
TestSshServer::~TestSshServer() = default;
int TestSshServer::port() const { return 0; }
std::string TestSshServer::hostKeyType() const { return {}; }
std::string TestSshServer::hostKeyBase64() const { return {}; }
std::string TestSshServer::hostKeyFingerprint() const { return {}; }
int TestSshServer::lastRemoteForwardPort() const { return 0; }
std::vector<std::string> TestSshServer::events() const { return {}; }
bool TestSshServer::waitForEvent(const std::string&, int) const { return false; }
void TestSshServer::stop() {}

struct TestEchoServer::Impl {};
TestEchoServer::TestEchoServer() : impl_(std::make_unique<Impl>()) { throw std::logic_error("TestEchoServer not implemented yet"); }
TestEchoServer::~TestEchoServer() = default;
int TestEchoServer::port() const { return 0; }
int TestEchoServer::connections() const { return 0; }
void TestEchoServer::stop() {}

struct TestScriptServer::Impl {};
TestScriptServer::TestScriptServer() : impl_(std::make_unique<Impl>()) {
    throw std::logic_error("TestScriptServer not implemented yet");
}
TestScriptServer::~TestScriptServer() = default;
int TestScriptServer::port() const { return 0; }
bool TestScriptServer::waitForConnection(int) { return false; }
std::string TestScriptServer::received() const { return {}; }
bool TestScriptServer::waitForReceived(const std::string&, int) const { return false; }
void TestScriptServer::send(const std::string&) {}
void TestScriptServer::closeClient() {}
void TestScriptServer::stop() {}

TempDir::TempDir() : path_(std::filesystem::temp_directory_path() / ("sc-test-" + sc::newUuid())) {
    std::filesystem::create_directories(path_);
}
TempDir::~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
}

}  // namespace sc::test
