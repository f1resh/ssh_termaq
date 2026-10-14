// Drag & drop building blocks: the virtual-file data object (formats, streams, SetData store, async capability), its
// MTA proxy (reads never run on the calling STA thread), a real shell drop into a folder (Explorer's own drop handler
// copies the virtual files), and the shell data object for local files.
#include "TestServers.h"

#include "core/Strings.h"
#include "core/Win.h"
#include "dnd/DragDrop.h"

#include <objbase.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

using namespace sc;
using namespace sc::test;
namespace fs = std::filesystem;

namespace {

struct OleScope {
    HRESULT hr;
    OleScope() : hr(OleInitialize(nullptr)) {}
    ~OleScope() {
        if (SUCCEEDED(hr)) OleUninitialize();
    }
};

class FakeSource final : public dnd::VirtualSource {
public:
    std::map<std::string, std::string> files;  // rel -> content
    std::vector<dnd::VirtualItem> items;
    std::mutex m;
    std::set<std::thread::id> readThreads;
    std::atomic<int> enumerations{0};

    FakeSource() {
        std::string big(1024 * 1024 + 5, '\0');
        for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>((i * 13 + 1) & 0xff);
        items = {{"folder", true, 0, 0},
                 {"folder/a.txt", false, 5, 1700000000},
                 {"folder/sub", true, 0, 0},
                 {"folder/sub/b.bin", false, big.size(), 1600000000},
                 {"top file.txt", false, 3, 0}};
        files = {{"folder/a.txt", "hello"}, {"folder/sub/b.bin", big}, {"top file.txt", "top"}};
    }

    std::vector<dnd::VirtualItem> enumerate() override {
        ++enumerations;
        return items;
    }

    std::unique_ptr<dnd::VirtualReader> open(size_t index) override {
        struct R : dnd::VirtualReader {
            FakeSource* src;
            std::string data;
            size_t pos = 0;
            size_t read(void* out, size_t len) override {
                {
                    std::lock_guard lk(src->m);
                    src->readThreads.insert(std::this_thread::get_id());
                }
                const size_t n = std::min(len, std::min<size_t>(data.size() - pos, 7000));  // short reads on purpose
                std::memcpy(out, data.data() + pos, n);
                pos += n;
                return n;
            }
        };
        auto r = std::make_unique<R>();
        r->src = this;
        r->data = files.at(items.at(index).relPath);
        return r;
    }
};

FORMATETC fmt(const wchar_t* name, DWORD tymed, LONG index = -1) {
    return FORMATETC{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(name)), nullptr, DVASPECT_CONTENT, index, tymed};
}

std::string readAll(IStream* s) {
    std::string out;
    char buf[3000];
    for (;;) {
        ULONG n = 0;
        REQUIRE(SUCCEEDED(s->Read(buf, sizeof buf, &n)));
        if (n == 0) break;
        out.append(buf, n);
    }
    return out;
}

std::string slurpFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

/** Pumps messages until pred() or timeout. */
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

/** Drops `data` onto a folder through the shell's own drop target (what Explorer does). */
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
    REQUIRE(SUCCEEDED(target->DragOver(MK_LBUTTON, pt, &effect)));
    effect = DROPEFFECT_COPY;
    REQUIRE(SUCCEEDED(target->Drop(data, 0, pt, &effect)));
    target->Release();
}

}  // namespace

TEST_CASE("dnd: virtual data object", "[dnd]") {
    OleScope ole;
    auto src = std::make_shared<FakeSource>();
    IDataObject* obj = dnd::createVirtualDataObject(src, false);
    REQUIRE(obj);

    FORMATETC fd = fmt(CFSTR_FILEDESCRIPTORW, TYMED_HGLOBAL);
    CHECK(obj->QueryGetData(&fd) == S_OK);
    STGMEDIUM med{};
    REQUIRE(obj->GetData(&fd, &med) == S_OK);
    auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(med.hGlobal));
    REQUIRE(group->cItems == 5);
    CHECK(std::wstring(group->fgd[0].cFileName) == L"folder");
    CHECK((group->fgd[0].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    CHECK(std::wstring(group->fgd[3].cFileName) == L"folder\\sub\\b.bin");
    CHECK(group->fgd[3].nFileSizeLow == 1024 * 1024 + 5);
    CHECK((group->fgd[1].dwFlags & FD_WRITESTIME) != 0);
    CHECK((group->fgd[4].dwFlags & FD_WRITESTIME) == 0);
    GlobalUnlock(med.hGlobal);
    ReleaseStgMedium(&med);
    CHECK(src->enumerations == 1);

    // Contents
    FORMATETC fc = fmt(CFSTR_FILECONTENTS, TYMED_ISTREAM, 3);
    REQUIRE(obj->GetData(&fc, &med) == S_OK);
    REQUIRE(med.tymed == TYMED_ISTREAM);
    STATSTG st{};
    REQUIRE(med.pstm->Stat(&st, STATFLAG_DEFAULT) == S_OK);
    CHECK(st.cbSize.QuadPart == 1024 * 1024 + 5);
    CHECK(std::wstring(st.pwcsName) == L"b.bin");
    CoTaskMemFree(st.pwcsName);
    CHECK(readAll(med.pstm) == src->files["folder/sub/b.bin"]);
    LARGE_INTEGER zero{};
    ULARGE_INTEGER pos{};
    REQUIRE(med.pstm->Seek(zero, STREAM_SEEK_SET, &pos) == S_OK);  // rewind = reopen
    CHECK(readAll(med.pstm).size() == 1024 * 1024 + 5);
    ReleaseStgMedium(&med);
    FORMATETC dirContents = fmt(CFSTR_FILECONTENTS, TYMED_ISTREAM, 0);
    CHECK(obj->GetData(&dirContents, &med) == DV_E_LINDEX);
    CHECK(src->enumerations == 1);

    // Generic store (drag image helper data and the like)
    FORMATETC custom = fmt(L"SSHCrawler.Test", TYMED_HGLOBAL);
    CHECK(obj->QueryGetData(&custom) == DV_E_FORMATETC);
    STGMEDIUM in{};
    in.tymed = TYMED_HGLOBAL;
    in.hGlobal = GlobalAlloc(GMEM_MOVEABLE, 4);
    std::memcpy(GlobalLock(in.hGlobal), "abcd", 4);
    GlobalUnlock(in.hGlobal);
    REQUIRE(obj->SetData(&custom, &in, TRUE) == S_OK);
    REQUIRE(obj->GetData(&custom, &med) == S_OK);
    CHECK(std::memcmp(GlobalLock(med.hGlobal), "abcd", 4) == 0);
    GlobalUnlock(med.hGlobal);
    ReleaseStgMedium(&med);

    IEnumFORMATETC* en = nullptr;
    REQUIRE(obj->EnumFormatEtc(DATADIR_GET, &en) == S_OK);
    FORMATETC all[16];
    ULONG fetched = 0;
    en->Next(16, all, &fetched);
    en->Release();
    CHECK(fetched == 4);

    IDataObjectAsyncCapability* async = nullptr;
    REQUIRE(obj->QueryInterface(IID_PPV_ARGS(&async)) == S_OK);
    BOOL mode = FALSE;
    async->GetAsyncMode(&mode);
    CHECK(mode == TRUE);
    async->Release();
    obj->Release();
}

TEST_CASE("dnd: the MTA object serves reads off the calling thread", "[dnd]") {
    OleScope ole;
    auto src = std::make_shared<FakeSource>();
    IDataObject* proxy = dnd::createVirtualDataObjectInMta(src, false);
    REQUIRE(proxy);
    FORMATETC fc = fmt(CFSTR_FILECONTENTS, TYMED_ISTREAM, 1);
    STGMEDIUM med{};
    REQUIRE(proxy->GetData(&fc, &med) == S_OK);
    CHECK(readAll(med.pstm) == "hello");
    ReleaseStgMedium(&med);
    proxy->Release();
    std::lock_guard lk(src->m);
    REQUIRE_FALSE(src->readThreads.empty());
    CHECK(src->readThreads.count(std::this_thread::get_id()) == 0);
}

TEST_CASE("dnd: Explorer's drop handler copies virtual files into a folder", "[dnd]") {
    OleScope ole;
    TempDir dest;
    auto src = std::make_shared<FakeSource>();
    for (bool viaMta : {false, true}) {
        TempDir target;
        IDataObject* obj = viaMta ? dnd::createVirtualDataObjectInMta(src, false) : dnd::createVirtualDataObject(src, false);
        REQUIRE(obj);
        shellDrop(obj, target.path());
        obj->Release();
        // Explorer sizes the files first and fills them afterwards: wait for the contents.
        const fs::path b = target.path() / "folder" / "sub" / "b.bin";
        auto complete = [&] {
            return slurpFile(target.path() / "top file.txt") == "top" &&
                   slurpFile(target.path() / "folder" / "a.txt") == "hello" &&
                   slurpFile(b) == src->files["folder/sub/b.bin"];
        };
        CHECK(pumpUntil(complete, std::chrono::seconds(30)));
        CHECK(fs::is_directory(target.path() / "folder" / "sub"));
    }
}

TEST_CASE("dnd: shell data object for local files", "[dnd]") {
    OleScope ole;
    TempDir tmp;
    { std::ofstream(tmp.path() / "one.txt") << "1"; }
    { std::ofstream(tmp.path() / "two.txt") << "2"; }
    const std::vector<std::string> paths = {narrow((tmp.path() / "one.txt").wstring()),
                                            narrow((tmp.path() / "two.txt").wstring())};
    IDataObject* obj = dnd::createShellDataObject(paths);
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
    CHECK(dnd::createShellDataObject({narrow((tmp.path() / "missing.txt").wstring())}) == nullptr);
}
