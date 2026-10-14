#pragma once
// Bridge handlers of the `local:*` namespace (local disks for the file manager's left pane) and the folder watchers
// behind local:watch / local:onChanged.
#include <functional>
#include <memory>

struct HWND__;

namespace st {

class Bridge;

class LocalFsService {
public:
    /** ownerWindow: parent for the shell's progress / conflict dialogs (may return null). */
    LocalFsService(Bridge& bridge, std::function<HWND__*()> ownerWindow);
    ~LocalFsService();

    void registerHandlers();
    /** Stops all folder watchers (app shutdown). */
    void closeAll();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace st
