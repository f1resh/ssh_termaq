// Drag & drop building blocks: the shell data object for local files.
#include "TestServers.h"

#include "core/Strings.h"
#include "platform/detail/WinApi.h"
#include "platform/win/sys/WinText.h"
#include "platform/win/ui/DragDropWin.h"

#include <objbase.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

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

}  // namespace

TEST_CASE("dnd: shell data object for local files", "[dnd]") {
    OleScope ole;
    TempDir tmp;
    { std::ofstream(tmp.path() / "one.txt") << "1"; }
    { std::ofstream(tmp.path() / "two.txt") << "2"; }
    const std::vector<std::string> paths = {narrow((tmp.path() / "one.txt").wstring()),
                                            narrow((tmp.path() / "two.txt").wstring())};
    IDataObject* obj = ui::dnd::createShellDataObject(paths);
    REQUIRE(obj);
    FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM med{};
    REQUIRE(obj->GetData(&fe, &med) == S_OK);
    auto drop = static_cast<HDROP>(med.hGlobal);
    REQUIRE(DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0) == 2);
    wchar_t buf[MAX_PATH];
    DragQueryFileW(drop, 0, buf, MAX_PATH);
    CHECK(iequals(narrow(buf), paths[0]));
    ReleaseStgMedium(&med);
    obj->Release();
    CHECK(ui::dnd::createShellDataObject({narrow((tmp.path() / "missing.txt").wstring())}) == nullptr);
}
