#include "localfs/LocalFsService.h"

#include "bridge/Bridge.h"
#include "core/Log.h"
#include "core/Uuid.h"
#include "dnd/DragDrop.h"
#include "localfs/LocalFs.h"

#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace st {

namespace {

/** Required string argument (the generic argStr() silently defaults). */
std::string argString(const Json& args, size_t i) {
    const Json& v = jsonArg(args, i);
    if (!v.is_string()) throw std::invalid_argument("argument " + std::to_string(i) + " must be a string");
    return v.get<std::string>();
}

}  // namespace

struct LocalFsService::Impl {
    Bridge& bridge;
    std::function<HWND__*()> ownerWindow;
    std::mutex m;
    std::unordered_map<std::string, std::unique_ptr<localfs::DirWatcher>> watchers;

    Impl(Bridge& b, std::function<HWND__*()> owner) : bridge(b), ownerWindow(std::move(owner)) {}

    localfs::ShellOpOptions shellOpts() const {
        localfs::ShellOpOptions o;
        o.owner = ownerWindow ? ownerWindow() : nullptr;
        return o;
    }
};

LocalFsService::LocalFsService(Bridge& bridge, std::function<HWND__*()> ownerWindow)
    : impl_(std::make_shared<Impl>(bridge, std::move(ownerWindow))) {}

LocalFsService::~LocalFsService() { closeAll(); }

void LocalFsService::registerHandlers() {
    auto impl = impl_;
    Bridge& b = impl->bridge;

    b.handleAsync("local:list", [](const Json& a) {
        const std::string path = argString(a, 0);
        Json out = Json::array();
        for (auto& e : path.empty() ? localfs::listPlaces() : localfs::list(path)) out.push_back(e);
        return out;
    });
    b.handleAsync("local:stat", [](const Json& a) -> Json {
        auto e = localfs::stat(argString(a, 0));
        return e ? Json(*e) : Json(nullptr);
    });
    b.handleAsync("local:home", [](const Json&) { return Json(localfs::documentsFolder()); });
    b.handleAsync("local:mkdir", [](const Json& a) {
        localfs::makeDir(argString(a, 0));
        return Json(nullptr);
    });
    b.handleAsync("local:rename", [](const Json& a) {
        localfs::rename(argString(a, 0), argString(a, 1));
        return Json(nullptr);
    });
    b.handleAsync("local:remove", [impl](const Json& a) {
        localfs::remove(argStrList(a, 0), argBool(a, 1, true), impl->shellOpts());
        return Json(nullptr);
    });
    b.handleAsync("local:copy", [impl](const Json& a) {
        localfs::copy(argStrList(a, 0), argString(a, 1), argBool(a, 2, false), impl->shellOpts());
        return Json(nullptr);
    });
    b.handleAsync("local:open", [](const Json& a) {
        localfs::open(argString(a, 0));
        return Json(nullptr);
    });
    b.handleAsync("local:reveal", [](const Json& a) {
        localfs::reveal(argString(a, 0));
        return Json(nullptr);
    });
    b.handleAsync("local:watch", [impl](const Json& a) {
        const std::string id = newUuid();
        std::weak_ptr<Impl> weak = impl;
        auto watcher = std::make_unique<localfs::DirWatcher>(argString(a, 0), [weak, id] {
            if (auto self = weak.lock()) self->bridge.emit("local:onChanged", Json::array({id}));
        });
        std::lock_guard lk(impl->m);
        impl->watchers[id] = std::move(watcher);
        return Json(id);
    });
    b.handle("local:startDrag", [impl](const Json& a, Bridge::Responder reply) {
        dnd::startFileDrag(impl->bridge.dispatcher(), argStrList(a, 0), [reply](const DragResult& r) { reply.resolve(Json(r)); });
    });
    b.on("local:unwatch", [impl](const Json& a) {
        std::unique_ptr<localfs::DirWatcher> w;
        {
            std::lock_guard lk(impl->m);
            auto it = impl->watchers.find(argString(a, 0));
            if (it == impl->watchers.end()) return;
            w = std::move(it->second);
            impl->watchers.erase(it);
        }
        // w's destructor stops and joins its thread outside the lock.
    });
}

void LocalFsService::closeAll() {
    std::unordered_map<std::string, std::unique_ptr<localfs::DirWatcher>> all;
    {
        std::lock_guard lk(impl_->m);
        all.swap(impl_->watchers);
    }
    all.clear();
}

}  // namespace st
