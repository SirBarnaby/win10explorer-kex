// Native Explorer tabs for Windows 10 22H2 x64.
// Copyright (c) 2026 Win10NativePatch contributors.
// COM/site and Shell view access patterns adapted from StExBar:
// Copyright (C) 2007-2022 Stefan Kueng. See vendor/stexbar/PROVENANCE.txt.
// This program is free software under GPL version 2 or, at your option, later.
// Distributed WITHOUT ANY WARRANTY. See vendor/stexbar/LICENSE.

#include "Patch.h"
#include "MvpHostProtocol.h"
#include <algorithm>
#include <atomic>
#include <commctrl.h>
#include <cstring>
#include <deque>
#include <dwmapi.h>
#include <exdisp.h>
#include <exdispid.h>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <oleacc.h>
#include <optional>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <uxtheme.h>
#include <windowsx.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
namespace
{
HINSTANCE module{};
std::atomic<long> objectCount{}, serverLocks{};
constexpr wchar_t TabClass[] = L"Win10NativePatch.ExplorerTabs.v2";
constexpr wchar_t ControllerProperty[] = L"Win10NativePatch.ExplorerController.v2";
constexpr wchar_t ProofProperty[] = L"Win10NativePatch.IsolatedProof.v2";
constexpr wchar_t SessionKey[] = L"Software\\Win10NativePatch\\v2\\ExplorerTabs";
constexpr UINT_PTR MaintenanceTimer = 0x57545032;
constexpr UINT SyncMessage = WM_APP + 0x482;
constexpr UINT RestoreMessage = WM_APP + 0x483;
constexpr UINT IconMessage = WM_APP + 0x484;
constexpr size_t MaxTabs = 64, MaxHistory = 64, MaxSelection = 512, MaxPacket = 8 * 1024 * 1024;
const CLSID TabsClsid = {0x73f7b7dc, 0xc022, 0x4862, {0xa3, 0xea, 0x7b, 0xd0, 0x4e, 0xae, 0x73, 0xa9}};
std::atomic<ULONGLONG> nextId{1};
std::atomic<bool> sessionRestored{false};
class Controller;
std::mutex controllersMutex;
std::unordered_map<HWND, Controller *> controllers;

template <class T> HRESULT Query(IUnknown *p, ComPtr<T> &out)
{
    out.Reset();
    return p ? p->QueryInterface(IID_PPV_ARGS(out.ReleaseAndGetAddressOf())) : E_NOINTERFACE;
}
struct Pidl
{
    PIDLIST_ABSOLUTE p{};
    Pidl() = default;
    explicit Pidl(PCIDLIST_ABSOLUTE v) : p(v ? ILCloneFull(v) : nullptr)
    {
    }
    static Pidl Adopt(PIDLIST_ABSOLUTE v)
    {
        Pidl r;
        r.p = v;
        return r;
    }
    Pidl(const Pidl &v) : Pidl(v.p)
    {
    }
    Pidl(Pidl &&v) noexcept : p(std::exchange(v.p, nullptr))
    {
    }
    Pidl &operator=(Pidl v) noexcept
    {
        std::swap(p, v.p);
        return *this;
    }
    ~Pidl()
    {
        CoTaskMemFree(p);
    }
    explicit operator bool() const
    {
        return p != nullptr;
    }
};
bool Equal(const Pidl &a, const Pidl &b)
{
    return a.p && b.p && ILIsEqual(a.p, b.p);
}
// Shell namespace providers are allowed to perform I/O. Only the metadata STA
// calls these APIs; the Explorer UI thread never waits for tab names or icons.
std::wstring ShellDisplayName(const Pidl &p)
{
    PWSTR s{};
    std::wstring result;
    if (p && SUCCEEDED(SHGetNameFromIDList(p.p, SIGDN_NORMALDISPLAY, &s)))
    {
        result = s;
        CoTaskMemFree(s);
    }
    return result.empty() ? L"Folder" : result;
}
struct Icon
{
    HICON handle{};
    explicit Icon(HICON value) : handle(value) {}
    ~Icon() { if (handle) DestroyIcon(handle); }
    Icon(const Icon &) = delete;
    Icon &operator=(const Icon &) = delete;
};
struct MetadataJob
{
    ULONGLONG cookie{}, tab{}, generation{};
    Pidl folder;
};
struct MetadataResult
{
    MetadataJob job;
    std::wstring name;
    std::shared_ptr<Icon> icon;
};
constexpr size_t MaxMetadata = 64;
std::mutex metadataMutex;
std::deque<MetadataJob> metadataJobs;
std::unordered_map<ULONGLONG, HWND> metadataTargets;
std::unordered_map<ULONGLONG, MetadataResult> metadataResults;
size_t metadataOutstanding{};
bool metadataWorkerRunning{};

// Results travel as integer tickets, never as pointers in a window message.
// Removing a target also destroys its undelivered results, even if the HWND is
// subsequently reused. A blocked provider can delay only this worker; it never
// holds a controller, an Explorer interface, or the UI thread alive.
void CancelMetadata(ULONGLONG cookie)
{
    std::lock_guard lock(metadataMutex);
    metadataTargets.erase(cookie);
    for (auto i = metadataJobs.begin(); i != metadataJobs.end();)
        if (i->cookie == cookie)
        {
            i = metadataJobs.erase(i);
            --metadataOutstanding;
        }
        else ++i;
    for (auto i = metadataResults.begin(); i != metadataResults.end();)
        if (i->second.job.cookie == cookie)
        {
            i = metadataResults.erase(i);
            --metadataOutstanding;
        }
        else ++i;
}
void RegisterMetadata(ULONGLONG cookie, HWND target)
{
    std::lock_guard lock(metadataMutex);
    metadataTargets[cookie] = target;
}
DWORD WINAPI MetadataThread(void *context)
{
    HMODULE heldModule = static_cast<HMODULE>(context);
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (;;)
    {
        std::optional<MetadataJob> job;
        {
            std::lock_guard lock(metadataMutex);
            if (metadataJobs.empty())
            {
                metadataWorkerRunning = false;
                break;
            }
            job = std::move(metadataJobs.front());
            metadataJobs.pop_front();
        }
        MetadataResult result;
        result.job = std::move(*job);
        try
        {
            if (SUCCEEDED(initialized))
            {
                result.name = ShellDisplayName(result.job.folder);
                SHFILEINFOW info{};
                if (SHGetFileInfoW(reinterpret_cast<PCWSTR>(result.job.folder.p), 0, &info, sizeof(info),
                                   SHGFI_PIDL | SHGFI_ICON | SHGFI_SMALLICON))
                {
                    HICON owned = info.hIcon;
                    try { result.icon = std::make_shared<Icon>(owned); }
                    catch (...) { if (owned) DestroyIcon(owned); throw; }
                }
            }
        }
        catch (...)
        {
            // A metadata allocation failure must not escape a native thread.
            result.name.clear();
            result.icon.reset();
        }
        {
            std::lock_guard lock(metadataMutex);
            auto target = metadataTargets.find(result.job.cookie);
            if (target == metadataTargets.end())
                --metadataOutstanding;
            else
            {
                ULONGLONG ticket = nextId++;
                try
                {
                    auto inserted = metadataResults.emplace(ticket, std::move(result));
                    if (!PostMessageW(target->second, IconMessage, static_cast<WPARAM>(ticket),
                                      static_cast<LPARAM>(inserted.first->second.job.cookie)))
                    {
                        metadataResults.erase(inserted.first);
                        --metadataOutstanding;
                    }
                }
                catch (...) { --metadataOutstanding; }
            }
            // Capacity became available. Wake other frames that could not
            // enqueue earlier; this is driven by completed work, never a timer.
            for(const auto& [cookie,window]:metadataTargets)
                if(cookie!=result.job.cookie)PostMessageW(window,IconMessage,0,static_cast<LPARAM>(cookie));
        }
        // An STA must dispatch messages between provider calls. No application
        // window is created and no Explorer COM interface crosses apartments.
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    --objectCount;
    FreeLibraryAndExitThread(heldModule, 0);
}
bool QueueMetadata(MetadataJob job)
{
    std::lock_guard lock(metadataMutex);
    if (!metadataTargets.contains(job.cookie)) return false;
    // Replace a queued old navigation for the same tab. A currently executing
    // provider is left alone and its generation will fail the delivery check.
    for (auto i = metadataJobs.begin(); i != metadataJobs.end();)
        if (i->cookie == job.cookie && i->tab == job.tab)
        {
            i = metadataJobs.erase(i);
            --metadataOutstanding;
        }
        else ++i;
    if (metadataOutstanding >= MaxMetadata) return false;
    try { metadataJobs.push_back(std::move(job)); }
    catch (...) { return false; }
    ++metadataOutstanding;
    if (!metadataWorkerRunning)
    {
        HMODULE held{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                reinterpret_cast<PCWSTR>(&MetadataThread), &held))
        {
            metadataJobs.pop_back();
            --metadataOutstanding;
            return false;
        }
        ++objectCount;
        metadataWorkerRunning = true;
        HANDLE thread = CreateThread(nullptr, 0, MetadataThread, held, 0, nullptr);
        if (!thread)
        {
            metadataWorkerRunning = false;
            --objectCount;
            FreeLibrary(held);
            metadataJobs.pop_back();
            --metadataOutstanding;
            return false;
        }
        CloseHandle(thread);
    }
    return true;
}
std::optional<MetadataResult> TakeMetadata(ULONGLONG ticket, ULONGLONG cookie)
{
    std::lock_guard lock(metadataMutex);
    auto i = metadataResults.find(ticket);
    if (i == metadataResults.end() || i->second.job.cookie != cookie) return {};
    auto result = std::move(i->second);
    metadataResults.erase(i);
    --metadataOutstanding;
    return result;
}
Pidl CurrentFolder(IShellBrowser *browser)
{
    ComPtr<IShellView> view;
    ComPtr<IFolderView> fv;
    ComPtr<IPersistFolder2> folder;
    PIDLIST_ABSOLUTE id{};
    if (browser && SUCCEEDED(browser->QueryActiveShellView(&view)) && SUCCEEDED(view.As(&fv)) &&
        SUCCEEDED(fv->GetFolder(IID_PPV_ARGS(&folder))) && SUCCEEDED(folder->GetCurFolder(&id)))
        return Pidl::Adopt(id);
    return {};
}
struct ViewState
{
    Pidl folder, focused, anchor;
    std::vector<Pidl> selected;
    FOLDERVIEWMODE mode = FVM_AUTO;
    int iconSize{};
    std::vector<SORTCOLUMN> sort;
    PROPERTYKEY group{};
    BOOL groupAscending = TRUE;
    bool hasGroup = false;
    std::wstring label;
};
struct Tab
{
    ULONGLONG id = nextId++;
    std::vector<ViewState> history;
    size_t position{};
    std::wstring title;
    HWND sourceFrame{};
    ULONGLONG sourceId{};
    DWORD sourceProcess{};
    std::shared_ptr<Icon> icon;
    Pidl metadataExpected, metadataLoaded;
    ULONGLONG metadataGeneration{};
    bool metadataPending = false;
};
const std::wstring &CachedName(const ViewState &state)
{
    static const std::wstring fallback = L"Folder";
    return state.label.empty() ? fallback : state.label;
}
ViewState Capture(IShellBrowser *browser)
{
    ViewState s;
    s.folder = CurrentFolder(browser);
    ComPtr<IShellView> view;
    ComPtr<IFolderView2> fv;
    if (!browser || FAILED(browser->QueryActiveShellView(&view)) || FAILED(view.As(&fv)))
        return s;
    fv->GetViewModeAndIconSize(&s.mode, &s.iconSize);
    int focused = -1;
    if (SUCCEEDED(fv->GetFocusedItem(&focused)) && focused >= 0)
    {
        PITEMID_CHILD p{};
        if (SUCCEEDED(fv->Item(focused, &p)))
            s.focused = Pidl::Adopt(p);
    }
    int visible = -1;
    if (SUCCEEDED(fv->GetVisibleItem(-1, FALSE, &visible)) && visible >= 0)
    {
        PITEMID_CHILD p{};
        if (SUCCEEDED(fv->Item(visible, &p)))
            s.anchor = Pidl::Adopt(p);
    }
    ComPtr<IEnumIDList> items;
    if (SUCCEEDED(fv->Items(SVGIO_SELECTION, IID_PPV_ARGS(&items))))
    {
        size_t bytes = 0;
        for (size_t n = 0; n < MaxSelection; n++)
        {
            PITEMID_CHILD p{};
            if (items->Next(1, &p, nullptr) != S_OK)
                break;
            bytes += ILGetSize(p);
            if (bytes > 256 * 1024)
            {
                CoTaskMemFree(p);
                break;
            }
            s.selected.push_back(Pidl::Adopt(p));
        }
    }
    int columns{};
    if (SUCCEEDED(fv->GetSortColumnCount(&columns)) && columns > 0 && columns <= 32)
    {
        s.sort.resize(columns);
        if (FAILED(fv->GetSortColumns(s.sort.data(), columns)))
            s.sort.clear();
    }
    s.hasGroup = SUCCEEDED(fv->GetGroupBy(&s.group, &s.groupAscending));
    return s;
}
void Restore(IShellBrowser *browser, const ViewState &s)
{
    ComPtr<IShellView> view;
    ComPtr<IFolderView2> fv;
    if (!browser || !Equal(CurrentFolder(browser), s.folder) || FAILED(browser->QueryActiveShellView(&view)) ||
        FAILED(view.As(&fv)))
        return;
    if (s.mode != FVM_AUTO && s.iconSize > 0)
        fv->SetViewModeAndIconSize(s.mode, s.iconSize);
    if (!s.sort.empty())
        fv->SetSortColumns(s.sort.data(), static_cast<int>(s.sort.size()));
    if (s.hasGroup)
        fv->SetGroupBy(s.group, s.groupAscending);
    // IFolderView2 does not expose a pixel scroll setter. Restore a visible item
    // anchor without forcing the native DirectUI view into a legacy ListView.
    if (s.anchor)
        view->SelectItem(s.anchor.p, SVSI_ENSUREVISIBLE);
    bool first = true;
    for (const auto &p : s.selected)
    {
        view->SelectItem(p.p, SVSI_SELECT | (first ? SVSI_DESELECTOTHERS : 0));
        first = false;
    }
    if (s.focused)
        view->SelectItem(s.focused.p, SVSI_FOCUSED | (s.selected.empty() ? SVSI_DESELECTOTHERS : 0));
}

struct Writer
{
    std::vector<BYTE> bytes;
    bool ok = true;
    template <class T> void number(T v)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        if (!ok || bytes.size() + sizeof(v) > MaxPacket)
        {
            ok = false;
            return;
        }
        const auto *p = reinterpret_cast<const BYTE *>(&v);
        bytes.insert(bytes.end(), p, p + sizeof(v));
    }
    void pidl(const Pidl &p)
    {
        DWORD n = p ? ILGetSize(p.p) : 0;
        number(n);
        if (n)
        {
            if (!ok || bytes.size() + n > MaxPacket)
            {
                ok = false;
                return;
            }
            const auto *b = reinterpret_cast<const BYTE *>(p.p);
            bytes.insert(bytes.end(), b, b + n);
        }
    }
    void state(const ViewState &s)
    {
        pidl(s.folder);
        pidl(s.focused);
        pidl(s.anchor);
        number(static_cast<int>(s.mode));
        number(s.iconSize);
        number(static_cast<DWORD>(s.selected.size()));
        for (auto &p : s.selected)
            pidl(p);
        number(static_cast<DWORD>(s.sort.size()));
        for (auto &v : s.sort)
            number(v);
        number(s.group);
        number(s.groupAscending);
        number(static_cast<DWORD>(s.hasGroup));
    }
    void tab(const Tab &t)
    {
        number(static_cast<DWORD>(t.history.size()));
        number(static_cast<DWORD>(t.position));
        for (auto &s : t.history)
            state(s);
    }
};
struct Reader
{
    const BYTE *p;
    size_t left;
    bool ok = true;
    template <class T> T number()
    {
        T v{};
        if (left < sizeof(v))
        {
            ok = false;
            return v;
        }
        memcpy(&v, p, sizeof(v));
        p += sizeof(v);
        left -= sizeof(v);
        return v;
    }
    Pidl pidl()
    {
        DWORD n = number<DWORD>();
        if (!n)
            return {};
        if (!ok || n < 2 || n > 65536 || n > left)
        {
            ok = false;
            return {};
        }
        size_t i = 0;
        while (i + 2 <= n)
        {
            USHORT cb{};
            memcpy(&cb, p + i, 2);
            if (cb == 0)
            {
                if (i + 2 != n)
                    ok = false;
                break;
            }
            if (cb < 2 || cb > n - i)
            {
                ok = false;
                break;
            }
            i += cb;
        }
        if (i + 2 != n)
            ok = false;
        if (!ok)
            return {};
        auto *id = static_cast<PIDLIST_ABSOLUTE>(CoTaskMemAlloc(n));
        if (!id)
        {
            ok = false;
            return {};
        }
        memcpy(id, p, n);
        p += n;
        left -= n;
        return Pidl::Adopt(id);
    }
    ViewState state()
    {
        ViewState s;
        s.folder = pidl();
        s.focused = pidl();
        s.anchor = pidl();
        int mode = number<int>();
        s.mode = static_cast<FOLDERVIEWMODE>(mode);
        s.iconSize = number<int>();
        DWORD n = number<DWORD>();
        if (n > MaxSelection)
        {
            ok = false;
            return s;
        }
        for (DWORD i = 0; i < n && ok; i++)
            s.selected.push_back(pidl());
        n = number<DWORD>();
        if (n > 32)
        {
            ok = false;
            return s;
        }
        for (DWORD i = 0; i < n && ok; i++)
            s.sort.push_back(number<SORTCOLUMN>());
        s.group = number<PROPERTYKEY>();
        s.groupAscending = number<BOOL>();
        s.hasGroup = number<DWORD>() != 0;
        if (!s.folder || mode < FVM_AUTO || mode > FVM_CONTENT || s.iconSize < 0 || s.iconSize > 1024)
            ok = false;
        return s;
    }
    Tab tab()
    {
        Tab t;
        DWORD n = number<DWORD>(), position = number<DWORD>();
        if (n == 0 || n > MaxHistory || position >= n)
        {
            ok = false;
            return t;
        }
        t.position = position;
        for (DWORD i = 0; i < n && ok; i++)
            t.history.push_back(state());
        if (ok)
            t.title = CachedName(t.history[t.position]);
        return t;
    }
};
constexpr DWORD PacketMagic = 0x32544257;
UINT TabFormat()
{
    static UINT v = RegisterClipboardFormatW(L"Win10NativePatch.ExplorerTab.v2");
    return v;
}
UINT TransferMessage()
{
    static UINT v = RegisterWindowMessageW(L"Win10NativePatch.ExplorerTabTransfer.v2");
    return v;
}
std::vector<BYTE> Serialize(const Tab &t, HWND source = nullptr)
{
    Writer w;
    w.number(PacketMagic);
    w.number(DWORD{2});
    w.number(DWORD{0});
    w.number(static_cast<ULONGLONG>(reinterpret_cast<ULONG_PTR>(source)));
    w.number(t.id);
    w.number(GetCurrentProcessId());
    w.tab(t);
    if (!w.ok)
        return {};
    DWORD size = static_cast<DWORD>(w.bytes.size());
    memcpy(w.bytes.data() + 8, &size, 4);
    return std::move(w.bytes);
}
std::optional<Tab> Deserialize(const BYTE *bytes, size_t size)
{
    if (!bytes || size < 40 || size > MaxPacket)
        return {};
    Reader r{bytes, size};
    if (r.number<DWORD>() != PacketMagic || r.number<DWORD>() != 2)
        return {};
    DWORD declared = r.number<DWORD>();
    if (declared < 40 || declared > size)
        return {};
    r.left = declared - 12;
    auto frame = r.number<ULONGLONG>(), id = r.number<ULONGLONG>();
    auto process = r.number<DWORD>();
    Tab t = r.tab();
    if (!r.ok || r.left != 0)
        return {};
    t.sourceFrame = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(frame));
    t.sourceId = id;
    t.sourceProcess = process;
    return t;
}
bool ExplorerRoot(HWND h)
{
    wchar_t name[64]{};
    GetClassNameW(h, name, 64);
    return wcscmp(name, L"CabinetWClass") == 0;
}
bool LocalExplorer()
{
    wchar_t name[MAX_PATH]{};
    GetModuleFileNameW(nullptr, name, MAX_PATH);
    return _wcsicmp(PathFindFileNameW(name), L"explorer.exe") == 0;
}
std::wstring ModulePath()
{
    wchar_t path[32768]{};
    GetModuleFileNameW(module, path, 32768);
    return path;
}
bool Dark()
{
    HIGHCONTRASTW hc{sizeof(hc)};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON))
        return false;
    DWORD light = 1, size = 4;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

class DropData final : public IDataObject
{
    std::atomic<ULONG> refs{1};
    std::vector<BYTE> packet;

  public:
    explicit DropData(std::vector<BYTE> p) : packet(std::move(p))
    {
        objectCount++;
    }
    ~DropData()
    {
        objectCount--;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override
    {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (id == IID_IUnknown || id == IID_IDataObject)
            *out = static_cast<IDataObject *>(this);
        if (!*out)
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++refs;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG n = --refs;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC *f, STGMEDIUM *m) override
    {
        if (!f || !m)
            return E_POINTER;
        if (FAILED(QueryGetData(f)))
            return DV_E_FORMATETC;
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, packet.size());
        if (!h)
            return E_OUTOFMEMORY;
        void *p = GlobalLock(h);
        if (!p)
        {
            GlobalFree(h);
            return E_OUTOFMEMORY;
        }
        memcpy(p, packet.data(), packet.size());
        GlobalUnlock(h);
        *m = {};
        m->tymed = TYMED_HGLOBAL;
        m->hGlobal = h;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override
    {
        return DATA_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *f) override
    {
        return f && f->cfFormat == TabFormat() && (f->tymed & TYMED_HGLOBAL) && f->dwAspect == DVASPECT_CONTENT
                   ? S_OK
                   : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *out) override
    {
        if (out)
            out->ptd = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC *, STGMEDIUM *, BOOL) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC **out) override
    {
        if (direction != DATADIR_GET)
            return E_NOTIMPL;
        FORMATETC f{static_cast<CLIPFORMAT>(TabFormat()), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        return SHCreateStdEnumFmtEtc(1, &f, out);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }
};
class DropSource final : public IDropSource
{
    std::atomic<ULONG> refs{1};

  public:
    bool escaped = false, released = false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override
    {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (id == IID_IUnknown || id == IID_IDropSource)
            *out = static_cast<IDropSource *>(this);
        if (!*out)
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++refs;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG n = --refs;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override
    {
        if (escape)
        {
            escaped = true;
            return DRAGDROP_S_CANCEL;
        }
        if (!(keys & MK_LBUTTON))
        {
            released = true;
            return DRAGDROP_S_DROP;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override
    {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }
};

class Controller final : public IObjectWithSite, public IAccessible, public IDropTarget
{
    ULONGLONG metadataCookie_ = nextId++;
    std::atomic<ULONG> refs_{1};
    ComPtr<IUnknown> site_;
    ComPtr<IWebBrowser2> automation_;
    ComPtr<IShellBrowser> browser_;
    ComPtr<IConnectionPoint> events_;
    ComPtr<ITypeInfo> accessInfo_;
    DWORD eventCookie_{};
    HWND frame_{}, strip_{}, netui_{}, travel_{};
    HHOOK keyboard_{};
    HRGN oldRegion_{};
    bool hadRegion_ = false, clipped_ = false;
    DWORD ownerThread_{};
    bool enabled_ = false, tearingDown_ = false, inDrag_ = false, dropPrivate_ = false, sameWindowDrop_ = false;
    bool frameHooked_ = false, selfHosted_ = false;
    bool dark_ = false;
    HFONT font_{};
    HICON folderIcon_{};
    UINT dpi_ = 96;
    RECT tabArea_{};
    std::vector<RECT> tabRects_;
    RECT plusRect_{}, overflowRect_{};
    size_t firstVisible_{};
    int hot_ = -1, pressed_ = -1;
    bool pressedClose_ = false;
    POINT dragStart_{};
    int backCommand_ = -1, forwardCommand_ = -1;
    std::vector<Tab> tabs_, closed_;
    size_t active_{};
    std::optional<ViewState> restoreState_;
    unsigned restoreTries_{};
    struct Pending
    {
        ULONGLONG id{};
        size_t position{};
        ULONGLONG start{};
        bool changeTab{};
        Pidl expected;
    };
    std::optional<Pending> pending_;
    struct Detached
    {
        ULONGLONG id{};
        ULONGLONG start{};
        std::unordered_set<HWND> oldWindows;
        std::vector<BYTE> packet;
    };
    std::optional<Detached> detached_;
    ComPtr<IDropTarget> folderDrop_;
    ComPtr<IDataObject> dropObject_;
    int hoverDrop_ = -1;
    ULONGLONG hoverStart_{};
    bool startupPending_ = false;
    ULONGLONG startupStart_{};
    static LRESULT CALLBACK StripProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK FrameProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK TravelProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK KeyProc(int, WPARAM, LPARAM);
    bool Attach();
    void Detach();
    void Enable(bool);
    void Layout();
    void Paint();
    void RebuildFont();
    void RefreshTravel();
    void UpdateNavigationButtons();
    void Remember();
    void Synchronize();
    bool Navigate(ULONGLONG, size_t, bool);
    bool Activate(size_t);
    void NewTab(const Pidl *folder = nullptr);
    void Close(size_t);
    void Reopen();
    void History(int);
    void HistoryMenu(bool, POINT);
    void Menu(int, POINT);
    void LimitMemory();
    void SaveSession();
    void LoadSession();
    void TabChanged();
    void RefreshMetadata();
    void AcceptMetadata(ULONGLONG);
    void Timer();
    void StartDrag(size_t);
    void DetachTab(size_t);
    bool Import(Tab, bool);
    void SnapshotRoots(std::unordered_set<HWND> &);
    int Hit(POINT) const;
    bool HitClose(int, POINT) const;
    void Mouse(UINT, WPARAM, LPARAM);
    bool BeginStartupGuard();
    void CompleteStartupGuard();
    void ClipRibbon(bool);
    void BindFolderDrop();
    HRESULT ChildName(LONG, BSTR *);
    bool ChildRect(LONG, RECT &) const;
    HRESULT AccessTypeInfo();

  public:
    void OwnMvpLifetime() { selfHosted_ = true; }
    Controller()
    {
        objectCount++;
    }
    ~Controller()
    {
        CancelMetadata(metadataCookie_);
        Detach();
        if (font_)
            DeleteObject(font_);
        if (folderIcon_)
            DestroyIcon(folderIcon_);
        objectCount--;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override
    {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (id == IID_IUnknown || id == IID_IObjectWithSite)
            *out = static_cast<IObjectWithSite *>(this);
        else if (id == IID_IDispatch || id == IID_IAccessible || id == DIID_DWebBrowserEvents2)
            *out = static_cast<IAccessible *>(this);
        else if (id == IID_IDropTarget)
            *out = static_cast<IDropTarget *>(this);
        if (!*out)
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++refs_;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG n = --refs_;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown *) override;
    HRESULT STDMETHODCALLTYPE GetSite(REFIID id, void **out) override
    {
        return site_ ? site_->QueryInterface(id, out) : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT *n) override
    {
        if (!n)
            return E_POINTER;
        *n = SUCCEEDED(AccessTypeInfo()) ? 1 : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT index, LCID, ITypeInfo **out) override
    {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (index)
            return DISP_E_BADINDEX;
        HRESULT hr = AccessTypeInfo();
        return SUCCEEDED(hr) ? accessInfo_.CopyTo(out) : hr;
    }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR *names, UINT count, LCID, DISPID *ids) override
    {
        HRESULT hr = AccessTypeInfo();
        return SUCCEEDED(hr) ? DispGetIDsOfNames(accessInfo_.Get(), names, count, ids) : hr;
    }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS *, VARIANT *, EXCEPINFO *, UINT *) override;
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *, DWORD, POINTL, DWORD *) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD *) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject *, DWORD, POINTL, DWORD *) override;
    HRESULT STDMETHODCALLTYPE get_accParent(IDispatch **p) override
    {
        if (!p)
            return E_POINTER;
        *p = nullptr;
        return frame_ ? AccessibleObjectFromWindow(frame_, OBJID_WINDOW, IID_IDispatch, reinterpret_cast<void **>(p))
                      : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accChildCount(long *n) override
    {
        if (!n)
            return E_POINTER;
        *n = static_cast<LONG>(tabs_.size() + 2);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT, IDispatch **p) override
    {
        if (!p)
            return E_POINTER;
        *p = nullptr;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accName(VARIANT v, BSTR *p) override
    {
        return ChildName(v.vt == VT_I4 ? v.lVal : 0, p);
    }
    HRESULT STDMETHODCALLTYPE get_accValue(VARIANT, BSTR *p) override
    {
        if (!p)
            return E_POINTER;
        *p = nullptr;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT, BSTR *p) override
    {
        if (!p)
            return E_POINTER;
        *p = SysAllocString(L"Folders in this File Explorer window");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accRole(VARIANT v, VARIANT *p) override
    {
        if (!p)
            return E_POINTER;
        VariantInit(p);
        p->vt = VT_I4;
        p->lVal = v.vt == VT_I4 && v.lVal > 0
                      ? (v.lVal <= static_cast<LONG>(tabs_.size()) ? ROLE_SYSTEM_PAGETAB : ROLE_SYSTEM_PUSHBUTTON)
                      : ROLE_SYSTEM_PAGETABLIST;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accState(VARIANT, VARIANT *) override;
    HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT, BSTR *p) override
    {
        if (!p)
            return E_POINTER;
        *p = nullptr;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR *p, VARIANT, long *n) override
    {
        if (p)
            *p = nullptr;
        if (n)
            *n = 0;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT v, BSTR *p) override
    {
        if (!p)
            return E_POINTER;
        *p = SysAllocString(v.vt == VT_I4 && v.lVal <= 9 ? L"Ctrl+number" : L"Ctrl+Tab");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT *p) override
    {
        if (!p)
            return E_POINTER;
        VariantInit(p);
        if (GetFocus() == strip_)
        {
            p->vt = VT_I4;
            p->lVal = static_cast<LONG>(active_ + 1);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT *p) override
    {
        if (!p)
            return E_POINTER;
        VariantInit(p);
        p->vt = VT_I4;
        p->lVal = static_cast<LONG>(active_ + 1);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT v, BSTR *p) override
    {
        if (!p)
            return E_POINTER;
        *p = SysAllocString(v.vt == VT_I4 && v.lVal <= static_cast<LONG>(tabs_.size()) ? L"Switch" : L"Press");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accSelect(long, VARIANT v) override
    {
        return accDoDefaultAction(v);
    }
    HRESULT STDMETHODCALLTYPE accLocation(long *, long *, long *, long *, VARIANT) override;
    HRESULT STDMETHODCALLTYPE accNavigate(long, VARIANT, VARIANT *) override;
    HRESULT STDMETHODCALLTYPE accHitTest(long, long, VARIANT *) override;
    HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT) override;
    HRESULT STDMETHODCALLTYPE put_accName(VARIANT, BSTR) override
    {
        return E_ACCESSDENIED;
    }
    HRESULT STDMETHODCALLTYPE put_accValue(VARIANT, BSTR) override
    {
        return E_ACCESSDENIED;
    }
    HRESULT Proof(const wchar_t *);
};

HRESULT Controller::SetSite(IUnknown *site)
{
    if (frame_ && ownerThread_ != GetCurrentThreadId())
        return RPC_E_WRONG_THREAD;
    Detach();
    tearingDown_ = false;
    if (!site)
        return S_OK;
    if (!patch::SupportedWindows() || !LocalExplorer())
        return E_FAIL;
    site_ = site;
    if (FAILED(site->QueryInterface(IID_PPV_ARGS(&automation_))))
    {
        site_.Reset();
        return E_NOINTERFACE;
    }
    SHANDLE_PTR handle{};
    if (FAILED(automation_->get_HWND(&handle)))
    {
        site_.Reset();
        automation_.Reset();
        return E_FAIL;
    }
    frame_ = GetAncestor(reinterpret_cast<HWND>(handle), GA_ROOT);
    DWORD process{};
    ownerThread_ = GetWindowThreadProcessId(frame_, &process);
    if (!ExplorerRoot(frame_) || process != GetCurrentProcessId() || ownerThread_ != GetCurrentThreadId())
    {
        frame_ = nullptr;
        site_.Reset();
        automation_.Reset();
        return E_FAIL;
    }
    ComPtr<IServiceProvider> provider;
    if (FAILED(site->QueryInterface(IID_PPV_ARGS(&provider))) ||
        (FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser_))) &&
         FAILED(provider->QueryService(SID_SShellBrowser, IID_PPV_ARGS(&browser_)))))
    {
        Detach();
        return E_NOINTERFACE;
    }
    if (GetPropW(frame_, ControllerProperty))
    {
        Detach();
        return S_FALSE;
    }
    if (!SetWindowSubclass(frame_, FrameProc, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this)))
    {
        Detach();
        return HRESULT_FROM_WIN32(GetLastError());
    }
    frameHooked_ = true;
    if (!SetPropW(frame_, ControllerProperty, this))
    {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        Detach();
        return hr;
    }
    {
        std::lock_guard lock(controllersMutex);
        controllers[frame_] = this;
    }
    ComPtr<IConnectionPointContainer> container;
    if (SUCCEEDED(automation_.As(&container)) &&
        SUCCEEDED(container->FindConnectionPoint(DIID_DWebBrowserEvents2, &events_)))
        events_->Advise(static_cast<IAccessible *>(this), &eventCookie_);
    Tab first;
    auto state = Capture(browser_.Get());
    if (state.folder)
    {
        first.title = CachedName(state);
        first.history.push_back(std::move(state));
        tabs_.push_back(std::move(first));
    }
    Enable((patch::LoadConfig().flags & patch::ExplorerTabs) != 0);
    return S_OK;
}
bool Controller::Attach()
{
    return frame_ && browser_ && automation_;
}
void Controller::Detach()
{
    if (tearingDown_)
        return;
    tearingDown_ = true;
    CancelMetadata(metadataCookie_);
    if (frame_ && ownerThread_ != GetCurrentThreadId())
    {
        // COM apartment ownership is a hard boundary, not an invitation to touch
        // another window thread. SetSite(NULL) is expected on the owning STA.
        patch::Log(L"ExplorerTabs", L"Rejected teardown from a different apartment.");
        return;
    }
    if (!tabs_.empty())
        SaveSession();
    Enable(false);
    if (events_ && eventCookie_)
        events_->Unadvise(eventCookie_);
    eventCookie_ = 0;
    events_.Reset();
    if (frame_ && frameHooked_)
    {
        KillTimer(frame_, MaintenanceTimer);
        RemoveWindowSubclass(frame_, FrameProc, reinterpret_cast<UINT_PTR>(this));
        if (GetPropW(frame_, ControllerProperty) == this)
            RemovePropW(frame_, ControllerProperty);
        std::lock_guard lock(controllersMutex);
        auto i = controllers.find(frame_);
        if (i != controllers.end() && i->second == this)
            controllers.erase(i);
    }
    frameHooked_ = false;
    frame_ = nullptr;
    browser_.Reset();
    automation_.Reset();
    site_.Reset();
    tabs_.clear();
    closed_.clear();
}
bool Controller::BeginStartupGuard()
{
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SessionKey, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return false;
    DWORD prior = 0, failures = 0, size = 4;
    ULONGLONG then = 0;
    DWORD qsize = 8;
    RegGetValueW(key, nullptr, L"PendingProcess", RRF_RT_REG_DWORD, nullptr, &prior, &size);
    size = 4;
    RegGetValueW(key, nullptr, L"StartupFailures", RRF_RT_REG_DWORD, nullptr, &failures, &size);
    RegGetValueW(key, nullptr, L"PendingTime", RRF_RT_REG_QWORD, nullptr, &then, &qsize);
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER now{};
    now.LowPart = ft.dwLowDateTime;
    now.HighPart = ft.dwHighDateTime;
    if (prior && prior != GetCurrentProcessId())
    {
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, prior);
        bool dead = !process || WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
        if (process)
            CloseHandle(process);
        if (dead && now.QuadPart > then && now.QuadPart - then < 60ull * 10000000ull)
            failures++;
        else
            failures = 0;
    }
    if (failures >= 3)
    {
        RegSetValueExW(key, L"StartupFailures", 0, REG_DWORD, reinterpret_cast<BYTE *>(&failures), 4);
        RegCloseKey(key);
        patch::Log(L"ExplorerTabs",
                   L"Startup circuit breaker is open. Turn Explorer tabs off and on in Options to retry.");
        return false;
    }
    DWORD pid = GetCurrentProcessId();
    RegSetValueExW(key, L"PendingProcess", 0, REG_DWORD, reinterpret_cast<BYTE *>(&pid), 4);
    RegSetValueExW(key, L"PendingTime", 0, REG_QWORD, reinterpret_cast<BYTE *>(&now.QuadPart), 8);
    RegSetValueExW(key, L"StartupFailures", 0, REG_DWORD, reinterpret_cast<BYTE *>(&failures), 4);
    RegCloseKey(key);
    startupPending_ = true;
    startupStart_ = GetTickCount64();
    SetTimer(frame_, MaintenanceTimer, 100, nullptr);
    return true;
}
void Controller::CompleteStartupGuard()
{
    if (!frame_)
        return;
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SessionKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS)
    {
        DWORD zero = 0;
        RegDeleteValueW(key, L"PendingProcess");
        RegSetValueExW(key, L"StartupFailures", 0, REG_DWORD, reinterpret_cast<BYTE *>(&zero), 4);
        RegCloseKey(key);
    }
    startupPending_ = false;
}
void Controller::Enable(bool value)
{
    if (value == enabled_ || !frame_)
        return;
    if (value)
    {
        RemovePropW(frame_,L"Win10NativePatch.TabsRejected.v2");
        if (!BeginStartupGuard())
            return;
        if (tabs_.empty())
        {
            Tab t;
            auto s = Capture(browser_.Get());
            if (!s.folder)
                return;
            t.title = CachedName(s);
            t.history.push_back(std::move(s));
            tabs_.push_back(std::move(t));
            active_ = 0;
        }
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = StripProc;
        wc.hInstance = module;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = TabClass;
        wc.style = CS_DBLCLKS;
        RegisterClassExW(&wc);
        dark_ = Dark();
        dpi_ = GetDpiForWindow(frame_);
        RebuildFont();
        strip_ = CreateWindowExW(WS_EX_CONTROLPARENT, TabClass, L"File Explorer tabs",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS, 0, 0, 0, 0, frame_, nullptr,
                                 module, this);
        if (!strip_)
        {
            CompleteStartupGuard();
            return;
        }
        enabled_ = true;
        RegisterMetadata(metadataCookie_, frame_);
        SetPropW(frame_, patch::TabsActiveProperty, this);
        HRESULT drop = RegisterDragDrop(strip_, static_cast<IDropTarget *>(this));
        if (FAILED(drop))
            patch::Log(L"ExplorerTabs", L"OLE tab-strip drop target registration failed: " + std::to_wstring(drop));
        keyboard_ = SetWindowsHookExW(WH_GETMESSAGE, KeyProc, nullptr, ownerThread_);
        RefreshTravel();
        Layout();
        if(!enabled_||!strip_)return;
        if ((patch::LoadConfig().flags & patch::RestoreTabs) && !sessionRestored.exchange(true))
            LoadSession();
        RefreshMetadata();
        NotifyWinEvent(EVENT_OBJECT_CREATE, strip_, OBJID_CLIENT, CHILDID_SELF);
    }
    else
    {
        CancelMetadata(metadataCookie_);
        for(auto& t:tabs_)t.metadataPending=false;
        Remember();
        SaveSession();
        pending_.reset();
        detached_.reset();
        restoreState_.reset();
        if (keyboard_)
        {
            UnhookWindowsHookEx(keyboard_);
            keyboard_ = nullptr;
        }
        if (travel_)
        {
            RemoveWindowSubclass(travel_, TravelProc, reinterpret_cast<UINT_PTR>(this));
            travel_ = nullptr;
        }
        ClipRibbon(false);
        if (strip_)
        {
            RevokeDragDrop(strip_);
            DestroyWindow(strip_);
            strip_ = nullptr;
        }
        RemovePropW(frame_, patch::TabsActiveProperty);
        enabled_ = false;
        dropObject_.Reset();
        folderDrop_.Reset();
        if (startupPending_ || !(patch::LoadConfig().flags & patch::ExplorerTabs))
            CompleteStartupGuard();
        RedrawWindow(frame_, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
    }
}
void Controller::RebuildFont()
{
    if (font_)
        DeleteObject(font_);
    LOGFONTW lf{};
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    if(SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0,dpi_))lf=metrics.lfCaptionFont;
    else {lf.lfHeight=-MulDiv(9,static_cast<int>(dpi_),72);lf.lfWeight=FW_NORMAL;wcscpy_s(lf.lfFaceName,L"Segoe UI");}
    font_ = CreateFontIndirectW(&lf);
    if (!folderIcon_)
    {
        SHSTOCKICONINFO info{sizeof(info)};
        if (SUCCEEDED(SHGetStockIconInfo(SIID_FOLDER, SHGSI_ICON | SHGSI_SMALLICON, &info)))
            folderIcon_ = info.hIcon;
    }
}
void Controller::ClipRibbon(bool value)
{
    if (clipped_ && IsWindow(netui_))
    {
        SetWindowRgn(netui_, hadRegion_ ? oldRegion_ : nullptr, TRUE);
        if (hadRegion_)
            oldRegion_ = nullptr;
        clipped_ = false;
        hadRegion_ = false;
    }
    if (oldRegion_)
    {
        DeleteObject(oldRegion_);
        oldRegion_ = nullptr;
    }
    if (!value || !strip_)
        return;
    struct Search
    {
        HWND strip;
        HWND found;
    };
    Search search{strip_, nullptr};
    EnumChildWindows(
        frame_,
        [](HWND h, LPARAM p) -> BOOL {
            auto &s = *reinterpret_cast<Search *>(p);
            wchar_t c[64]{};
            GetClassNameW(h, c, 64);
            if (wcscmp(c, L"NetUIHWND"))
                return TRUE;
            RECT a{}, b{}, over{};
            GetWindowRect(h, &a);
            GetWindowRect(s.strip, &b);
            if (IntersectRect(&over, &a, &b) && over.bottom > over.top)
            {
                s.found = h;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    netui_ = search.found;
    if (!netui_)
        return;
    oldRegion_ = CreateRectRgn(0, 0, 0, 0);
    int result = GetWindowRgn(netui_, oldRegion_);
    hadRegion_ = result != ERROR;
    RECT client{}, tab{};
    GetWindowRect(netui_, &client);
    GetWindowRect(strip_, &tab);
    OffsetRect(&tab, -client.left, -client.top);
    HRGN region = hadRegion_ ? CreateRectRgn(0, 0, 0, 0)
                             : CreateRectRgn(0, 0, client.right - client.left, client.bottom - client.top);
    if (hadRegion_)
        CombineRgn(region, oldRegion_, nullptr, RGN_COPY);
    HRGN cut = CreateRectRgnIndirect(&tab);
    CombineRgn(region, region, cut, RGN_DIFF);
    DeleteObject(cut);
    if (SetWindowRgn(netui_, region, TRUE))
        clipped_ = true;
    else
        DeleteObject(region);
}
void Controller::Layout()
{
    if (!enabled_ || !strip_)
        return;
    if(IsIconic(frame_))return;
    UINT now = GetDpiForWindow(frame_);
    if (now != dpi_)
    {
        dpi_ = now;
        RebuildFont();
    }
    RECT client{},buttons{},outer{};GetClientRect(frame_,&client);GetWindowRect(frame_,&outer);
    bool measured=SUCCEEDED(DwmGetWindowAttribute(frame_,DWMWA_CAPTION_BUTTON_BOUNDS,&buttons,sizeof(buttons)))&&buttons.right>buttons.left&&buttons.bottom>buttons.top;
    POINT origin{};ClientToScreen(frame_,&origin);
    // Explorer's ribbon window classes vary. The actual caption buttons supply
    // the top row's geometry; ClipRibbon verifies its native paint owner below.
    int right=buttons.left+outer.left-origin.x,height=buttons.bottom+outer.top-origin.y;
    const int start = MulDiv(40, static_cast<int>(dpi_), 96), space = MulDiv(28, static_cast<int>(dpi_), 96);
    right -= space;
    if(!measured||height<MulDiv(16,static_cast<int>(dpi_),96)||height>MulDiv(56,static_cast<int>(dpi_),96)||right<start+space*2||right>client.right)
    {
        patch::Log(L"ExplorerTabs",L"Unsupported caption/ribbon geometry; ordinary Explorer chrome was restored.");
        SetPropW(frame_,L"Win10NativePatch.TabsRejected.v2",reinterpret_cast<HANDLE>(1));
        Enable(false);return;
    }
    tabArea_={start,0,right,height};
    SetWindowPos(strip_, HWND_TOP, tabArea_.left, tabArea_.top, tabArea_.right - tabArea_.left,
                 std::max<LONG>(20, tabArea_.bottom - tabArea_.top), SWP_NOACTIVATE);
    RECT r{};
    GetClientRect(strip_, &r);
    int unit = MulDiv(30, static_cast<int>(dpi_), 96), minimum = MulDiv(92, static_cast<int>(dpi_), 96),
        maximum = MulDiv(200, static_cast<int>(dpi_), 96);
    int available = std::max(unit, static_cast<int>(r.right) - unit * 2);
    size_t count = std::max<size_t>(1, tabs_.size());
    int width = std::clamp(available / static_cast<int>(count), minimum, maximum);
    size_t capacity = std::max<size_t>(1, available / width);
    if (active_ < firstVisible_)
        firstVisible_ = active_;
    if (active_ >= firstVisible_ + capacity)
        firstVisible_ = active_ - capacity + 1;
    if (firstVisible_ >= tabs_.size())
        firstVisible_ = 0;
    tabRects_.assign(tabs_.size(), RECT{});
    int x = 0;
    for (size_t i = firstVisible_; i < tabs_.size() && i < firstVisible_ + capacity; i++)
    {
        tabRects_[i] = {x, 0, std::min(x + width, available), r.bottom};
        x += width;
    }
    plusRect_ = {std::min(x, available), 0, std::min(x, available) + unit, r.bottom};
    overflowRect_ = {r.right - unit, 0, r.right, r.bottom};
    ClipRibbon(true);
    if(!clipped_){patch::Log(L"ExplorerTabs",L"The native caption paint region could not be isolated; tabs were disabled.");SetPropW(frame_,L"Win10NativePatch.TabsRejected.v2",reinterpret_cast<HANDLE>(2));Enable(false);return;}
    InvalidateRect(strip_, nullptr, FALSE);
}
void Controller::Paint()
{
    PAINTSTRUCT ps{};
    HDC target = BeginPaint(strip_, &ps);
    RECT r{};
    GetClientRect(strip_, &r);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, std::max<LONG>(1, r.right), std::max<LONG>(1, r.bottom));
    HGDIOBJ prior = SelectObject(dc, bitmap), oldFont = SelectObject(dc, font_);
    HIGHCONTRASTW hc{sizeof(hc)};
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0);
    bool contrast = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    COLORREF bg = contrast ? GetSysColor(COLOR_BTNFACE) : (dark_ ? RGB(32, 32, 32) : RGB(243, 243, 243));
    COLORREF selected = contrast ? GetSysColor(COLOR_HIGHLIGHT) : (dark_ ? RGB(55, 55, 55) : RGB(255, 255, 255));
    COLORREF text = contrast ? GetSysColor(COLOR_BTNTEXT) : (dark_ ? RGB(245, 245, 245) : RGB(25, 25, 25));
    HBRUSH brush = CreateSolidBrush(bg);
    FillRect(dc, &r, brush);
    DeleteObject(brush);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text);
    int pad = MulDiv(9, static_cast<int>(dpi_), 96), icon = MulDiv(16, static_cast<int>(dpi_), 96);
    for (size_t i = 0; i < tabs_.size(); i++)
    {
        RECT t = tabRects_[i];
        if (IsRectEmpty(&t))
            continue;
        if (i == active_ || static_cast<int>(i) == hot_)
        {
            HBRUSH b = CreateSolidBrush(i == active_ ? selected : (dark_ ? RGB(46, 46, 46) : RGB(226, 226, 226)));
            FillRect(dc, &t, b);
            DeleteObject(b);
        }
        if (i == active_)
        {
            HPEN pen = CreatePen(PS_SOLID, std::max(1, MulDiv(2, static_cast<int>(dpi_), 96)),
                                 contrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_HIGHLIGHT));
            HGDIOBJ old = SelectObject(dc, pen);
            MoveToEx(dc, t.left, t.bottom - 1, nullptr);
            LineTo(dc, t.right, t.bottom - 1);
            SelectObject(dc, old);
            DeleteObject(pen);
        }
        HICON tabIcon=tabs_[i].icon?tabs_[i].icon->handle:folderIcon_;
        if (tabIcon)
            DrawIconEx(dc, t.left + pad, (t.bottom - icon) / 2, tabIcon, icon, icon, 0, nullptr, DI_NORMAL);
        RECT label = t;
        label.left += pad + icon + pad;
        label.right -= pad * 3;
        if (contrast && i == active_)
            SetTextColor(dc, GetSysColor(COLOR_HIGHLIGHTTEXT));
        DrawTextW(dc, tabs_[i].title.c_str(), -1, &label, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SetTextColor(dc, text);
        int cx = t.right - pad * 2, cy = t.bottom / 2;
        HPEN pen = CreatePen(PS_SOLID, 1, text);
        HGDIOBJ old = SelectObject(dc, pen);
        int d = MulDiv(3, static_cast<int>(dpi_), 96);
        MoveToEx(dc, cx - d, cy - d, nullptr);
        LineTo(dc, cx + d + 1, cy + d + 1);
        MoveToEx(dc, cx + d, cy - d, nullptr);
        LineTo(dc, cx - d - 1, cy + d + 1);
        SelectObject(dc, old);
        DeleteObject(pen);
    }
    DrawTextW(dc, L"+", 1, &plusRect_, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    DrawTextW(dc, L"\x2304", 1, &overflowRect_, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    BitBlt(target, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, prior);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(strip_, &ps);
}
void Controller::Remember()
{
    if (tabs_.empty() || active_ >= tabs_.size() || pending_)
        return;
    auto s = Capture(browser_.Get());
    auto &t = tabs_[active_];
    if (!t.history.empty() && Equal(s.folder, t.history[t.position].folder))
    {
        s.label=t.history[t.position].label;
        t.history[t.position] = std::move(s);
    }
}
bool Controller::Navigate(ULONGLONG id, size_t position, bool changeTab)
{
    if (pending_ || !browser_)
        return false;
    auto found = std::find_if(tabs_.begin(), tabs_.end(), [&](auto &t) { return t.id == id; });
    if (found == tabs_.end() || position >= found->history.size())
        return false;
    Remember();
    Pending pending;
    pending.id = id;
    pending.position = position;
    pending.changeTab = changeTab;
    pending.expected = found->history[position].folder;
    pending.start = GetTickCount64();
    pending_ = std::move(pending);
    HRESULT hr = browser_->BrowseObject(pending_->expected.p,
                                        SBSP_SAMEBROWSER | SBSP_ABSOLUTE | SBSP_CREATENOHISTORY | SBSP_PLAYNOSOUND);
    if (FAILED(hr))
    {
        pending_.reset();
        patch::Log(L"ExplorerTabs", L"Shell navigation rejected: " + std::to_wstring(hr));
        return false;
    }
    SetTimer(frame_, MaintenanceTimer, 100, nullptr);
    PostMessageW(frame_, SyncMessage, 0, 0);
    return true;
}
bool Controller::Activate(size_t index)
{
    if (index >= tabs_.size() || pending_)
        return false;
    if (index == active_)
        return true;
    return Navigate(tabs_[index].id, tabs_[index].position, true);
}
void Controller::Synchronize()
{
    if (tearingDown_ || !browser_)
        return;
    auto s = Capture(browser_.Get());
    if (!s.folder)
        return;
    if (tabs_.empty())
    {
        Tab t;
        t.title = CachedName(s);
        t.history.push_back(std::move(s));
        tabs_.push_back(std::move(t));
        active_ = 0;
        if (!enabled_ && (patch::LoadConfig().flags & patch::ExplorerTabs))
            Enable(true);
        TabChanged();
        return;
    }
    if (pending_)
    {
        if (!Equal(s.folder, pending_->expected))
            return;
        auto i = std::find_if(tabs_.begin(), tabs_.end(), [&](auto &t) { return t.id == pending_->id; });
        if (i == tabs_.end())
        {
            pending_.reset();
            return;
        }
        active_ = static_cast<size_t>(i - tabs_.begin());
        i->position = pending_->position;
        restoreState_ = i->history[i->position];
        restoreTries_ = 0;
        pending_.reset();
        PostMessageW(frame_, RestoreMessage, 0, 0);
        SetTimer(frame_, MaintenanceTimer, 100, nullptr);
    }
    else
    {
        auto &t = tabs_[active_];
        if (!Equal(t.history[t.position].folder, s.folder))
        {
            t.history.erase(t.history.begin() + static_cast<ptrdiff_t>(t.position + 1), t.history.end());
            t.history.push_back(std::move(s));
            if (t.history.size() > MaxHistory)
                t.history.erase(t.history.begin());
            t.position = t.history.size() - 1;
        }
        t.title = CachedName(t.history[t.position]);
    }
    RefreshTravel();
    TabChanged();
}
void Controller::NewTab(const Pidl *folder)
{
    if (tabs_.size() >= MaxTabs || pending_ || tabs_.empty())
        return;
    Remember();
    Tab t;
    ViewState state;
    state.folder = folder && *folder ? *folder : tabs_[active_].history[tabs_[active_].position].folder;
    t.title = CachedName(state);
    t.history.push_back(std::move(state));
    ULONGLONG id = t.id;
    tabs_.push_back(std::move(t));
    if (!Navigate(id, 0, true))
        tabs_.pop_back();
    TabChanged();
}
void Controller::Close(size_t index)
{
    if (index >= tabs_.size() || pending_)
        return;
    if (tabs_.size() == 1)
    {
        if (frame_)
            PostMessageW(frame_, WM_SYSCOMMAND, SC_CLOSE, 0);
        return;
    }
    Remember();
    if (index == active_)
    {
        size_t target = index + 1 < tabs_.size() ? index + 1 : index - 1;
        if (!Activate(target))
            return;
    }
    closed_.push_back(tabs_[index]);
    if (closed_.size() > 32)
        closed_.erase(closed_.begin());
    tabs_.erase(tabs_.begin() + static_cast<ptrdiff_t>(index));
    if (active_ > index)
        active_--;
    else if (active_ >= tabs_.size())
        active_ = tabs_.size() - 1;
    TabChanged();
}
void Controller::Reopen()
{
    if (closed_.empty() || pending_ || tabs_.size() >= MaxTabs)
        return;
    Tab t = std::move(closed_.back());
    closed_.pop_back();
    t.id = nextId++;
    auto id = t.id;
    auto position = t.position;
    tabs_.push_back(std::move(t));
    if (!Navigate(id, position, true))
    {
        closed_.push_back(std::move(tabs_.back()));
        tabs_.pop_back();
    }
    TabChanged();
}
void Controller::History(int direction)
{
    if (tabs_.empty() || pending_)
        return;
    auto &t = tabs_[active_];
    if (direction < 0 && t.position > 0)
        Navigate(t.id, t.position - 1, false);
    else if (direction > 0 && t.position + 1 < t.history.size())
        Navigate(t.id, t.position + 1, false);
}
void Controller::TabChanged()
{
    LimitMemory();
    RefreshMetadata();
    if (enabled_ && strip_)
    {
        Layout();
        NotifyWinEvent(EVENT_OBJECT_SELECTION, strip_, OBJID_CLIENT, static_cast<LONG>(active_ + 1));
        NotifyWinEvent(EVENT_OBJECT_REORDER, strip_, OBJID_CLIENT, CHILDID_SELF);
    }
    UpdateNavigationButtons();
    SaveSession();
}
void Controller::RefreshMetadata()
{
    if(!enabled_||!frame_)return;
    for(auto& tab:tabs_)
    {
        if(tab.position>=tab.history.size())continue;
        auto& state=tab.history[tab.position];
        if(Equal(tab.metadataLoaded,state.folder))continue;
        if(tab.metadataPending&&Equal(tab.metadataExpected,state.folder))continue;
        if(!Equal(tab.metadataExpected,state.folder)){tab.icon.reset();tab.title=CachedName(state);}
        tab.metadataExpected=state.folder;
        ++tab.metadataGeneration;
        tab.metadataPending=QueueMetadata({metadataCookie_,tab.id,tab.metadataGeneration,state.folder});
    }
}
void Controller::AcceptMetadata(ULONGLONG ticket)
{
    auto result=TakeMetadata(ticket,metadataCookie_);
    if(result&&enabled_)
    {
        auto tab=std::find_if(tabs_.begin(),tabs_.end(),[&](const Tab& t){return t.id==result->job.tab;});
        if(tab!=tabs_.end()&&tab->metadataGeneration==result->job.generation&&tab->position<tab->history.size()&&Equal(tab->history[tab->position].folder,result->job.folder))
        {
            tab->metadataPending=false;tab->metadataLoaded=result->job.folder;
            if(!result->name.empty()){tab->title=result->name.substr(0,256);tab->history[tab->position].label=tab->title;}
            tab->icon=std::move(result->icon);
            if(strip_)InvalidateRect(strip_,nullptr,FALSE);
            NotifyWinEvent(EVENT_OBJECT_NAMECHANGE,strip_,OBJID_CLIENT,static_cast<LONG>((tab-tabs_.begin())+1));
        }
    }
    RefreshMetadata();
}
void Controller::SaveSession()
{
    if (tabs_.empty())
        return;
    auto encode = [&](const std::vector<Tab> &tabs) {
        Writer writer;
        writer.number(PacketMagic);
        writer.number(DWORD{3});
        writer.number(static_cast<DWORD>(tabs.size()));
        writer.number(static_cast<DWORD>(active_));
        for (const auto &tab : tabs)
            writer.tab(tab);
        return writer;
    };
    Writer w = encode(tabs_);
    if (!w.ok)
    {
        // Keep every open tab even when a very large selection/history would
        // exceed the persistence budget. Live in-memory history is unchanged.
        auto compact = tabs_;
        for (auto &tab : compact)
        {
            ViewState current = tab.history[tab.position];
            current.selected.clear();
            current.focused = {};
            current.anchor = {};
            tab.history.clear();
            tab.history.push_back(std::move(current));
            tab.position = 0;
        }
        w = encode(compact);
        if (!w.ok)
            return;
    }
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SessionKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) ==
        ERROR_SUCCESS)
    {
        RegSetValueExW(key, L"LastSession", 0, REG_BINARY, w.bytes.data(), static_cast<DWORD>(w.bytes.size()));
        RegCloseKey(key);
    }
}
void Controller::LoadSession()
{
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, SessionKey, L"LastSession", RRF_RT_REG_BINARY, nullptr, nullptr, &size) !=
            ERROR_SUCCESS ||
        size > MaxPacket || size < 16)
        return;
    std::vector<BYTE> bytes(size);
    if (RegGetValueW(HKEY_CURRENT_USER, SessionKey, L"LastSession", RRF_RT_REG_BINARY, nullptr, bytes.data(), &size) !=
        ERROR_SUCCESS)
        return;
    Reader r{bytes.data(), size};
    if (r.number<DWORD>() != PacketMagic || r.number<DWORD>() != 3)
        return;
    DWORD count = r.number<DWORD>(), active = r.number<DWORD>();
    if (count == 0 || count > MaxTabs || active >= count)
        return;
    std::vector<Tab> loaded;
    for (DWORD i = 0; i < count && r.ok; i++)
        loaded.push_back(r.tab());
    if (!r.ok || r.left || loaded.size() != count)
        return;
    auto previous = std::move(tabs_);
    tabs_ = std::move(loaded);
    active_ = active;
    if (!Navigate(tabs_[active_].id, tabs_[active_].position, true))
    {
        tabs_ = std::move(previous);
        active_ = 0;
    }
}
void Controller::RefreshTravel()
{
    if (!enabled_)
        return;
    HWND found{};
    EnumChildWindows(
        frame_,
        [](HWND h, LPARAM p) -> BOOL {
            wchar_t c[64]{};
            GetClassNameW(h, c, 64);
            if (!wcscmp(c, L"TravelBand"))
            {
                *reinterpret_cast<HWND *>(p) = h;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    if (found != travel_)
    {
        if (travel_)
            RemoveWindowSubclass(travel_, TravelProc, reinterpret_cast<UINT_PTR>(this));
        travel_ = found;
        if (travel_)
            SetWindowSubclass(travel_, TravelProc, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this));
    }
    if (travel_)
    {
        HWND toolbar = FindWindowExW(travel_, nullptr, TOOLBARCLASSNAMEW, nullptr);
        TBBUTTON b{};
        if (toolbar && SendMessageW(toolbar, TB_GETBUTTON, 0, reinterpret_cast<LPARAM>(&b)))
            backCommand_ = b.idCommand;
        if (toolbar && SendMessageW(toolbar, TB_GETBUTTON, 1, reinterpret_cast<LPARAM>(&b)))
            forwardCommand_ = b.idCommand;
    }
    UpdateNavigationButtons();
}
void Controller::UpdateNavigationButtons()
{
    if (!enabled_ || !travel_ || tabs_.empty())
        return;
    HWND toolbar = FindWindowExW(travel_, nullptr, TOOLBARCLASSNAMEW, nullptr);
    auto &t = tabs_[active_];
    if (toolbar)
    {
        if (backCommand_ >= 0)
            SendMessageW(toolbar, TB_ENABLEBUTTON, backCommand_, MAKELPARAM(!pending_ && t.position > 0, 0));
        if (forwardCommand_ >= 0)
            SendMessageW(toolbar, TB_ENABLEBUTTON, forwardCommand_,
                         MAKELPARAM(!pending_ && t.position + 1 < t.history.size(), 0));
    }
}
void Controller::HistoryMenu(bool back, POINT screen)
{
    if (tabs_.empty() || pending_)
        return;
    auto &t = tabs_[active_];
    HMENU menu = CreatePopupMenu();
    std::vector<size_t> entries;
    if (back)
    {
        for (size_t i = t.position; i > 0; i--)
            entries.push_back(i - 1);
    }
    else
    {
        for (size_t i = t.position + 1; i < t.history.size(); i++)
            entries.push_back(i);
    }
    for (size_t i = 0; i < entries.size(); i++)
        AppendMenuW(menu, MF_STRING, 100 + static_cast<UINT_PTR>(i), CachedName(t.history[entries[i]]).c_str());
    UINT selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, frame_, nullptr);
    DestroyMenu(menu);
    if (selected >= 100 && selected - 100 < entries.size())
        Navigate(t.id, entries[selected - 100], false);
}
int Controller::Hit(POINT point) const
{
    for (size_t i = 0; i < tabRects_.size(); i++)
        if (PtInRect(&tabRects_[i], point))
            return static_cast<int>(i);
    if (PtInRect(&plusRect_, point))
        return -2;
    if (PtInRect(&overflowRect_, point))
        return -3;
    return -1;
}
bool Controller::HitClose(int i, POINT p) const
{
    if (i < 0 || static_cast<size_t>(i) >= tabRects_.size())
        return false;
    RECT r = tabRects_[i];
    r.left = r.right - MulDiv(30, static_cast<int>(dpi_), 96);
    return PtInRect(&r, p) != FALSE;
}
void Controller::Menu(int hit, POINT point)
{
    HMENU menu = CreatePopupMenu();
    constexpr UINT New = 1, CloseTab = 2, ReopenTab = 3, Duplicate = 4, DetachTabCommand = 5, Options = 6;
    if (hit == -3)
    {
        for (size_t i = 0; i < tabs_.size(); i++)
            AppendMenuW(menu, MF_STRING | (i == active_ ? MF_CHECKED : 0), 100 + static_cast<UINT_PTR>(i),
                        tabs_[i].title.c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING, New, L"New tab\tCtrl+T");
    AppendMenuW(menu, MF_STRING | (closed_.empty() ? MF_GRAYED : 0), ReopenTab, L"Reopen closed tab\tCtrl+Shift+T");
    if (hit >= 0)
    {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, Duplicate, L"Duplicate tab");
        AppendMenuW(menu, MF_STRING, DetachTabCommand, L"Move tab to new window");
        AppendMenuW(menu, MF_STRING, CloseTab, L"Close tab\tCtrl+W");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, Options, L"Windows 10 patch options...");
    UINT selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, frame_, nullptr);
    DestroyMenu(menu);
    if (selected >= 100 && selected - 100 < tabs_.size())
    {
        Activate(selected - 100);
        return;
    }
    if (selected == New)
        NewTab();
    else if (selected == CloseTab && hit >= 0)
        Close(hit);
    else if (selected == ReopenTab)
        Reopen();
    else if (selected == Duplicate && hit >= 0)
    {
        Pidl id = tabs_[hit].history[tabs_[hit].position].folder;
        NewTab(&id);
    }
    else if (selected == DetachTabCommand && hit >= 0)
        DetachTab(hit);
    else if (selected == Options)
        patch::OpenOptions(frame_);
}
void Controller::Mouse(UINT message, WPARAM, LPARAM data)
{
    POINT point{GET_X_LPARAM(data), GET_Y_LPARAM(data)};
    int hit = Hit(point);
    if (message == WM_MOUSEMOVE)
    {
        if (hot_ != hit)
        {
            hot_ = hit;
            InvalidateRect(strip_, nullptr, FALSE);
        }
        TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, strip_, 0};
        TrackMouseEvent(&t);
        if (pressed_ >= 0 && !pressedClose_ && (GetKeyState(VK_LBUTTON) & 0x8000) && !inDrag_)
        {
            if (abs(point.x - dragStart_.x) >= GetSystemMetrics(SM_CXDRAG) ||
                abs(point.y - dragStart_.y) >= GetSystemMetrics(SM_CYDRAG))
            {
                int source = pressed_;
                pressed_ = -1;
                ReleaseCapture();
                StartDrag(source);
            }
        }
        return;
    }
    if (message == WM_LBUTTONDOWN)
    {
        SetFocus(strip_);
        pressed_ = hit;
        pressedClose_ = HitClose(hit, point);
        dragStart_ = point;
        SetCapture(strip_);
        if (hit == -1)
        {
            ReleaseCapture();
            POINT screen = point;
            ClientToScreen(strip_, &screen);
            SendMessageW(frame_, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(screen.x, screen.y));
        }
        return;
    }
    if (message == WM_LBUTTONUP)
    {
        ReleaseCapture();
        int was = pressed_;
        pressed_ = -1;
        if (was != hit)
            return;
        if (hit >= 0)
        {
            if (pressedClose_ && HitClose(hit, point))
                Close(hit);
            else
                Activate(hit);
        }
        else if (hit == -2)
            NewTab();
        else if (hit == -3)
        {
            ClientToScreen(strip_, &point);
            Menu(-3, point);
        }
        return;
    }
    if (message == WM_MBUTTONUP && hit >= 0)
    {
        Close(hit);
        return;
    }
    if (message == WM_RBUTTONUP)
    {
        ClientToScreen(strip_, &point);
        Menu(hit, point);
        return;
    }
    if (message == WM_LBUTTONDBLCLK && hit == -1)
        SendMessageW(frame_, WM_SYSCOMMAND, IsZoomed(frame_) ? SC_RESTORE : SC_MAXIMIZE, 0);
}
LRESULT CALLBACK Controller::StripProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    Controller *self = reinterpret_cast<Controller *>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE)
    {
        self = static_cast<Controller *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
        return DefWindowProcW(h, m, w, l);
    switch (m)
    {
    case WM_PAINT:
        self->Paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
    case WM_LBUTTONDBLCLK:
        self->Mouse(m, w, l);
        return 0;
    case WM_MOUSELEAVE:
        self->hot_ = -1;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_GETOBJECT:
        if (static_cast<LONG>(l) == OBJID_CLIENT)
            return LresultFromObject(IID_IAccessible, w, static_cast<IAccessible *>(self));
        break;
    case WM_SETFOCUS:
        NotifyWinEvent(EVENT_OBJECT_FOCUS, h, OBJID_CLIENT, static_cast<LONG>(self->active_ + 1));
        break;
    case WM_KEYDOWN:
        if (w == VK_LEFT && self->active_ > 0)
        {
            self->Activate(self->active_ - 1);
            return 0;
        }
        if (w == VK_RIGHT && self->active_ + 1 < self->tabs_.size())
        {
            self->Activate(self->active_ + 1);
            return 0;
        }
        if (w == VK_DELETE)
        {
            self->Close(self->active_);
            return 0;
        }
        if (w == VK_APPS || (w == VK_F10 && (GetKeyState(VK_SHIFT) & 0x8000)))
        {
            POINT p{};
            GetCursorPos(&p);
            self->Menu(static_cast<int>(self->active_), p);
            return 0;
        }
        break;
    case WM_NCDESTROY:
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(h, m, w, l);
}
LRESULT CALLBACK Controller::FrameProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR p)
{
    auto *self = reinterpret_cast<Controller *>(p);
    self->AddRef();
    LRESULT result = 0;
    bool handled = false;
    if (m == patch::mvp::DetachMessage() && self->selfHosted_)
    {
        self->selfHosted_ = false;
        self->Detach();
        self->Release();
        handled = true;
        result = 1;
    }
    else if (m == patch::SettingsChangedMessage())
    {
        self->Enable((patch::LoadConfig().flags & patch::ExplorerTabs) != 0);
        handled = true;
    }
    else if (m == SyncMessage)
    {
        self->Synchronize();
        handled = true;
    }
    else if (m == IconMessage)
    {
        if(static_cast<ULONGLONG>(l)==self->metadataCookie_)self->AcceptMetadata(static_cast<ULONGLONG>(w));
        handled=true;
    }
    else if (m == RestoreMessage)
    {
        if (self->restoreState_)
        {
            Restore(self->browser_.Get(), *self->restoreState_);
            self->restoreTries_++;
        }
        handled = true;
    }
    else if (m == WM_TIMER && w == MaintenanceTimer)
    {
        self->Timer();
        handled = true;
    }
    else if (m == WM_COPYDATA && self->enabled_)
    {
        auto *d = reinterpret_cast<COPYDATASTRUCT *>(l);
        if (d && d->dwData == PacketMagic && d->cbData <= MaxPacket)
        {
            auto tab = Deserialize(static_cast<const BYTE *>(d->lpData), d->cbData);
            result = tab && self->Import(std::move(*tab), true) ? 1 : 0;
            handled = true;
        }
    }
    else if (m == WM_APPCOMMAND && self->enabled_)
    {
        int command = GET_APPCOMMAND_LPARAM(l);
        if (command == APPCOMMAND_BROWSER_BACKWARD || command == APPCOMMAND_BROWSER_FORWARD)
        {
            self->History(command == APPCOMMAND_BROWSER_BACKWARD ? -1 : 1);
            result = 1;
            handled = true;
        }
    }
    if (!handled)
    {
        if (m == WM_SIZE || m == WM_WINDOWPOSCHANGED || m == WM_DPICHANGED)
        {
            result = DefSubclassProc(h, m, w, l);
            self->Layout();
            handled = true;
        }
        else if (m == WM_THEMECHANGED || m == WM_SETTINGCHANGE || m == WM_SYSCOLORCHANGE)
        {
            result = DefSubclassProc(h, m, w, l);
            self->dark_ = Dark();
            self->RebuildFont();
            self->Layout();
            handled = true;
        }
        else if (m == WM_CLOSE)
        {
            self->Remember();
            self->SaveSession();
        }
        else if (m == WM_NCDESTROY)
        {
            bool owned = std::exchange(self->selfHosted_, false);
            self->Detach();
            if (owned) self->Release();
        }
    }
    if (!handled)
        result = DefSubclassProc(h, m, w, l);
    self->Release();
    return result;
}
LRESULT CALLBACK Controller::TravelProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR p)
{
    auto *self = reinterpret_cast<Controller *>(p);
    if (self->enabled_)
    {
        if (m == WM_COMMAND)
        {
            int cmd = LOWORD(w);
            if (cmd == self->backCommand_ || cmd == self->forwardCommand_)
            {
                self->History(cmd == self->backCommand_ ? -1 : 1);
                return 0;
            }
        }
        if (m == WM_NOTIFY)
        {
            auto *hdr = reinterpret_cast<NMHDR *>(l);
            if (hdr && hdr->code == TBN_DROPDOWN)
            {
                auto *drop = reinterpret_cast<NMTOOLBARW *>(l);
                if (drop->iItem == self->backCommand_ || drop->iItem == self->forwardCommand_)
                {
                    POINT point{};
                    GetCursorPos(&point);
                    self->HistoryMenu(drop->iItem == self->backCommand_, point);
                    return TBDDRET_DEFAULT;
                }
            }
        }
        if (m == WM_CONTEXTMENU)
        {
            POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            POINT local = point;
            HWND toolbar = FindWindowExW(h, nullptr, TOOLBARCLASSNAMEW, nullptr);
            ScreenToClient(toolbar, &local);
            LRESULT index = SendMessageW(toolbar, TB_HITTEST, 0, reinterpret_cast<LPARAM>(&local));
            if (index == 0 || index == 1)
            {
                self->HistoryMenu(index == 0, point);
                return 0;
            }
        }
    }
    return DefSubclassProc(h, m, w, l);
}
LRESULT CALLBACK Controller::KeyProc(int code, WPARAM remove, LPARAM data)
{
    if (code >= 0 && remove == PM_REMOVE)
    {
        auto *message = reinterpret_cast<MSG *>(data);
        HWND frame = GetAncestor(message->hwnd, GA_ROOT);
        auto *self = static_cast<Controller *>(GetPropW(frame, ControllerProperty));
        if (self && self->enabled_)
        {
            bool eat = false;
            if (message->message == WM_KEYDOWN || message->message == WM_SYSKEYDOWN)
            {
                bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0,
                     alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
                UINT key = static_cast<UINT>(message->wParam);
                bool repeated = (message->lParam & (1ll << 30)) != 0;
                if (ctrl && !alt && key == 'T')
                {
                    if (!repeated)
                    {
                        if (shift)
                            self->Reopen();
                        else
                            self->NewTab();
                    }
                    eat = true;
                }
                else if (ctrl && !alt && !shift && key == 'W')
                {
                    if (!repeated)
                        self->Close(self->active_);
                    eat = true;
                }
                else if (ctrl && !alt && key == VK_TAB)
                {
                    if (!self->tabs_.empty())
                        self->Activate((self->active_ + self->tabs_.size() + (shift ? -1 : 1)) % self->tabs_.size());
                    eat = true;
                }
                else if (ctrl && !alt && !shift && key >= '1' && key <= '9')
                {
                    size_t index = key == '9' ? self->tabs_.size() - 1 : key - '1';
                    self->Activate(index);
                    eat = true;
                }
                else if (alt && !ctrl && !shift && (key == VK_LEFT || key == VK_RIGHT))
                {
                    if (!repeated)
                        self->History(key == VK_LEFT ? -1 : 1);
                    eat = true;
                }
                else if (!ctrl && !alt && !shift && key == VK_BACK)
                {
                    wchar_t name[64]{};
                    GetClassNameW(GetFocus(), name, 64);
                    if (_wcsicmp(name, L"Edit") != 0)
                    {
                        if (!repeated)
                            self->History(-1);
                        eat = true;
                    }
                }
            }
            else if (message->message == WM_XBUTTONUP)
            {
                self->History(GET_XBUTTON_WPARAM(message->wParam) == XBUTTON1 ? -1 : 1);
                eat = true;
            }
            else if (message->message == WM_XBUTTONDOWN)
                eat = true;
            if (eat)
            {
                message->message = WM_NULL;
                message->wParam = 0;
                message->lParam = 0;
            }
        }
    }
    return CallNextHookEx(nullptr, code, remove, data);
}
HRESULT Controller::Invoke(DISPID id, REFIID, LCID, WORD flags, DISPPARAMS *params, VARIANT *result,
                           EXCEPINFO *exception, UINT *argument)
{
    if (id < 0)
    {
        HRESULT hr = AccessTypeInfo();
        return SUCCEEDED(hr) ? DispInvoke(static_cast<IAccessible *>(this), accessInfo_.Get(), id, flags, params,
                                          result, exception, argument)
                             : hr;
    }
    if (tearingDown_)
        return S_OK;
    if (id == DISPID_BEFORENAVIGATE2)
        Remember();
    else if (id == DISPID_NAVIGATECOMPLETE2 || id == DISPID_DOCUMENTCOMPLETE)
        PostMessageW(frame_, SyncMessage, 0, 0);
    else if (id == DISPID_NAVIGATEERROR)
    {
        pending_.reset();
        restoreState_.reset();
        PostMessageW(frame_, SyncMessage, 0, 0);
    }
    return S_OK;
}
HRESULT Controller::AccessTypeInfo()
{
    if (accessInfo_)
        return S_OK;
    ComPtr<ITypeLib> library;
    HRESULT hr = LoadRegTypeLib(LIBID_Accessibility, 1, 1, LOCALE_SYSTEM_DEFAULT, &library);
    return SUCCEEDED(hr) ? library->GetTypeInfoOfGuid(IID_IAccessible, &accessInfo_) : hr;
}
void Controller::LimitMemory()
{
    constexpr size_t budget = 32 * 1024 * 1024;
    auto size = [](const ViewState &s) {
        size_t n = 0;
        for (auto *p : {&s.folder, &s.focused, &s.anchor})
            if (*p)
                n += ILGetSize(p->p);
        for (auto &p : s.selected)
            if (p)
                n += ILGetSize(p.p);
        return n;
    };
    size_t total = 0;
    for (auto &t : tabs_)
        for (auto &s : t.history)
            total += size(s);
    for (auto &t : closed_)
        for (auto &s : t.history)
            total += size(s);
    while (total > budget && !closed_.empty())
    {
        for (auto &s : closed_.front().history)
            total -= size(s);
        closed_.erase(closed_.begin());
    }
    // Old history retains its folder, view mode and scroll anchor. Expensive
    // selections are shed before any navigation entries are removed.
    for (auto &t : tabs_)
    {
        for (size_t i = 0; i < t.history.size() && total > budget; i++)
        {
            if (i == t.position)
                continue;
            auto &s = t.history[i];
            size_t before = size(s);
            s.selected.clear();
            s.focused = {};
            total -= before - size(s);
        }
    }
    for (size_t i = 0; i < tabs_.size() && total > budget; i++)
    {
        if (i == active_)
            continue;
        auto &s = tabs_[i].history[tabs_[i].position];
        size_t before = size(s);
        s.selected.clear();
        s.focused = {};
        total -= before - size(s);
    }
    for (auto &t : tabs_)
    {
        while (total > budget && t.history.size() > t.position + 1)
        {
            total -= size(t.history.back());
            t.history.pop_back();
        }
        while (total > budget && t.history.size() > 1 && t.position > 0)
        {
            total -= size(t.history.front());
            t.history.erase(t.history.begin());
            t.position--;
        }
    }
}
void Controller::SnapshotRoots(std::unordered_set<HWND> &roots)
{
    EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
            if (ExplorerRoot(h))
                reinterpret_cast<std::unordered_set<HWND> *>(p)->insert(h);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&roots));
}
bool Controller::Import(Tab tab, bool replaceInitial)
{
    if (!enabled_ || pending_ || tab.history.empty() || tabs_.size() >= MaxTabs)
        return false;
    tab.id = nextId++;
    tab.sourceFrame = nullptr;
    tab.sourceId = 0;
    tab.sourceProcess = 0;
    if (replaceInitial && tabs_.size() == 1 && Equal(CurrentFolder(browser_.Get()), tab.history[tab.position].folder))
    {
        tabs_[0] = std::move(tab);
        active_ = 0;
        restoreState_ = tabs_[0].history[tabs_[0].position];
        restoreTries_ = 0;
        PostMessageW(frame_, RestoreMessage, 0, 0);
        SetTimer(frame_, MaintenanceTimer, 100, nullptr);
        TabChanged();
        return true;
    }
    auto id = tab.id;
    size_t position = tab.position;
    tabs_.push_back(std::move(tab));
    if (!Navigate(id, position, true))
    {
        tabs_.pop_back();
        return false;
    }
    TabChanged();
    return true;
}
void Controller::DetachTab(size_t index)
{
    if (index >= tabs_.size() || pending_ || detached_)
        return;
    Remember();
    Detached d;
    d.id = tabs_[index].id;
    d.start = GetTickCount64();
    d.packet = Serialize(tabs_[index], frame_);
    if (d.packet.empty())
        return;
    SnapshotRoots(d.oldWindows);
    Pidl folder = tabs_[index].history[tabs_[index].position].folder;
    detached_ = std::move(d);
    HRESULT hr = browser_->BrowseObject(folder.p, SBSP_NEWBROWSER | SBSP_ABSOLUTE | SBSP_NOTRANSFERHIST);
    if (FAILED(hr))
    {
        detached_.reset();
        patch::Log(L"ExplorerTabs", L"Opening the detached Explorer window failed.");
        return;
    }
    SetTimer(frame_, MaintenanceTimer, 100, nullptr);
}
void Controller::StartDrag(size_t index)
{
    if (index >= tabs_.size() || pending_ || inDrag_)
        return;
    Remember();
    ULONGLONG source = tabs_[index].id;
    auto packet = Serialize(tabs_[index], frame_);
    if (packet.empty())
        return;
    AddRef();
    inDrag_ = true;
    sameWindowDrop_ = false;
    ComPtr<DropData> data;
    data.Attach(new (std::nothrow) DropData(std::move(packet)));
    ComPtr<DropSource> sourceObject;
    sourceObject.Attach(new (std::nothrow) DropSource());
    DWORD effect = 0;
    if (data && sourceObject)
    {
        DoDragDrop(data.Get(), sourceObject.Get(), DROPEFFECT_MOVE | DROPEFFECT_COPY, &effect);
        inDrag_ = false;
        auto i = std::find_if(tabs_.begin(), tabs_.end(), [&](auto &t) { return t.id == source; });
        if (i != tabs_.end() && frame_ && enabled_)
        {
            size_t position = static_cast<size_t>(i - tabs_.begin());
            if (effect == DROPEFFECT_MOVE && !sameWindowDrop_)
                Close(position);
            else if (effect == DROPEFFECT_NONE && sourceObject->released && !sourceObject->escaped)
            {
                POINT point{};
                GetCursorPos(&point);
                HWND target = GetAncestor(WindowFromPoint(point), GA_ROOT);
                if (target != frame_)
                    DetachTab(position);
            }
        }
    }
    else
        inDrag_ = false;
    Release();
}
void Controller::Timer()
{
    if (tabs_.empty() && startupPending_)
        Synchronize();
    if (startupPending_ && GetTickCount64() - startupStart_ >= 10000)
        CompleteStartupGuard();
    if (pending_)
    {
        Synchronize();
        if (pending_ && GetTickCount64() - pending_->start > 15000)
        {
            pending_.reset();
            Synchronize();
            patch::Log(L"ExplorerTabs", L"The requested folder did not become ready within 15 seconds. The current "
                                        L"Explorer location was retained.");
        }
    }
    if (restoreState_)
    {
        if (Equal(CurrentFolder(browser_.Get()), restoreState_->folder))
        {
            Restore(browser_.Get(), *restoreState_);
            if (++restoreTries_ >= 10)
            {
                restoreState_.reset();
                Remember();
                SaveSession();
            }
        }
        else
            restoreState_.reset();
    }
    if (detached_)
    {
        ComPtr<IShellWindows> windows;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows))))
        {
            long count = 0;
            windows->get_Count(&count);
            for (long i = 0; i < count && detached_; i++)
            {
                VARIANT v{};
                v.vt = VT_I4;
                v.lVal = i;
                ComPtr<IDispatch> dispatch;
                ComPtr<IWebBrowser2> web;
                ComPtr<IServiceProvider> provider;
                ComPtr<IShellBrowser> browser;
                if (FAILED(windows->Item(v, &dispatch)) || FAILED(dispatch.As(&web)))
                    continue;
                SHANDLE_PTR raw{};
                web->get_HWND(&raw);
                HWND root = GetAncestor(reinterpret_cast<HWND>(raw), GA_ROOT);
                if (!root || detached_->oldWindows.contains(root) || !GetPropW(root, patch::TabsActiveProperty) ||
                    FAILED(dispatch.As(&provider)) ||
                    FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))))
                    continue;
                auto source = std::find_if(tabs_.begin(), tabs_.end(), [&](auto &t) { return t.id == detached_->id; });
                if (source == tabs_.end())
                {
                    detached_.reset();
                    break;
                }
                if (!Equal(CurrentFolder(browser.Get()), source->history[source->position].folder))
                    continue;
                COPYDATASTRUCT data{PacketMagic, static_cast<DWORD>(detached_->packet.size()),
                                    detached_->packet.data()};
                DWORD_PTR accepted{};
                if (SendMessageTimeoutW(root, WM_COPYDATA, reinterpret_cast<WPARAM>(frame_),
                                        reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000,
                                        &accepted) &&
                    accepted)
                {
                    size_t index = static_cast<size_t>(source - tabs_.begin());
                    detached_.reset();
                    Close(index);
                    break;
                }
            }
        }
        if (detached_ && GetTickCount64() - detached_->start > 10000)
        {
            detached_.reset();
            patch::Log(L"ExplorerTabs", L"The new window did not accept the detached tab. The original tab was kept.");
        }
    }
    if (!startupPending_ && !pending_ && !restoreState_ && !detached_)
        KillTimer(frame_, MaintenanceTimer);
}
void Controller::BindFolderDrop()
{
    folderDrop_.Reset();
    ComPtr<IShellView> view;
    if (browser_ && SUCCEEDED(browser_->QueryActiveShellView(&view)))
        view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&folderDrop_));
}
HRESULT Controller::DragEnter(IDataObject *object, DWORD keys, POINTL point, DWORD *effect)
{
    if (!effect)
        return E_POINTER;
    dropObject_ = object;
    FORMATETC f{static_cast<CLIPFORMAT>(TabFormat()), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    dropPrivate_ = object && SUCCEEDED(object->QueryGetData(&f));
    hoverDrop_ = -1;
    hoverStart_ = GetTickCount64();
    BindFolderDrop();
    if (dropPrivate_)
    {
        *effect &= DROPEFFECT_MOVE | DROPEFFECT_COPY;
        return S_OK;
    }
    POINT local{point.x, point.y};
    ScreenToClient(strip_, &local);
    hoverDrop_ = Hit(local);
    if (folderDrop_ && hoverDrop_ == static_cast<int>(active_))
        return folderDrop_->DragEnter(object, keys, point, effect);
    *effect = DROPEFFECT_NONE;
    return S_OK;
}
HRESULT Controller::DragOver(DWORD keys, POINTL point, DWORD *effect)
{
    if (!effect)
        return E_POINTER;
    if (dropPrivate_)
    {
        *effect &= DROPEFFECT_MOVE | DROPEFFECT_COPY;
        return S_OK;
    }
    POINT local{point.x, point.y};
    ScreenToClient(strip_, &local);
    int hit = Hit(local);
    if (hit != hoverDrop_)
    {
        if (folderDrop_)
            folderDrop_->DragLeave();
        hoverDrop_ = hit;
        hoverStart_ = GetTickCount64();
        folderDrop_.Reset();
    }
    if (hit >= 0 && hit != static_cast<int>(active_) && GetTickCount64() - hoverStart_ > 600 && !pending_)
    {
        Activate(hit);
        hoverStart_ = GetTickCount64();
    }
    if (hit >= 0 && hit == static_cast<int>(active_) && !pending_)
    {
        if (!folderDrop_)
        {
            BindFolderDrop();
            if (folderDrop_)
                return folderDrop_->DragEnter(dropObject_.Get(), keys, point, effect);
        }
        if (folderDrop_)
            return folderDrop_->DragOver(keys, point, effect);
    }
    *effect = DROPEFFECT_NONE;
    return S_OK;
}
HRESULT Controller::DragLeave()
{
    if (folderDrop_ && !dropPrivate_)
        folderDrop_->DragLeave();
    folderDrop_.Reset();
    dropObject_.Reset();
    hoverDrop_ = -1;
    dropPrivate_ = false;
    return S_OK;
}
HRESULT Controller::Drop(IDataObject *object, DWORD keys, POINTL point, DWORD *effect)
{
    if (!effect)
        return E_POINTER;
    DWORD allowed = *effect;
    *effect = DROPEFFECT_NONE;
    FORMATETC f{static_cast<CLIPFORMAT>(TabFormat()), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (object && SUCCEEDED(object->GetData(&f, &medium)))
    {
        if (medium.tymed == TYMED_HGLOBAL)
        {
            SIZE_T size = GlobalSize(medium.hGlobal);
            const BYTE *data = static_cast<const BYTE *>(GlobalLock(medium.hGlobal));
            auto tab = Deserialize(data, size);
            if (data)
                GlobalUnlock(medium.hGlobal);
            if (tab)
            {
                POINT local{point.x, point.y};
                ScreenToClient(strip_, &local);
                int hit = Hit(local);
                if (tab->sourceFrame == frame_ && tab->sourceProcess == GetCurrentProcessId())
                {
                    auto i = std::find_if(tabs_.begin(), tabs_.end(), [&](auto &t) { return t.id == tab->sourceId; });
                    if (i != tabs_.end() && !pending_)
                    {
                        ULONGLONG activeId = tabs_[active_].id;
                        size_t from = static_cast<size_t>(i - tabs_.begin()),
                               to = hit >= 0 ? static_cast<size_t>(hit) : tabs_.size() - 1;
                        Tab moved = std::move(*i);
                        tabs_.erase(i);
                        to = std::min(to, tabs_.size());
                        tabs_.insert(tabs_.begin() + static_cast<ptrdiff_t>(to), std::move(moved));
                        auto active =
                            std::find_if(tabs_.begin(), tabs_.end(), [&](auto &t) { return t.id == activeId; });
                        active_ = static_cast<size_t>(active - tabs_.begin());
                        sameWindowDrop_ = true;
                        *effect = allowed & DROPEFFECT_MOVE;
                        TabChanged();
                        (void)from;
                    }
                }
                else if (Import(std::move(*tab), false))
                    *effect = allowed & DROPEFFECT_MOVE;
            }
        }
        ReleaseStgMedium(&medium);
        DragLeave();
        return S_OK;
    }
    POINT local{point.x, point.y};
    ScreenToClient(strip_, &local);
    int hit = Hit(local);
    if (hit >= 0 && hit == static_cast<int>(active_) && !pending_)
    {
        if (!folderDrop_)
            BindFolderDrop();
        if (folderDrop_)
        {
            *effect = allowed;
            HRESULT hr = folderDrop_->Drop(object, keys, point, effect);
            folderDrop_.Reset();
            dropObject_.Reset();
            dropPrivate_ = false;
            return hr;
        }
    }
    // Dropping directories on the spare/new-tab area opens folders. Files dropped
    // on a tab itself always use the Shell's real move/copy/drop implementation.
    FORMATETC files{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    if (hit < 0 && object && SUCCEEDED(object->GetData(&files, &medium)))
    {
        auto drop = reinterpret_cast<HDROP>(medium.hGlobal);
        UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
        size_t first = tabs_.size();
        for (UINT i = 0; i < count && tabs_.size() < MaxTabs; i++)
        {
            UINT n = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(n + 1, L'\0');
            DragQueryFileW(drop, i, path.data(), n + 1);
            path.resize(n);
            DWORD attrs = GetFileAttributesW(path.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
                continue;
            PIDLIST_ABSOLUTE id{};
            if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &id, 0, nullptr)))
            {
                Tab t;
                ViewState s;
                s.folder = Pidl::Adopt(id);
                t.title = CachedName(s);
                t.history.push_back(std::move(s));
                tabs_.push_back(std::move(t));
            }
        }
        ReleaseStgMedium(&medium);
        if (tabs_.size() > first)
        {
            Activate(first);
            *effect = allowed & (DROPEFFECT_LINK | DROPEFFECT_COPY);
            if (*effect & DROPEFFECT_LINK)
                *effect = DROPEFFECT_LINK;
            else if (*effect & DROPEFFECT_COPY)
                *effect = DROPEFFECT_COPY;
            TabChanged();
        }
    }
    DragLeave();
    return S_OK;
}
HRESULT Controller::ChildName(LONG child, BSTR *out)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;
    std::wstring name;
    if (child == 0)
        name = L"File Explorer tabs";
    else if (child > 0 && static_cast<size_t>(child) <= tabs_.size())
        name = tabs_[child - 1].title;
    else if (child == static_cast<LONG>(tabs_.size() + 1))
        name = L"New tab";
    else if (child == static_cast<LONG>(tabs_.size() + 2))
        name = L"All tabs and options";
    else
        return E_INVALIDARG;
    *out = SysAllocString(name.c_str());
    return *out ? S_OK : E_OUTOFMEMORY;
}
bool Controller::ChildRect(LONG child, RECT &out) const
{
    if (!strip_)
        return false;
    if (child == 0)
        GetClientRect(strip_, &out);
    else if (child > 0 && static_cast<size_t>(child) <= tabRects_.size())
        out = tabRects_[child - 1];
    else if (child == static_cast<LONG>(tabs_.size() + 1))
        out = plusRect_;
    else if (child == static_cast<LONG>(tabs_.size() + 2))
        out = overflowRect_;
    else
        return false;
    MapWindowPoints(strip_, nullptr, reinterpret_cast<POINT *>(&out), 2);
    return true;
}
HRESULT Controller::get_accState(VARIANT v, VARIANT *out)
{
    if (!out)
        return E_POINTER;
    VariantInit(out);
    out->vt = VT_I4;
    out->lVal = STATE_SYSTEM_FOCUSABLE;
    LONG child = v.vt == VT_I4 ? v.lVal : 0;
    if (child > 0 && static_cast<size_t>(child) <= tabs_.size())
    {
        out->lVal |= STATE_SYSTEM_SELECTABLE;
        if (static_cast<size_t>(child - 1) == active_)
            out->lVal |= STATE_SYSTEM_SELECTED;
        if (static_cast<size_t>(child - 1) >= tabRects_.size() || IsRectEmpty(&tabRects_[child - 1]))
            out->lVal |= STATE_SYSTEM_OFFSCREEN;
    }
    if (!enabled_ || !strip_)
        out->lVal |= STATE_SYSTEM_INVISIBLE;
    if (GetFocus() == strip_ && static_cast<size_t>(std::max(0L, child - 1)) == active_)
        out->lVal |= STATE_SYSTEM_FOCUSED;
    return S_OK;
}
HRESULT Controller::accLocation(long *x, long *y, long *w, long *h, VARIANT v)
{
    if (!x || !y || !w || !h)
        return E_POINTER;
    RECT r{};
    if (!ChildRect(v.vt == VT_I4 ? v.lVal : 0, r))
        return E_INVALIDARG;
    *x = r.left;
    *y = r.top;
    *w = r.right - r.left;
    *h = r.bottom - r.top;
    return S_OK;
}
HRESULT Controller::accNavigate(long direction, VARIANT from, VARIANT *out)
{
    if (!out)
        return E_POINTER;
    VariantInit(out);
    LONG child = from.vt == VT_I4 ? from.lVal : 0;
    LONG count = static_cast<LONG>(tabs_.size() + 2), next = 0;
    if (direction == NAVDIR_FIRSTCHILD)
        next = 1;
    else if (direction == NAVDIR_LASTCHILD)
        next = count;
    else if (direction == NAVDIR_NEXT || direction == NAVDIR_RIGHT)
        next = child + 1;
    else if (direction == NAVDIR_PREVIOUS || direction == NAVDIR_LEFT)
        next = child - 1;
    if (next < 1 || next > count)
        return S_FALSE;
    out->vt = VT_I4;
    out->lVal = next;
    return S_OK;
}
HRESULT Controller::accHitTest(long x, long y, VARIANT *out)
{
    if (!out)
        return E_POINTER;
    VariantInit(out);
    POINT p{x, y};
    RECT r{};
    if (!strip_ || !GetWindowRect(strip_, &r) || !PtInRect(&r, p))
        return S_FALSE;
    ScreenToClient(strip_, &p);
    int hit = Hit(p);
    out->vt = VT_I4;
    out->lVal = hit >= 0    ? hit + 1
                : hit == -2 ? static_cast<LONG>(tabs_.size() + 1)
                : hit == -3 ? static_cast<LONG>(tabs_.size() + 2)
                            : CHILDID_SELF;
    return S_OK;
}
HRESULT Controller::accDoDefaultAction(VARIANT v)
{
    if (v.vt != VT_I4 || !enabled_)
        return E_INVALIDARG;
    if (v.lVal > 0 && static_cast<size_t>(v.lVal) <= tabs_.size())
        return Activate(v.lVal - 1) ? S_OK : S_FALSE;
    if (v.lVal == static_cast<LONG>(tabs_.size() + 1))
    {
        NewTab();
        return S_OK;
    }
    if (v.lVal == static_cast<LONG>(tabs_.size() + 2))
    {
        POINT p{};
        GetCursorPos(&p);
        Menu(-3, p);
        return S_OK;
    }
    return E_INVALIDARG;
}

bool WriteReport(const wchar_t *path, const std::string &text)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
    FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}
HRESULT Controller::Proof(const wchar_t *report)
{
    if (!frame_ || !GetPropW(frame_, ProofProperty) || ownerThread_ != GetCurrentThreadId())
        return E_ACCESSDENIED;
    Remember();
    auto originalTabs = tabs_;
    auto originalActive = active_;
    Pidl original = CurrentFolder(browser_.Get());
    auto pump = [&] {
        ULONGLONG start = GetTickCount64();
        while (pending_ && GetTickCount64() - start < 5000)
        {
            MSG m{};
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        return !pending_;
    };
    size_t before = tabs_.size();
    NewTab(&original);
    bool created = pump() && tabs_.size() == before + 1 && Equal(CurrentFolder(browser_.Get()), original);
    bool correct = browser_ && enabled_ && GetParent(strip_) == frame_;
    bool serial = false;
    if (!tabs_.empty())
    {
        auto bytes = Serialize(tabs_[active_], frame_);
        serial = Deserialize(bytes.data(), bytes.size()).has_value();
    }
    bool toggled = false;
    Enable(false);
    bool removed = !GetPropW(frame_, patch::TabsActiveProperty) && !strip_;
    Enable(true);
    toggled = removed && enabled_ && strip_ && Equal(CurrentFolder(browser_.Get()), original);
    tabs_ = std::move(originalTabs);
    active_ = originalActive;
    pending_.reset();
    TabChanged();
    std::ostringstream out;
    out << "{\n  \"isolatedOwnedWindow\": true,\n  \"realShellBrowser\": " << (correct ? "true" : "false")
        << ",\n  \"newTabSameRealFolder\": " << (created ? "true" : "false")
        << ",\n  \"packetRoundTrip\": " << (serial ? "true" : "false")
        << ",\n  \"togglePreservesActiveFolder\": " << (toggled ? "true" : "false")
        << ",\n  \"cleanVmAcceptance\": false,\n  \"pixelExactScrollGuaranteed\": false\n}\n";
    return WriteReport(report, out.str()) ? (correct && created && serial && toggled ? S_OK : S_FALSE)
                                          : HRESULT_FROM_WIN32(GetLastError());
}

class Factory final : public IClassFactory
{
    std::atomic<ULONG> refs{1};

  public:
    Factory()
    {
        objectCount++;
    }
    ~Factory()
    {
        objectCount--;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override
    {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (id == IID_IUnknown || id == IID_IClassFactory)
            *out = static_cast<IClassFactory *>(this);
        if (!*out)
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++refs;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG n = --refs;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *outer, REFIID id, void **out) override
    {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (outer)
            return CLASS_E_NOAGGREGATION;
        auto *controller = new (std::nothrow) Controller();
        if (!controller)
            return E_OUTOFMEMORY;
        HRESULT hr = controller->QueryInterface(id, out);
        controller->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override
    {
        if (lock)
            serverLocks++;
        else
            serverLocks--;
        return S_OK;
    }
};
HRESULT RegistryString(HKEY hive, const std::wstring &path, const wchar_t *name, const std::wstring &value)
{
    HKEY key{};
    LONG status =
        RegCreateKeyExW(hive, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(status);
    status = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(status);
}
} // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
extern "C" HRESULT WINAPI DllCanUnloadNow()
{
    return objectCount.load() == 0 && serverLocks.load() == 0 ? S_OK : S_FALSE;
}
// The MVP starter installs this hook only on a verified Explorer folder thread.
// Creating the controller here keeps every COM and window operation in its STA.
extern "C" LRESULT CALLBACK MvpExplorerHook(int code, WPARAM removed, LPARAM data)
{
    if (code >= 0 && removed == PM_REMOVE && data)
    {
        auto *message = reinterpret_cast<MSG *>(data);
        if (message->message == patch::mvp::AttachMessage())
        {
            HWND frame = message->hwnd;
            message->message = WM_NULL;
            static thread_local bool attaching = false;
            DWORD process{};
            DWORD thread = GetWindowThreadProcessId(frame, &process);
            if (!attaching && LocalExplorer() && ExplorerRoot(frame) &&
                process == GetCurrentProcessId() && thread == GetCurrentThreadId() &&
                (patch::LoadConfig().flags & patch::ExplorerTabs) && !GetPropW(frame, ControllerProperty))
            {
                attaching = true;
                HRESULT result = E_NOINTERFACE;
                ComPtr<IShellWindows> windows;
                if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                               IID_PPV_ARGS(&windows))))
                {
                    long count{};
                    windows->get_Count(&count);
                    for (long i = 0; i < std::min(count, 256L); ++i)
                    {
                        VARIANT index{};
                        index.vt = VT_I4;
                        index.lVal = i;
                        ComPtr<IDispatch> dispatch;
                        ComPtr<IWebBrowser2> browser;
                        SHANDLE_PTR handle{};
                        if (FAILED(windows->Item(index, &dispatch)) || !dispatch ||
                            FAILED(dispatch.As(&browser)) || FAILED(browser->get_HWND(&handle)) ||
                            reinterpret_cast<HWND>(handle) != frame)
                            continue;
                        auto *controller = new (std::nothrow) Controller();
                        if (!controller) { result = E_OUTOFMEMORY; break; }
                        result = controller->SetSite(browser.Get());
                        if (SUCCEEDED(result) && GetPropW(frame, ControllerProperty) == controller)
                        {
                            HMODULE retained{};
                            // Window callbacks may outlive the starter. Retain this DLL
                            // until Explorer exits, including after an emergency detach.
                            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                      GET_MODULE_HANDLE_EX_FLAG_PIN,
                                                  reinterpret_cast<LPCWSTR>(&MvpExplorerHook), &retained))
                                controller->OwnMvpLifetime();
                            else
                            {
                                result = HRESULT_FROM_WIN32(GetLastError());
                                controller->SetSite(nullptr);
                                controller->Release();
                            }
                        }
                        else controller->Release();
                        break;
                    }
                }
                SetPropW(frame, patch::mvp::AttachResult,
                         reinterpret_cast<HANDLE>(SUCCEEDED(result) ? 1ull : static_cast<DWORD>(result)));
                attaching = false;
            }
        }
    }
    return CallNextHookEx(nullptr, code, removed, data);
}
extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID id, void **out)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;
    if (clsid != TabsClsid)
        return CLASS_E_CLASSNOTAVAILABLE;
    auto *factory = new (std::nothrow) Factory();
    if (!factory)
        return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(id, out);
    factory->Release();
    return hr;
}
extern "C" HRESULT WINAPI DllRegisterServer()
{
    if (!patch::SupportedWindows())
        return HRESULT_FROM_WIN32(ERROR_OLD_WIN_VERSION);
    const std::wstring clsid = L"Software\\Classes\\CLSID\\" + std::wstring(patch::TabsClassId);
    HRESULT hr = RegistryString(HKEY_LOCAL_MACHINE, clsid, nullptr, L"Windows 10 native Explorer tabs");
    if (FAILED(hr))
        return hr;
    if (FAILED(hr = RegistryString(HKEY_LOCAL_MACHINE, clsid + L"\\InprocServer32", nullptr, ModulePath())))
        return hr;
    if (FAILED(hr = RegistryString(HKEY_LOCAL_MACHINE, clsid + L"\\InprocServer32", L"ThreadingModel", L"Apartment")))
        return hr;
    const std::wstring bho = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Browser Helper Objects\\" +
                             std::wstring(patch::TabsClassId);
    if (FAILED(hr = RegistryString(HKEY_LOCAL_MACHINE, bho, nullptr, L"Windows 10 native Explorer tabs")))
        return hr;
    return RegistryString(HKEY_LOCAL_MACHINE,
                          L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved",
                          patch::TabsClassId, L"Windows 10 native Explorer tabs");
}
extern "C" HRESULT WINAPI DllUnregisterServer()
{
    const std::wstring clsid = L"Software\\Classes\\CLSID\\" + std::wstring(patch::TabsClassId);
    wchar_t current[32768]{};
    DWORD bytes = sizeof(current);
    LONG found = RegGetValueW(HKEY_LOCAL_MACHINE, (clsid + L"\\InprocServer32").c_str(), nullptr,
                              RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, current, &bytes);
    if (found == ERROR_FILE_NOT_FOUND || found == ERROR_PATH_NOT_FOUND)
        return S_OK;
    if (found != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(found);
    if (_wcsicmp(current, ModulePath().c_str()))
        return HRESULT_FROM_WIN32(ERROR_INVALID_OWNER);
    const std::wstring bho = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Browser Helper Objects\\" +
                             std::wstring(patch::TabsClassId);
    LONG status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, bho.c_str());
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
        return HRESULT_FROM_WIN32(status);
    HKEY approved{};
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved",
                      0, KEY_SET_VALUE | KEY_WOW64_64KEY, &approved) == ERROR_SUCCESS)
    {
        RegDeleteValueW(approved, patch::TabsClassId);
        RegCloseKey(approved);
    }
    status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, clsid.c_str());
    return status == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(status);
}
extern "C" HRESULT WINAPI RunProof(HWND frame, const wchar_t *report)
{
    if (!frame || !report || !LocalExplorer() || !ExplorerRoot(frame) || !GetPropW(frame, ProofProperty))
        return E_ACCESSDENIED;
    DWORD pid{};
    DWORD thread = GetWindowThreadProcessId(frame, &pid);
    if (pid != GetCurrentProcessId() || thread != GetCurrentThreadId())
        return RPC_E_WRONG_THREAD;
    auto *controller = static_cast<Controller *>(GetPropW(frame, ControllerProperty));
    if (!controller)
        return E_NOINTERFACE;
    controller->AddRef();
    HRESULT hr = controller->Proof(report);
    controller->Release();
    return hr;
}
// This entry point exercises only the bounded, window-free model and parser. It
// does not register the extension, attach to Explorer, or read/change settings.
extern "C" HRESULT WINAPI RunModelTests(const wchar_t *report)
{
    if (!report)
        return E_POINTER;
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::vector<std::pair<std::string, bool>> checks;
    USHORT desktop = 0;
    Tab t;
    ViewState state;
    state.folder = Pidl(reinterpret_cast<PCIDLIST_ABSOLUTE>(&desktop));
    t.history.push_back(state);
    t.title = L"Desktop";
    auto original = Serialize(t);
    auto roundtrip = Deserialize(original.data(), original.size());
    checks.emplace_back("valid_pidl_history_round_trip", roundtrip && roundtrip->history.size() == 1 &&
                                                             roundtrip->position == 0 &&
                                                             Equal(roundtrip->history[0].folder, state.folder));
    auto truncated = original;
    truncated.pop_back();
    checks.emplace_back("truncated_packet_rejected", !Deserialize(truncated.data(), truncated.size()));
    auto corrupt = original;
    corrupt[0] ^= 0xff;
    checks.emplace_back("invalid_magic_rejected", !Deserialize(corrupt.data(), corrupt.size()));
    corrupt = original;
    DWORD impossible = static_cast<DWORD>(MaxHistory + 1);
    memcpy(corrupt.data() + 32, &impossible, 4);
    checks.emplace_back("oversized_history_rejected", !Deserialize(corrupt.data(), corrupt.size()));
    corrupt = original;
    DWORD wrongPosition = 1;
    memcpy(corrupt.data() + 36, &wrongPosition, 4);
    checks.emplace_back("out_of_range_history_position_rejected", !Deserialize(corrupt.data(), corrupt.size()));
    corrupt = original;
    corrupt[44] = 1;
    checks.emplace_back("malformed_pidl_segment_rejected", !Deserialize(corrupt.data(), corrupt.size()));
    corrupt = original;
    corrupt.insert(corrupt.end(), 16, 0);
    checks.emplace_back("hglobal_allocation_padding_accepted", Deserialize(corrupt.data(), corrupt.size()).has_value());
    Writer writer;
    writer.bytes.resize(MaxPacket);
    writer.number(DWORD{1});
    checks.emplace_back("serializer_hard_memory_bound", !writer.ok && writer.bytes.size() == MaxPacket);
    // Exercise the asynchronous path with a message-only test window. No
    // Explorer window, registration or controller is involved in this check.
    WNDCLASSW testClass{};testClass.hInstance=module;testClass.lpfnWndProc=DefWindowProcW;
    testClass.lpszClassName=L"Win10NativePatch.MetadataModelTest.v2";
    ATOM atom=RegisterClassW(&testClass);
    HWND target=atom?CreateWindowExW(0,testClass.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,module,nullptr):nullptr;
    checks.emplace_back("metadata_message_only_target_created",target!=nullptr);
    ULONGLONG cookie=nextId++;bool queued=false,received=false;
    if(target)
    {
        RegisterMetadata(cookie,target);
        queued=QueueMetadata({cookie,t.id,1,state.folder});
        ULONGLONG deadline=GetTickCount64()+5000;
        while(queued&&!received&&GetTickCount64()<deadline)
        {
            MSG message{};
            while(PeekMessageW(&message,target,IconMessage,IconMessage,PM_REMOVE))
            {
                auto result=TakeMetadata(static_cast<ULONGLONG>(message.wParam),cookie);
                received=result&&result->job.tab==t.id&&result->job.generation==1&&Equal(result->job.folder,state.folder)&&!result->name.empty();
            }
            if(!received)MsgWaitForMultipleObjectsEx(0,nullptr,20,QS_POSTMESSAGE,MWMO_INPUTAVAILABLE);
        }
        CancelMetadata(cookie);
    }
    checks.emplace_back("metadata_job_queued_without_explorer",queued);
    checks.emplace_back("metadata_sta_returns_matching_generation",received);
    checks.emplace_back("metadata_cancel_rejects_stale_target",target&&!QueueMetadata({cookie,t.id,2,state.folder}));
    if(target)DestroyWindow(target);if(atom)UnregisterClassW(testClass.lpszClassName,module);
    bool success = std::all_of(checks.begin(), checks.end(), [](auto &c) { return c.second; });
    std::ostringstream out;
    out << "{\n  \"success\": " << (success ? "true" : "false")
        << ",\n  \"explorerModified\": false,\n  \"cleanVmAcceptance\": false,\n  \"checks\": [\n";
    for (size_t i = 0; i < checks.size(); i++)
        out << "    {\"name\": \"" << checks[i].first << "\", \"passed\": " << (checks[i].second ? "true" : "false")
            << "}" << (i + 1 < checks.size() ? "," : "") << "\n";
    out << "  ]\n}\n";
    bool written = WriteReport(report, out.str());
    if (SUCCEEDED(initialized))
        CoUninitialize();
    return written ? (success ? S_OK : S_FALSE) : HRESULT_FROM_WIN32(GetLastError());
}
