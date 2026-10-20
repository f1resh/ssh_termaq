#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace st {

/** Marshals work onto the UI thread (implemented by the host window, platform/ui/HostWindow.h). */
class Dispatcher {
public:
    virtual ~Dispatcher() = default;
    /** Thread-safe. Queues fn to run on the UI thread. */
    virtual void post(std::function<void()> fn) = 0;
    virtual bool isUiThread() const = 0;
};

/** Fixed-size worker pool for blocking background work (bridge async handlers, imports, keygen...). */
class ThreadPool {
public:
    explicit ThreadPool(size_t threads);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void submit(std::function<void()> job);

    /** Process-wide pool (4 threads), created on first use. */
    static ThreadPool& shared();

private:
    void run();
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    std::vector<std::thread> threads_;
    bool stop_ = false;
};

}  // namespace st
