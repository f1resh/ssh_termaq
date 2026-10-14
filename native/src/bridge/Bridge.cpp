#include "bridge/Bridge.h"

#include "core/Log.h"

namespace st {

struct Bridge::Responder::State {
    Bridge* bridge = nullptr;
    std::shared_ptr<std::atomic<bool>> alive;
    int64_t id = 0;
    std::atomic<bool> done{false};

    ~State() {
        // Dropped without a reply -> reject so the UI promise never hangs.
        if (!done.exchange(true) && alive && alive->load() && bridge)
            bridge->respond(id, false, Json("Request was dropped without a response"));
    }
};

void Bridge::Responder::resolve(Json result) const {
    if (!state_ || state_->done.exchange(true)) return;
    if (state_->alive && state_->alive->load()) state_->bridge->respond(state_->id, true, result);
}

void Bridge::Responder::reject(const std::string& message) const {
    if (!state_ || state_->done.exchange(true)) return;
    if (state_->alive && state_->alive->load()) state_->bridge->respond(state_->id, false, Json(message));
}

bool Bridge::Responder::done() const { return !state_ || state_->done.load(); }

Bridge::Bridge(Dispatcher& dispatcher) : dispatcher_(dispatcher), alive_(std::make_shared<std::atomic<bool>>(true)) {}

Bridge::~Bridge() { alive_->store(false); }

void Bridge::setSink(std::function<void(const std::string&)> sink) {
    std::lock_guard lock(mutex_);
    sink_ = std::move(sink);
}

void Bridge::handle(const std::string& channel, InvokeHandler h) {
    std::lock_guard lock(mutex_);
    invokes_[channel] = std::move(h);
}

void Bridge::handleSync(const std::string& channel, SyncHandler h) {
    handle(channel, [h = std::move(h), channel](const Json& args, Responder reply) {
        try {
            reply.resolve(h(args));
        } catch (const std::exception& e) {
            reply.reject(e.what());
        }
    });
}

void Bridge::handleAsync(const std::string& channel, SyncHandler h) {
    auto shared = std::make_shared<SyncHandler>(std::move(h));
    handle(channel, [shared](const Json& args, Responder reply) {
        ThreadPool::shared().submit([shared, args, reply] {
            try {
                reply.resolve((*shared)(args));
            } catch (const std::exception& e) {
                reply.reject(e.what());
            }
        });
    });
}

void Bridge::on(const std::string& channel, SendHandler h) {
    std::lock_guard lock(mutex_);
    sends_[channel] = std::move(h);
}

bool Bridge::hasHandler(const std::string& channel) const {
    std::lock_guard lock(mutex_);
    return invokes_.count(channel) || sends_.count(channel);
}

void Bridge::emit(const std::string& channel, Json args) {
    if (!args.is_array()) args = Json::array({std::move(args)});
    Json msg = {{"t", "evt"}, {"ch", channel}, {"args", std::move(args)}};
    post(msg.dump(-1, ' ', false, Json::error_handler_t::replace));
}

void Bridge::respond(int64_t id, bool ok, const Json& payload) {
    Json msg = {{"t", "res"}, {"id", id}, {"ok", ok}};
    if (ok)
        msg["result"] = payload;
    else
        msg["error"] = payload.is_string() ? payload : Json(payload.dump());
    post(msg.dump(-1, ' ', false, Json::error_handler_t::replace));
}

void Bridge::post(std::string text) {
    auto alive = alive_;
    auto deliver = [this, alive, text = std::move(text)] {
        if (!alive->load()) return;
        std::function<void(const std::string&)> sink;
        {
            std::lock_guard lock(mutex_);
            sink = sink_;
        }
        if (sink) sink(text);
    };
    if (dispatcher_.isUiThread())
        deliver();
    else
        dispatcher_.post(std::move(deliver));
}

void Bridge::onWebMessage(const std::string& json, const std::vector<std::string>& filePaths) {
    Json msg;
    try {
        msg = Json::parse(json);
    } catch (const std::exception& e) {
        applog::warn(std::string("Bridge: invalid JSON from UI: ") + e.what());
        return;
    }
    if (!msg.is_object()) return;
    const std::string t = msg.value("t", "");
    const std::string ch = msg.value("ch", "");
    Json args = msg.contains("args") && msg["args"].is_array() ? msg["args"] : Json::array();

    if (t == "req") {
        Responder reply;
        reply.state_ = std::make_shared<Responder::State>();
        reply.state_->bridge = this;
        reply.state_->alive = alive_;
        reply.state_->id = msg.value("id", int64_t{0});

        if (ch == "files:pathsForFiles") {
            reply.resolve(filePaths);
            return;
        }
        InvokeHandler h;
        {
            std::lock_guard lock(mutex_);
            auto it = invokes_.find(ch);
            if (it != invokes_.end()) h = it->second;
        }
        if (!h) {
            applog::warn("Bridge: no handler for invoke " + ch);
            reply.reject("Not implemented: " + ch);
            return;
        }
        try {
            h(args, reply);
        } catch (const std::exception& e) {
            reply.reject(e.what());
        }
    } else if (t == "send") {
        SendHandler h;
        {
            std::lock_guard lock(mutex_);
            auto it = sends_.find(ch);
            if (it != sends_.end()) h = it->second;
        }
        if (!h) {
            applog::warn("Bridge: no handler for send " + ch);
            return;
        }
        try {
            h(args);
        } catch (const std::exception& e) {
            applog::error("Bridge: send handler " + ch + " threw: " + e.what());
        }
    }
}

}  // namespace st
