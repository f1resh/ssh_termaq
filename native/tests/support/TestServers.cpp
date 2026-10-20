// TestEchoServer and TestScriptServer (TestSshServer lives in TestSshServer.cpp, TempDir in TestEnv.cpp, posixShell and
// findTool in native/tests/platform/<os>/support).
#include "TestServers.h"

#include "TestNet.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace st::test {

namespace {
constexpr int kPollMs = 50;
constexpr size_t kEchoBacklogLimit = 1 << 20;  // stop reading from a client while this much echo is pending
}  // namespace

// ============================================================================================ TestEchoServer

struct TestEchoServer::Impl {
    net::Socket listener;
    int port = 0;
    std::atomic<bool> stopping{false};
    std::atomic<int> connections{0};
    std::thread thread;

    struct Client {
        net::Socket sock;
        std::string pending;  // bytes received but not yet echoed back
        bool readClosed = false;
    };

    void run() {
        std::vector<Client> clients;
        std::vector<net::PollItem> items;
        char buf[16384];
        while (!stopping.load()) {
            items.clear();
            items.push_back(net::PollItem{listener.get(), true, false});
            for (auto& c : clients) {
                net::PollItem it{c.sock.get()};
                it.read = !c.readClosed && c.pending.size() < kEchoBacklogLimit;
                it.write = !c.pending.empty();
                items.push_back(it);
            }
            if (!net::pollSockets(items, kPollMs)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            // Existing clients first (indices of `items` match `clients` before new accepts are appended).
            for (size_t i = 0; i < clients.size(); ++i) {
                auto& c = clients[i];
                const net::PollItem& it = items[i + 1];
                bool dead = false;
                if (it.readable) {
                    if (!c.readClosed) {
                        const long n = sys::recvSome(c.sock.get(), buf, sizeof(buf));
                        if (n > 0) c.pending.append(buf, static_cast<size_t>(n));
                        else if (n == 0) c.readClosed = true;
                        else if (!net::wouldBlock()) dead = true;
                    } else if (it.failed) {
                        dead = true;
                    }
                }
                if (!dead && !c.pending.empty()) {
                    const long n = sys::sendSome(c.sock.get(), c.pending.data(), std::min<size_t>(c.pending.size(), 1 << 20));
                    if (n > 0) c.pending.erase(0, static_cast<size_t>(n));
                    else if (n < 0 && !net::wouldBlock()) dead = true;
                }
                if (!dead && c.readClosed && c.pending.empty()) {
                    sys::shutdownWrite(c.sock.get());
                    dead = true;
                }
                if (dead) c.sock.reset();
            }
            std::erase_if(clients, [](const Client& c) { return !c.sock.valid(); });
            if (items[0].readable) {
                for (;;) {
                    net::Socket s = net::acceptNonBlocking(listener.get());
                    if (!s) break;
                    net::setNonBlocking(s.get(), true);
                    connections.fetch_add(1);
                    clients.push_back(Client{std::move(s), {}, false});
                }
            }
        }
        // stop(): close all client connections (Socket destructors).
    }
};

TestEchoServer::TestEchoServer() : impl_(std::make_unique<Impl>()) {
    impl_->listener = net::listenLoopback(0, impl_->port);
    net::setNonBlocking(impl_->listener.get(), true);
    impl_->thread = std::thread([this] { impl_->run(); });
}

TestEchoServer::~TestEchoServer() { stop(); }

int TestEchoServer::port() const { return impl_->port; }
int TestEchoServer::connections() const { return impl_->connections.load(); }

void TestEchoServer::stop() {
    impl_->stopping.store(true);
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->listener.reset();
}

// ========================================================================================== TestScriptServer

struct TestScriptServer::Impl {
    net::Socket listener;
    net::Socket client;  // closed only by stop() after the reader thread has exited
    int port = 0;
    std::atomic<bool> stopping{false};
    std::thread thread;

    mutable std::mutex mu;
    mutable std::condition_variable cv;
    std::string received;
    bool accepted = false;
    bool disconnected = false;  // peer closed, socket error or closeClient()
    std::mutex sendMu;

    void run() {
        // 1. Wait for exactly one connection, then stop listening so further connects are refused.
        while (!stopping.load()) {
            if (!net::waitReadable(listener.get(), kPollMs)) continue;
            net::Socket s(sys::acceptConnection(listener.get(), true));  // TCP_NODELAY
            if (!s) continue;
            net::setNonBlocking(s.get(), false);
            {
                std::lock_guard lk(mu);
                client = std::move(s);
                accepted = true;
            }
            cv.notify_all();
            break;
        }
        listener.reset();
        // 2. Record everything the client sends until it disconnects (or the server is stopped).
        char buf[16384];
        while (!stopping.load() && client.valid()) {
            if (!net::waitReadable(client.get(), kPollMs)) continue;
            const long n = sys::recvSome(client.get(), buf, sizeof(buf));
            if (n > 0) {
                {
                    std::lock_guard lk(mu);
                    received.append(buf, static_cast<size_t>(n));
                }
                cv.notify_all();
                continue;
            }
            if (n < 0 && net::wouldBlock()) continue;
            break;  // orderly close or error
        }
        {
            std::lock_guard lk(mu);
            disconnected = true;
        }
        cv.notify_all();
    }
};

TestScriptServer::TestScriptServer() : impl_(std::make_unique<Impl>()) {
    impl_->listener = net::listenLoopback(0, impl_->port);
    net::setNonBlocking(impl_->listener.get(), true);
    impl_->thread = std::thread([this] { impl_->run(); });
}

TestScriptServer::~TestScriptServer() { stop(); }

int TestScriptServer::port() const { return impl_->port; }

bool TestScriptServer::waitForConnection(int timeoutMs) {
    std::unique_lock lk(impl_->mu);
    return impl_->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return impl_->accepted; });
}

std::string TestScriptServer::received() const {
    std::lock_guard lk(impl_->mu);
    return impl_->received;
}

bool TestScriptServer::waitForReceived(const std::string& needle, int timeoutMs) const {
    std::unique_lock lk(impl_->mu);
    return impl_->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                              [&] { return impl_->received.find(needle) != std::string::npos; });
}

void TestScriptServer::send(const std::string& bytes) {
    net::Handle s = sys::kInvalidSocket;
    {
        std::lock_guard lk(impl_->mu);
        if (!impl_->accepted) return;
        s = impl_->client.get();
    }
    if (s == sys::kInvalidSocket) return;
    std::lock_guard lk(impl_->sendMu);  // keep concurrent send() calls from interleaving
    net::sendAll(s, bytes);
}

void TestScriptServer::closeClient() {
    {
        std::lock_guard lk(impl_->mu);
        if (!impl_->accepted || !impl_->client.valid()) return;
        // Graceful close (FIN). The reader thread keeps draining until the peer closes its side.
        sys::shutdownWrite(impl_->client.get());
        impl_->disconnected = true;
    }
    impl_->cv.notify_all();
}

void TestScriptServer::stop() {
    impl_->stopping.store(true);
    if (impl_->thread.joinable()) impl_->thread.join();
    std::lock_guard lk(impl_->mu);
    impl_->listener.reset();
    if (impl_->client.valid()) {
        sys::shutdownBoth(impl_->client.get());
        impl_->client.reset();
    }
    impl_->disconnected = impl_->accepted;
    impl_->cv.notify_all();
}

bool TestScriptServer::connected() const {
    std::lock_guard lk(impl_->mu);
    return impl_->accepted && !impl_->disconnected;
}

bool TestScriptServer::waitForDisconnect(int timeoutMs) const {
    std::unique_lock lk(impl_->mu);
    return impl_->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                              [&] { return impl_->accepted && impl_->disconnected; });
}

}  // namespace st::test
