// Drag out to Explorer, end to end: remote files on the SFTP (and SCP) test server -> virtual-file data object in the
// MTA -> the shell's own drop handler for a folder, which pulls the streams over the SSH connection.
#include "SftpFixture.h"

#include "dnd/DragDrop.h"

#include <objbase.h>
#include <ole2.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <catch2/catch_test_macros.hpp>

#include <thread>

using namespace st;
using namespace st::test;
namespace fs = std::filesystem;

namespace {

struct OleScope {
    HRESULT hr;
    OleScope() : hr(OleInitialize(nullptr)) {}
    ~OleScope() {
        if (SUCCEEDED(hr)) OleUninitialize();
    }
};

template <class Pred>
bool pumpUntil(Pred pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

void shellDrop(IDataObject* data, const fs::path& folder) {
    IShellItem* item = nullptr;
    REQUIRE(SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&item))));
    IDropTarget* target = nullptr;
    REQUIRE(SUCCEEDED(item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&target))));
    item->Release();
    POINTL pt{0, 0};
    DWORD effect = DROPEFFECT_COPY;
    REQUIRE(SUCCEEDED(target->DragEnter(data, MK_LBUTTON, pt, &effect)));
    effect = DROPEFFECT_COPY;
    REQUIRE(SUCCEEDED(target->Drop(data, 0, pt, &effect)));
    target->Release();
}

void dragOut(SftpFixture& f, const std::string& base) {
    const fs::path r = f.remoteRoot.path();
    fs::create_directories(r / "tree" / "deep dir");
    spit(r / "tree" / "deep dir" / "data.bin", std::string(600000, 'd'));
    spit(r / "tree" / "readme.txt", "read me");
    spit(r / "single.txt", "single");

    auto source = f.sftp->dragSourceForTests(f.id, {base + "/tree", base + "/single.txt"});
    IDataObject* obj = dnd::createVirtualDataObjectInMta(source, false);
    REQUIRE(obj);
    TempDir target;
    shellDrop(obj, target.path());
    obj->Release();
    auto complete = [&] {
        return slurp(target.path() / "single.txt") == "single" && slurp(target.path() / "tree" / "readme.txt") == "read me" &&
               slurp(target.path() / "tree" / "deep dir" / "data.bin") == std::string(600000, 'd');
    };
    CHECK(pumpUntil(complete, std::chrono::seconds(30)));
}

}  // namespace

TEST_CASE("drag out: remote files land in an Explorer folder (SFTP)", "[drag]") {
    OleScope ole;
    SftpFixture f;
    dragOut(f, "");
}

TEST_CASE("drag out: remote files land in an Explorer folder (SCP)", "[drag]") {
    if (gitShell().empty()) SKIP("Git for Windows (sh.exe, ls.exe, scp.exe) is not installed");
    OleScope ole;
    TestSshServerOptions o;
    o.enableSftp = false;
    o.execShell = gitShell();
    SftpFixture f(o);
    REQUIRE(f.opened["protocol"] == "scp");
    dragOut(f, f.opened["home"].get<std::string>());
}
