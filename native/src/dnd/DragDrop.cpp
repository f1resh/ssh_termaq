#include "dnd/DragDrop.h"

#include "core/Log.h"
#include "core/Strings.h"
#include "core/Threading.h"
#include "core/Win.h"

#include <objbase.h>
#include <ole2.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

namespace st::dnd {

namespace {

HWND g_window = nullptr;
std::once_flag g_mtaOnce;
CO_MTA_USAGE_COOKIE g_mtaCookie = nullptr;

void ensureMta() {
    // Objects created in the MTA must outlive the short-lived thread that creates them.
    std::call_once(g_mtaOnce, [] { CoIncrementMTAUsage(&g_mtaCookie); });
}

FILETIME unixToFileTime(int64_t seconds) {
    ULARGE_INTEGER u;
    u.QuadPart = static_cast<ULONGLONG>(seconds * 10000000LL + 116444736000000000LL);
    FILETIME ft;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    return ft;
}

CLIPFORMAT clipFormat(const wchar_t* name) { return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(name)); }

HGLOBAL duplicateGlobal(HGLOBAL src) {
    const SIZE_T size = GlobalSize(src);
    HGLOBAL dst = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!dst) return nullptr;
    void* from = GlobalLock(src);
    void* to = GlobalLock(dst);
    if (from && to) std::memcpy(to, from, size);
    if (to) GlobalUnlock(dst);
    if (from) GlobalUnlock(src);
    return dst;
}

std::wstring baseNameW(const std::string& rel) {
    const size_t slash = rel.find_last_of('/');
    return widen(slash == std::string::npos ? rel : rel.substr(slash + 1));
}

/* ------------------------------------------------------------------------ */
/* Stream over one virtual file                                              */
/* ------------------------------------------------------------------------ */

class VirtualStream final : public IStream {
public:
    VirtualStream(std::shared_ptr<VirtualSource> src, size_t index, VirtualItem item)
        : src_(std::move(src)), index_(index), item_(std::move(item)) {}

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IStream || riid == IID_ISequentialStream) {
            *ppv = static_cast<IStream*>(this);
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

    // ISequentialStream
    HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* pcbRead) override {
        std::lock_guard lk(m_);
        ULONG total = 0;
        try {
            while (total < cb && !eof_) {
                if (!reader_) reader_ = src_->open(index_);
                const size_t n = reader_->read(static_cast<char*>(pv) + total, cb - total);
                if (n == 0) {
                    eof_ = true;
                    reader_.reset();
                    break;
                }
                total += static_cast<ULONG>(n);
                pos_ += n;
            }
        } catch (const std::exception& e) {
            applog::warn("drag: reading '" + item_.relPath + "' failed: " + e.what());
            if (pcbRead) *pcbRead = total;
            return STG_E_READFAULT;
        }
        if (pcbRead) *pcbRead = total;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }

    // IStream
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* newPos) override {
        std::lock_guard lk(m_);
        int64_t target = 0;
        switch (origin) {
            case STREAM_SEEK_SET: target = move.QuadPart; break;
            case STREAM_SEEK_CUR: target = static_cast<int64_t>(pos_) + move.QuadPart; break;
            case STREAM_SEEK_END: target = static_cast<int64_t>(item_.size) + move.QuadPart; break;
            default: return STG_E_INVALIDFUNCTION;
        }
        if (target != static_cast<int64_t>(pos_)) {
            // Sequential source: only "rewind" (reopen) and "jump to the end" are possible.
            if (target == 0) {
                reader_.reset();
                eof_ = false;
                pos_ = 0;
            } else if (target == static_cast<int64_t>(item_.size)) {
                reader_.reset();
                eof_ = true;
                pos_ = item_.size;
            } else {
                return STG_E_INVALIDFUNCTION;
            }
        }
        if (newPos) newPos->QuadPart = pos_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* dst, ULARGE_INTEGER cb, ULARGE_INTEGER* read,
                                     ULARGE_INTEGER* written) override {
        if (!dst) return STG_E_INVALIDPOINTER;
        std::vector<char> buf(64 * 1024);
        ULONGLONG left = cb.QuadPart, totalRead = 0, totalWritten = 0;
        HRESULT hr = S_OK;
        while (left > 0) {
            ULONG n = 0;
            hr = Read(buf.data(), static_cast<ULONG>(std::min<ULONGLONG>(left, buf.size())), &n);
            if (FAILED(hr) || n == 0) break;
            totalRead += n;
            ULONG w = 0;
            hr = dst->Write(buf.data(), n, &w);
            totalWritten += w;
            if (FAILED(hr)) break;
            left -= n;
        }
        if (read) read->QuadPart = totalRead;
        if (written) written->QuadPart = totalWritten;
        return FAILED(hr) ? hr : S_OK;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* st, DWORD flag) override {
        if (!st) return STG_E_INVALIDPOINTER;
        ZeroMemory(st, sizeof *st);
        st->type = STGTY_STREAM;
        st->cbSize.QuadPart = item_.size;
        st->grfMode = STGM_READ;
        if (item_.mtime > 0) st->mtime = unixToFileTime(item_.mtime);
        if (!(flag & STATFLAG_NONAME)) {
            const std::wstring name = baseNameW(item_.relPath);
            const size_t bytes = (name.size() + 1) * sizeof(wchar_t);
            st->pwcsName = static_cast<LPOLESTR>(CoTaskMemAlloc(bytes));
            if (!st->pwcsName) return E_OUTOFMEMORY;
            std::memcpy(st->pwcsName, name.c_str(), bytes);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }

private:
    ~VirtualStream() = default;
    LONG ref_ = 1;
    std::mutex m_;
    std::shared_ptr<VirtualSource> src_;
    size_t index_;
    VirtualItem item_;
    std::unique_ptr<VirtualReader> reader_;
    uint64_t pos_ = 0;
    bool eof_ = false;
};

/* ------------------------------------------------------------------------ */
/* Virtual data object                                                       */
/* ------------------------------------------------------------------------ */

class VirtualDataObject final : public IDataObject, public IDataObjectAsyncCapability {
public:
    VirtualDataObject(std::shared_ptr<VirtualSource> src, bool progressUi) : src_(std::move(src)), progressUi_(progressUi) {
        cfDescriptor_ = clipFormat(CFSTR_FILEDESCRIPTORW);
        cfContents_ = clipFormat(CFSTR_FILECONTENTS);
        cfPreferred_ = clipFormat(CFSTR_PREFERREDDROPEFFECT);
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *ppv = static_cast<IDataObject*>(this);
        } else if (riid == IID_IDataObjectAsyncCapability) {
            *ppv = static_cast<IDataObjectAsyncCapability*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    // IDataObject
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* fe, STGMEDIUM* med) override {
        if (!fe || !med) return E_INVALIDARG;
        ZeroMemory(med, sizeof *med);
        if (fe->cfFormat == cfDescriptor_ && (fe->tymed & TYMED_HGLOBAL)) return descriptor(med);
        if (fe->cfFormat == cfContents_ && (fe->tymed & TYMED_ISTREAM)) return contents(fe->lindex, med);
        if (fe->cfFormat == cfPreferred_ && (fe->tymed & TYMED_HGLOBAL)) {
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
            if (!h) return E_OUTOFMEMORY;
            *static_cast<DWORD*>(GlobalLock(h)) = DROPEFFECT_COPY;
            GlobalUnlock(h);
            med->tymed = TYMED_HGLOBAL;
            med->hGlobal = h;
            return S_OK;
        }
        std::lock_guard lk(m_);
        for (auto& s : stored_) {
            if (s.fe.cfFormat != fe->cfFormat || !(s.fe.tymed & fe->tymed)) continue;
            return copyMedium(s.med, med);
        }
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* fe) override {
        if (!fe) return E_INVALIDARG;
        if ((fe->cfFormat == cfDescriptor_ || fe->cfFormat == cfPreferred_) && (fe->tymed & TYMED_HGLOBAL)) return S_OK;
        if (fe->cfFormat == cfContents_ && (fe->tymed & TYMED_ISTREAM)) return S_OK;
        std::lock_guard lk(m_);
        for (auto& s : stored_)
            if (s.fe.cfFormat == fe->cfFormat && (s.fe.tymed & fe->tymed)) return S_OK;
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (out) out->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* fe, STGMEDIUM* med, BOOL release) override {
        if (!fe || !med) return E_INVALIDARG;
        Stored s;
        s.fe = *fe;
        s.fe.ptd = nullptr;
        if (release) {
            s.med = *med;
        } else {
            const HRESULT hr = copyMedium(*med, &s.med);
            if (FAILED(hr)) return hr;
        }
        std::lock_guard lk(m_);
        for (auto it = stored_.begin(); it != stored_.end(); ++it) {
            if (it->fe.cfFormat == fe->cfFormat) {
                ReleaseStgMedium(&it->med);
                stored_.erase(it);
                break;
            }
        }
        stored_.push_back(s);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dir, IEnumFORMATETC** out) override {
        if (!out) return E_POINTER;
        if (dir != DATADIR_GET) return E_NOTIMPL;
        std::vector<FORMATETC> formats = {
            {cfDescriptor_, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
            {cfContents_, nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM},
            {cfPreferred_, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
        };
        {
            std::lock_guard lk(m_);
            for (auto& s : stored_) formats.push_back(s.fe);
        }
        return SHCreateStdEnumFmtEtc(static_cast<UINT>(formats.size()), formats.data(), out);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

    // IDataObjectAsyncCapability: lets Explorer extract in the background.
    HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL async) override {
        async_ = async;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL* async) override {
        if (!async) return E_POINTER;
        *async = async_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx*) override {
        inOperation_ = TRUE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE InOperation(BOOL* in) override {
        if (!in) return E_POINTER;
        *in = inOperation_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EndOperation(HRESULT, IBindCtx*, DWORD) override {
        inOperation_ = FALSE;
        return S_OK;
    }

private:
    struct Stored {
        FORMATETC fe{};
        STGMEDIUM med{};
    };

    ~VirtualDataObject() {
        for (auto& s : stored_) ReleaseStgMedium(&s.med);
    }

    static HRESULT copyMedium(const STGMEDIUM& from, STGMEDIUM* to) {
        ZeroMemory(to, sizeof *to);
        switch (from.tymed) {
            case TYMED_HGLOBAL:
                to->hGlobal = duplicateGlobal(from.hGlobal);
                if (!to->hGlobal) return E_OUTOFMEMORY;
                break;
            case TYMED_ISTREAM:
                to->pstm = from.pstm;
                if (to->pstm) to->pstm->AddRef();
                break;
            default: return DV_E_TYMED;
        }
        to->tymed = from.tymed;
        return S_OK;
    }

    /** Lazily enumerated items (the first request may block on the network). */
    bool items(std::vector<VirtualItem>& out) {
        std::lock_guard lk(enumMutex_);
        if (!enumerated_) {
            try {
                items_ = src_->enumerate();
            } catch (const std::exception& e) {
                applog::warn(std::string("drag: listing failed: ") + e.what());
                return false;
            }
            enumerated_ = true;
        }
        out = items_;
        return true;
    }

    HRESULT descriptor(STGMEDIUM* med) {
        std::vector<VirtualItem> list;
        if (!items(list)) return E_FAIL;
        const size_t bytes = sizeof(FILEGROUPDESCRIPTORW) + (list.empty() ? 0 : list.size() - 1) * sizeof(FILEDESCRIPTORW);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
        if (!h) return E_OUTOFMEMORY;
        auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(h));
        group->cItems = static_cast<UINT>(list.size());
        for (size_t i = 0; i < list.size(); ++i) {
            FILEDESCRIPTORW& d = group->fgd[i];
            const VirtualItem& item = list[i];
            d.dwFlags = FD_ATTRIBUTES | FD_FILESIZE | (progressUi_ ? FD_PROGRESSUI : 0) | FD_UNICODE;
            d.dwFileAttributes = item.dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            d.nFileSizeHigh = static_cast<DWORD>(item.size >> 32);
            d.nFileSizeLow = static_cast<DWORD>(item.size & 0xffffffffu);
            if (item.mtime > 0) {
                d.dwFlags |= FD_WRITESTIME;
                d.ftLastWriteTime = unixToFileTime(item.mtime);
            }
            std::wstring name = widen(item.relPath);
            for (auto& c : name)
                if (c == L'/') c = L'\\';
            wcsncpy_s(d.cFileName, name.c_str(), _TRUNCATE);
        }
        GlobalUnlock(h);
        med->tymed = TYMED_HGLOBAL;
        med->hGlobal = h;
        return S_OK;
    }

    HRESULT contents(LONG index, STGMEDIUM* med) {
        std::vector<VirtualItem> list;
        if (!items(list)) return E_FAIL;
        if (index < 0 || static_cast<size_t>(index) >= list.size() || list[static_cast<size_t>(index)].dir) return DV_E_LINDEX;
        med->tymed = TYMED_ISTREAM;
        med->pstm = new VirtualStream(src_, static_cast<size_t>(index), list[static_cast<size_t>(index)]);
        return S_OK;
    }

    LONG ref_ = 1;
    std::shared_ptr<VirtualSource> src_;
    bool progressUi_;
    CLIPFORMAT cfDescriptor_ = 0, cfContents_ = 0, cfPreferred_ = 0;
    std::mutex enumMutex_;
    bool enumerated_ = false;
    std::vector<VirtualItem> items_;
    std::mutex m_;
    std::vector<Stored> stored_;
    std::atomic<BOOL> async_{TRUE};
    std::atomic<BOOL> inOperation_{FALSE};
};

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

void init(void* mainWindow) {
    g_window = static_cast<HWND>(mainWindow);
    if (g_window) ensureMta();
}

IDataObject* createVirtualDataObject(std::shared_ptr<VirtualSource> source, bool progressUi) {
    return new VirtualDataObject(std::move(source), progressUi);
}

IDataObject* createVirtualDataObjectInMta(std::shared_ptr<VirtualSource> source, bool progressUi) {
    ensureMta();
    IStream* marshaled = nullptr;
    HRESULT hr = E_FAIL;
    std::thread([&] {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IDataObject* obj = createVirtualDataObject(source, progressUi);
        hr = CoMarshalInterThreadInterfaceInStream(IID_IDataObject, obj, &marshaled);
        obj->Release();
        if (SUCCEEDED(init)) CoUninitialize();
    }).join();
    if (FAILED(hr) || !marshaled) return nullptr;
    IDataObject* proxy = nullptr;
    if (FAILED(CoGetInterfaceAndReleaseStream(marshaled, IID_IDataObject, reinterpret_cast<void**>(&proxy)))) return nullptr;
    return proxy;
}

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

void startVirtualDrag(Dispatcher& dispatcher, std::shared_ptr<VirtualSource> source,
                      std::function<void(const DragResult&)> done) {
    dispatcher.post([source = std::move(source), done = std::move(done)] {
        IDataObject* data = createVirtualDataObjectInMta(source, true);
        DragResult r = runDrag(data, DROPEFFECT_COPY);
        if (data) data->Release();
        done(r);
    });
}

void startFileDrag(Dispatcher& dispatcher, std::vector<std::string> paths, std::function<void(const DragResult&)> done) {
    dispatcher.post([paths = std::move(paths), done = std::move(done)] {
        IDataObject* data = createShellDataObject(paths);
        DragResult r = runDrag(data, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
        if (data) data->Release();
        done(r);
    });
}

}  // namespace st::dnd
