#pragma once
// Header-only helpers for tests that drive bridge handlers: a dispatcher running everything inline and a Bridge
// wired to a capturing sink (invoke() sends a request and waits for its response; events are collected).
// Tests using it must link st_bridge.
#include "bridge/Bridge.h"
#include "core/Threading.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace st::test {

struct InlineDispatcher : Dispatcher {
    void post(std::function<void()> fn) override { fn(); }
    bool isUiThread() const override { return true; }
};

struct BridgeHarness {
    InlineDispatcher dispatcher;
    Bridge bridge{dispatcher};
    std::mutex m;
    std::condition_variable cv;
    std::map<int64_t, Json> responses;
    std::vector<Json> events;
    int64_t nextId = 1;

    BridgeHarness() {
        bridge.setSink([this](const std::string& text) {
            Json j = Json::parse(text);
            std::lock_guard l(m);
            if (j["t"] == "res")
                responses[j["id"].get<int64_t>()] = j;
            else
                events.push_back(j);
            cv.notify_all();
        });
    }

    /** Raw response ({"ok":..., "result"|"error"}). */
    Json request(const std::string& ch, Json args,
                 std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)) {
        int64_t id;
        {
            std::lock_guard l(m);
            id = nextId++;
        }
        bridge.onWebMessage(Json{{"t", "req"}, {"id", id}, {"ch", ch}, {"args", args}}.dump());
        std::unique_lock l(m);
        if (!cv.wait_for(l, timeout, [&] { return responses.count(id) > 0; })) {
            throw std::runtime_error("timeout waiting for " + ch);
        }
        return responses[id];
    }

    /** Result of an invoke; throws with the error message when it was rejected. */
    Json invoke(const std::string& ch, Json args, std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)) {
        Json r = request(ch, std::move(args), timeout);
        if (!r["ok"].get<bool>()) throw std::runtime_error(ch + " failed: " + r["error"].get<std::string>());
        return r["result"];
    }

    /** Error message of an invoke that is expected to fail ("" when it succeeded). */
    std::string invokeError(const std::string& ch, Json args,
                            std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)) {
        Json r = request(ch, std::move(args), timeout);
        return r["ok"].get<bool>() ? std::string() : r["error"].get<std::string>();
    }

    void send(const std::string& ch, Json args) {
        bridge.onWebMessage(Json{{"t", "send"}, {"ch", ch}, {"args", args}}.dump());
    }

    template <class Pred>
    bool waitEvent(Pred pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)) {
        std::unique_lock l(m);
        return cv.wait_for(l, timeout, [&] {
            for (const auto& e : events)
                if (pred(e)) return true;
            return false;
        });
    }

    /** Waits until at least n events arrived on channel ch. */
    bool waitEventCount(const std::string& ch, size_t n, std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)) {
        std::unique_lock l(m);
        return cv.wait_for(l, timeout, [&] {
            size_t c = 0;
            for (const auto& e : events)
                if (e.value("ch", std::string()) == ch) ++c;
            return c >= n;
        });
    }

    /** Events on one channel received so far (copies). */
    std::vector<Json> eventsOn(const std::string& ch) {
        std::lock_guard l(m);
        std::vector<Json> out;
        for (const auto& e : events)
            if (e.value("ch", std::string()) == ch) out.push_back(e);
        return out;
    }
};

inline std::string slurp(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

inline void spit(const std::filesystem::path& p, const std::string& data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << data;
}

}  // namespace st::test
