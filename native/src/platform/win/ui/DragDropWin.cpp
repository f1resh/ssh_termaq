// Drag & drop on Windows: OLE DoDragDrop of local files with the shell's own data object.
#include "platform/win/ui/DragDropWin.h"

#include "core/Strings.h"
#include "core/Threading.h"
#include "platform/detail/WinApi.h"
#include "platform/win/sys/WinText.h"

#include <objbase.h>
#include <ole2.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace st::ui::dnd {

namespace {

HWND g_window = nullptr;

/* ------------------------------------------------------------------------ */
/* Drop source                                                               */
/* ------------------------------------------------------------------------ */

class DropSource final : public IDropSource {
public:
    explicit DropSource(HWND window) : window_(window) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *ppv = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
        if (escape) return DRAGDROP_S_CANCEL;
        if (keys & (MK_LBUTTON | MK_RBUTTON)) return S_OK;
        // Released: a drop back onto our own window is handled by the page, not by the WebView's drop target.
        POINT p{};
        GetCursorPos(&p);
        HWND under = WindowFromPoint(p);
        if (window_ && under && (under == window_ || GetAncestor(under, GA_ROOT) == window_)) {
            internal = true;
            point = p;
            move = (keys & MK_SHIFT) != 0;
            return DRAGDROP_S_CANCEL;
        }
        return DRAGDROP_S_DROP;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

    bool internal = false;
    POINT point{};
    bool move = false;

private:
    ~DropSource() = default;
    LONG ref_ = 1;
    HWND window_;
};

DragResult runDrag(IDataObject* data, DWORD effects) {
    DragResult r;
    if (!data) return r;
    auto* src = new DropSource(g_window);
    DWORD effect = DROPEFFECT_NONE;
    const HRESULT hr = SHDoDragDrop(g_window, data, src, effects, &effect);
    if (src->internal) {
        POINT p = src->point;
        if (g_window) ScreenToClient(g_window, &p);
        r.result = "internal";
        r.x = p.x;
        r.y = p.y;
        r.move = src->move;
    } else if (hr == DRAGDROP_S_DROP) {
        r.result = "external";
    } else {
        r.result = "cancelled";
    }
    src->Release();
    return r;
}

}  // namespace

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

void init(void* mainWindow) { g_window = static_cast<HWND>(mainWindow); }

IDataObject* createShellDataObject(const std::vector<std::string>& paths) {
    if (paths.empty()) return nullptr;
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (const auto& p : paths) {
        PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(widen(p).c_str());
        if (!pidl) {
            for (auto q : pidls) ILFree(q);
            return nullptr;
        }
        pidls.push_back(pidl);
    }
    IDataObject* obj = nullptr;
    IShellItemArray* array = nullptr;
    if (SUCCEEDED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()),
                                                    const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), &array))) {
        array->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&obj));
        array->Release();
    }
    for (auto q : pidls) ILFree(q);
    return obj;
}

void startFileDrag(Dispatcher& dispatcher, std::vector<std::string> paths, std::function<void(const DragResult&)> done) {
    dispatcher.post([paths = std::move(paths), done = std::move(done)] {
        IDataObject* data = createShellDataObject(paths);
        DragResult r = runDrag(data, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
        if (data) data->Release();
        done(r);
    });
}

}  // namespace st::ui::dnd
