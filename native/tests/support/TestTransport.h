#pragma once
// Header-only test doubles for transports: a recording TransportListener and a scriptable Prompter.
#include "core/Strings.h"
#include "session/Prompter.h"
#include "transport/Transport.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace st::test {

struct RecordingListener : TransportListener {
    std::mutex m;
    std::condition_variable cv;
    std::string data, local;
    std::vector<ConnStatus> statuses;
    std::vector<std::string> logs;
    std::vector<LdiscModes> ldisc;
    bool closed = false;
    int closeCount = 0;
    CloseInfo info;

    void onData(std::string_view b) override {
        std::lock_guard l(m);
        data.append(b);
        cv.notify_all();
    }
    void onStatus(ConnStatus s, const std::string&) override {
        std::lock_guard l(m);
        statuses.push_back(s);
        cv.notify_all();
    }
    void onLog(LogLevel, const std::string& msg) override {
        std::lock_guard l(m);
        logs.push_back(msg);
    }
    void onClose(const CloseInfo& i) override {
        std::lock_guard l(m);
        closed = true;
        ++closeCount;
        info = i;
        cv.notify_all();
    }
    void onLdiscChanged(LdiscModes md) override {
        std::lock_guard l(m);
        ldisc.push_back(md);
        cv.notify_all();
    }
    void onLocalOutput(const std::string& t) override {
        std::lock_guard l(m);
        local += t;
        cv.notify_all();
    }
    template <class Pred>
    bool waitFor(Pred pred, std::chrono::milliseconds t = std::chrono::seconds(5)) {
        std::unique_lock l(m);
        return cv.wait_for(l, t, pred);
    }
    bool waitData(const std::string& needle, std::chrono::milliseconds t = std::chrono::seconds(5)) {
        return waitFor([&] { return data.find(needle) != std::string::npos; }, t);
    }
    bool waitConnected(std::chrono::milliseconds t = std::chrono::seconds(10)) {
        return waitFor([&] { return closed || std::count(statuses.begin(), statuses.end(), ConnStatus::Connected) > 0; }, t) &&
               !closed;
    }
    bool waitClosed(std::chrono::milliseconds t = std::chrono::seconds(10)) {
        return waitFor([&] { return closed; }, t);
    }
    std::string allLogs() {
        std::lock_guard l(m);
        return join(logs, "\n");
    }
};

struct ScriptedPrompter : Prompter {
    std::function<PromptResponse(const Json&)> handler;
    PromptResponse prompt(const PromptInput& in) override {
        if (handler) return handler(in.json);
        return PromptResponse{};
    }
};

}  // namespace st::test
