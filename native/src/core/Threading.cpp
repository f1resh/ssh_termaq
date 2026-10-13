#include "core/Threading.h"

#include "core/Log.h"

namespace sc {

ThreadPool::ThreadPool(size_t threads) {
    for (size_t i = 0; i < threads; ++i) threads_.emplace_back([this] { run(); });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_)
        if (t.joinable()) t.join();
}

void ThreadPool::submit(std::function<void()> job) {
    {
        std::lock_guard lock(mutex_);
        jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
}

void ThreadPool::run() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (stop_ && jobs_.empty()) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        try {
            job();
        } catch (const std::exception& e) {
            applog::error(std::string("ThreadPool job threw: ") + e.what());
        } catch (...) {
            applog::error("ThreadPool job threw unknown exception");
        }
    }
}

ThreadPool& ThreadPool::shared() {
    static ThreadPool pool(4);
    return pool;
}

}  // namespace sc
