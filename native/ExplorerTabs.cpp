// Native Explorer tabs for Windows 10 22H2 x64.
// Copyright (c) 2026 Win10NativePatch contributors.
// COM/site and Shell view access patterns adapted from StExBar:
// Copyright (C) 2007-2022 Stefan Kueng. See vendor/stexbar/PROVENANCE.txt.
// This program is free software under GPL version 2 or, at your option, later.
// Distributed WITHOUT ANY WARRANTY. See vendor/stexbar/LICENSE.

#include "Patch.h"
#include "MvpHostProtocol.h"
#include "HeaderIntegration.h"
#include "TabHeaderRenderer.h"
#include "TabModel.h"
#include <algorithm>
#include <atomic>
#include <commctrl.h>
#include <cstring>
#include <deque>
#include <dwmapi.h>
#include <exdisp.h>
#include <exdispid.h>
#include <shdispid.h>
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
const wchar_t* TabWindowClass(){
    static const std::wstring name=L"Win10NativePatch.ExplorerTabs.v2."+std::to_wstring(reinterpret_cast<ULONG_PTR>(module));
    return name.c_str();
}
constexpr wchar_t ControllerProperty[] = L"Win10NativePatch.ExplorerController.v2";
constexpr wchar_t ProofProperty[] = L"Win10NativePatch.IsolatedProof.v2";
constexpr wchar_t SessionKey[] = L"Software\\Win10NativePatch\\v2\\ExplorerTabs";
constexpr wchar_t WindowSessionProperty[]=L"Win10NativePatch.WindowSession.v1";
constexpr UINT_PTR MaintenanceTimer = 0x57545032, SessionTimer = 0x57545033, ScrollTimer = 0x57545034,
    MotionTimer = 0x57545035;
constexpr UINT SyncMessage = WM_APP + 0x482;
constexpr UINT RestoreMessage = WM_APP + 0x483;
constexpr UINT IconMessage = WM_APP + 0x484;
constexpr size_t MaxTabs = 500, MaxHistory = 64, MaxSelection = 512, MaxPacket = 8 * 1024 * 1024;
// LastSession stays small enough to read and write without a visible pause.
constexpr size_t SessionBudget = 1024 * 1024, BackgroundHistory = 16;
constexpr UINT SessionDelayMs = 1500;
const CLSID TabsClsid = {0x73f7b7dc, 0xc022, 0x4862, {0xa3, 0xea, 0x7b, 0xd0, 0x4e, 0xae, 0x73, 0xa9}};
std::atomic<ULONGLONG> nextId{1};
std::atomic<bool> sessionRestored{false};
class Controller;
std::mutex controllersMutex;
std::unordered_map<HWND, Controller *> controllers;

// Diagnostic timing marks for lab measurements. Off unless the per-user
// HKCU\Software\Win10NativePatch\v2 TimingLog value names a file; release
// installs never set it. Lines are "<QPC microseconds> <event> <a> <b>", so a
// test process can append its own input marks to the same file on one clock.
namespace timing
{
HANDLE OpenLog()
{
    wchar_t path[MAX_PATH]{};
    DWORD bytes = sizeof(path);
    if (RegGetValueW(HKEY_CURRENT_USER, patch::SettingsKey, L"TimingLog", RRF_RT_REG_SZ, nullptr, path, &bytes) !=
            ERROR_SUCCESS ||
        !*path)
        return INVALID_HANDLE_VALUE;
    return CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}
HANDLE Log()
{
    static const HANDLE file = OpenLog();
    return file;
}
void Mark(const char *event, ULONGLONG a = 0, ULONGLONG b = 0)
{
    const HANDLE file = Log();
    if (file == INVALID_HANDLE_VALUE)
        return;
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const auto ticks = static_cast<ULONGLONG>(now.QuadPart), rate = static_cast<ULONGLONG>(frequency.QuadPart);
    const ULONGLONG micro = ticks / rate * 1000000 + ticks % rate * 1000000 / rate;
    char line[160]{};
    const int length = wsprintfA(line, "%I64u %s %I64u %I64u\r\n", micro, event, a, b);
    DWORD written{};
    if (length > 0)
        WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
}
} // namespace timing

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
            const ULONGLONG finished = result.job.cookie;
            auto target = metadataTargets.find(finished);
            if (target == metadataTargets.end())
                --metadataOutstanding;
            else
            {
                ULONGLONG ticket = nextId++;
                try
                {
                    auto inserted = metadataResults.emplace(ticket, std::move(result));
                    if (!PostMessageW(target->second, IconMessage, static_cast<WPARAM>(ticket),
                                      static_cast<LPARAM>(finished)))
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
                if(cookie!=finished)PostMessageW(window,IconMessage,0,static_cast<LPARAM>(cookie));
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
bool SameSort(IFolderView2 *fv, const std::vector<SORTCOLUMN> &wanted)
{
    int columns{};
    if (FAILED(fv->GetSortColumnCount(&columns)) || columns != static_cast<int>(wanted.size()))
        return false;
    std::vector<SORTCOLUMN> current(wanted.size());
    if (FAILED(fv->GetSortColumns(current.data(), columns)))
        return false;
    for (size_t i = 0; i < wanted.size(); ++i)
        if (current[i].direction != wanted[i].direction || !IsEqualPropertyKey(current[i].propkey, wanted[i].propkey))
            return false;
    return true;
}
// Each setter re-sorts, re-groups or re-lays out the native view, so values
// that already match are left alone.
void RestoreLayout(IFolderView2 *fv, const ViewState &s)
{
    FOLDERVIEWMODE mode{};
    int iconSize{};
    if (s.mode != FVM_AUTO && s.iconSize > 0 &&
        (FAILED(fv->GetViewModeAndIconSize(&mode, &iconSize)) || mode != s.mode || iconSize != s.iconSize))
        fv->SetViewModeAndIconSize(s.mode, s.iconSize);
    if (!s.sort.empty() && !SameSort(fv, s.sort))
        fv->SetSortColumns(s.sort.data(), static_cast<int>(s.sort.size()));
    PROPERTYKEY group{};
    BOOL ascending = TRUE;
    if (s.hasGroup && (FAILED(fv->GetGroupBy(&group, &ascending)) || !IsEqualPropertyKey(group, s.group) ||
                       ascending != s.groupAscending))
        fv->SetGroupBy(s.group, s.groupAscending);
}
// True when `item` (an index in the view) names the same child as `wanted`.
bool SameItem(IFolderView2 *fv, IShellFolder *folder, int item, const Pidl &wanted)
{
    PITEMID_CHILD child{};
    if (!wanted || FAILED(fv->Item(item, &child)))
        return false;
    const HRESULT order = folder->CompareIDs(SHCIDS_CANONICALONLY, child, reinterpret_cast<PCUITEMID_CHILD>(wanted.p));
    CoTaskMemFree(child);
    return SUCCEEDED(order) && HRESULT_CODE(order) == 0;
}
// Compare-before-set: the view already shows exactly the saved selection and
// focus, so a confirming restore pass changes nothing.
bool SelectionMatches(IFolderView2 *fv, IShellFolder *folder, const ViewState &s)
{
    int count{};
    if (!fv || !folder || FAILED(fv->ItemCount(SVGIO_SELECTION, &count)) ||
        static_cast<size_t>(count) != s.selected.size())
        return false;
    for (int start = 0, found = 0; found < count; ++found)
    {
        int item{};
        if (fv->GetSelectedItem(start, &item) != S_OK)
            return false;
        if (std::none_of(s.selected.begin(), s.selected.end(),
                         [&](const Pidl &wanted) { return SameItem(fv, folder, item, wanted); }))
            return false;
        start = item + 1;
    }
    int focused{};
    return !s.focused || (SUCCEEDED(fv->GetFocusedItem(&focused)) && SameItem(fv, folder, focused, s.focused));
}
// True when the items a restore needs are already listed by the view, so it
// can be applied without waiting for the view's enumeration event.
bool RestoreItemsListed(IShellView *view, const ViewState &s)
{
    const Pidl *probe = s.anchor ? &s.anchor : s.focused ? &s.focused : s.selected.empty() ? nullptr : &s.selected.front();
    if (!probe)
        return true;
    ComPtr<IFolderView> fv;
    ComPtr<IShellFolder> folder;
    int count{};
    if (!view || FAILED(view->QueryInterface(IID_PPV_ARGS(&fv))) || FAILED(fv->GetFolder(IID_PPV_ARGS(&folder))) ||
        FAILED(fv->ItemCount(SVGIO_ALLVIEW, &count)) || count <= 0 || count > 5000)
        return false;
    const auto wanted = reinterpret_cast<PCUITEMID_CHILD>(probe->p);
    for (int i = 0; i < count; ++i)
    {
        PITEMID_CHILD child{};
        if (FAILED(fv->Item(i, &child)))
            continue;
        const HRESULT order = folder->CompareIDs(SHCIDS_CANONICALONLY, child, wanted);
        CoTaskMemFree(child);
        if (SUCCEEDED(order) && HRESULT_CODE(order) == 0)
            return true;
    }
    return false;
}
// Receives DShellFolderViewEvents from one folder view. Kept apart from the
// controller's own IDispatch so its DISPIDs never meet DWebBrowserEvents2.
class ViewEvents final : public IDispatch
{
    std::atomic<ULONG> refs_{1};
    HWND target_{};
    UINT message_{};
    ComPtr<IConnectionPoint> point_;
    DWORD cookie_{};
    ViewEvents(HWND target, UINT message) : target_(target), message_(message) { objectCount++; }
    ~ViewEvents() { objectCount--; }

  public:
    static ComPtr<ViewEvents> Connect(IShellView *view, HWND target, UINT message)
    {
        ComPtr<IDispatch> automation;
        ComPtr<IConnectionPointContainer> container;
        ComPtr<ViewEvents> sink;
        if (!view || FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&automation))) ||
            FAILED(automation.As(&container)))
            return nullptr;
        sink.Attach(new (std::nothrow) ViewEvents(target, message));
        if (!sink || FAILED(container->FindConnectionPoint(DIID_DShellFolderViewEvents, &sink->point_)) ||
            FAILED(sink->point_->Advise(static_cast<IDispatch *>(sink.Get()), &sink->cookie_)))
            return nullptr;
        return sink;
    }
    void Disconnect()
    {
        target_ = nullptr;
        if (point_ && cookie_)
            point_->Unadvise(cookie_);
        cookie_ = 0;
        point_.Reset();
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override
    {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (id == IID_IUnknown || id == IID_IDispatch || id == DIID_DShellFolderViewEvents)
            *out = static_cast<IDispatch *>(this);
        if (!*out)
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG n = --refs_;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT *n) override
    {
        if (n)
            *n = 0;
        return n ? S_OK : E_POINTER;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR *, UINT, LCID, DISPID *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID id, REFIID, LCID, WORD, DISPPARAMS *, VARIANT *, EXCEPINFO *,
                                     UINT *) override
    {
        // Applied from the frame's queue, never inside the view's own event.
        if (target_ && (id == DISPID_FILELISTENUMDONE || id == DISPID_INITIALENUMERATIONDONE))
            PostMessageW(target_, message_, 0, 0);
        return S_OK;
    }
};
void Restore(IShellBrowser *browser, const ViewState &s)
{
    ComPtr<IShellView> view;
    ComPtr<IFolderView2> fv;
    if (!browser || !Equal(CurrentFolder(browser), s.folder) || FAILED(browser->QueryActiveShellView(&view)) ||
        FAILED(view.As(&fv)))
        return;
    RestoreLayout(fv.Get(), s);
    ComPtr<IShellFolder> folder;
    if (SUCCEEDED(fv->GetFolder(IID_PPV_ARGS(&folder))) && SelectionMatches(fv.Get(), folder.Get(), s))
        return;
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
    void state(const ViewState &s, bool selection = true)
    {
        static const Pidl none;
        pidl(s.folder);
        pidl(selection ? s.focused : none);
        pidl(selection ? s.anchor : none);
        number(static_cast<int>(s.mode));
        number(s.iconSize);
        number(static_cast<DWORD>(selection ? s.selected.size() : 0));
        if (selection)
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
    // Writes history entries [first, last) of the tab. With
    // `currentSelectionOnly`, only the current entry keeps its selection.
    void tab(const Tab &t, size_t first, size_t last, bool selection, bool currentSelectionOnly)
    {
        number(static_cast<DWORD>(last - first));
        number(static_cast<DWORD>(t.position - first));
        for (size_t i = first; i < last; ++i)
            state(t.history[i], selection && (!currentSelectionOnly || i == t.position));
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
// Encodes LastSession within `budget`, giving up detail in steps so every open
// tab survives: full history; background tabs trimmed to their current entry
// plus BackgroundHistory neighbours, selections only on current entries;
// current entries only, selection kept for the active tab; bare current
// folders. Returns empty when even the bare form does not fit.
std::vector<BYTE> EncodeSession(const std::vector<Tab> &tabs, size_t active, size_t budget)
{
    enum class Detail { Full, Background, Current, Bare };
    for (const auto detail : {Detail::Full, Detail::Background, Detail::Current, Detail::Bare})
    {
        Writer writer;
        writer.number(PacketMagic);
        writer.number(DWORD{3});
        writer.number(static_cast<DWORD>(tabs.size()));
        writer.number(static_cast<DWORD>(active));
        for (size_t i = 0; i < tabs.size() && writer.ok && writer.bytes.size() <= budget; ++i)
        {
            const Tab &tab = tabs[i];
            size_t first = 0, last = tab.history.size();
            if (detail == Detail::Background && i != active)
            {
                // Keep a window of BackgroundHistory+1 entries around the
                // current one, preferring older entries (Back is used more).
                first = tab.position > BackgroundHistory ? tab.position - BackgroundHistory : 0;
                last = std::min(last, first + BackgroundHistory + 1);
            }
            else if (detail == Detail::Current || detail == Detail::Bare)
            {
                first = tab.position;
                last = tab.position + 1;
            }
            const bool selection = detail == Detail::Full || detail == Detail::Background ||
                                   (detail == Detail::Current && i == active);
            writer.tab(tab, first, last, selection, detail != Detail::Full);
        }
        if (writer.ok && writer.bytes.size() <= budget)
            return std::move(writer.bytes);
    }
    return {};
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
    HWND frame_{}, strip_{}, travel_{};
    HHOOK keyboard_{};
    bool headerAttached_=false,layingOut_=false,hasUsableSession_=false,sessionDirty_=false;
    RECT placed_{};
    // Overflow scrolling and Firefox's close-with-mouse width lock.
    int scroll_{}, lockedWidth_{};
    ULONGLONG revealedId_{};
    HWND tooltip_{};
    header::TargetId tipTarget_{};
    DWORD ownerThread_{};
    bool enabled_ = false, tearingDown_ = false, inDrag_ = false, dropPrivate_ = false, sameWindowDrop_ = false;
    bool frameHooked_ = false, selfHosted_ = false, legacyHidden_ = false, closingFrame_ = false;
    HICON folderIcon_{};
    UINT dpi_ = 96;
    header::LayoutState layout_;
    header::TabHeaderRenderer renderer_;
    bool rendererReady_=false;
    bool rendering_=false;
    std::uint64_t renderEpoch_{};
    header::TargetId hotTarget_,pressedTarget_,focusedTarget_;
    UINT pressedButton_{};
    unsigned menuDepth_{};
    HMENU systemMenu_{};
    UINT focusMenuId_{};
    POINT dragStart_{};
    // Local reorder (Firefox): the pressed tab follows the pointer inside the
    // strip while the others slide aside; leaving the strip vertically hands
    // the tab to OLE drag and drop. dragOrigin_ restores the order on cancel.
    bool reordering_ = false, motion_ = true;
    ULONGLONG dragTab_{};
    size_t dragOrigin_{};
    int dragX_{}, dragGrab_{};
    POINT dragPointer_{};
    bool dragScrolling_ = false;
    int backCommand_ = -1, forwardCommand_ = -1;
    std::vector<Tab> tabs_, closed_;
    size_t active_{}; // The tab the Shell view shows (committed).
    tabs::SwitchModel switching_;
    bool busyShown_ = false;
    // One restore per arrival: applied when the view lists its items, at the
    // fallback deadline, or never if the user acts in the view first.
    std::optional<ViewState> restoreState_;
    ULONGLONG restoreDeadline_{};
    HWND restoreView_{};
    ComPtr<ViewEvents> viewEvents_;
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
        HWND window{};
        ComPtr<IWebBrowser2> created;
        Pidl expected;
        std::vector<BYTE> packet;
        bool transferred{},closeRequested{};
    };
    std::optional<Detached> detached_;
    ComPtr<IDropTarget> folderDrop_;
    ComPtr<IDataObject> dropObject_;
    int hoverDrop_ = -1;
    ULONGLONG hoverStart_{};
    bool startupPending_ = false;
    ULONGLONG startupStart_{};
    ULONGLONG headerRetryDeadline_{};
    static LRESULT CALLBACK StripProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK FrameProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK TravelProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK KeyProc(int, WPARAM, LPARAM);
    bool Attach();
    void Detach();
    void Enable(bool);
    void Layout(bool transient=false);
    void Paint();
    bool Render();
    void HeaderFailed();
    void RefreshAppearance();
    void RefreshTravel();
    void UpdateNavigationButtons();
    void Remember();
    void Synchronize();
    bool Navigate(ULONGLONG, size_t, bool, bool remember = true);
    bool Activate(size_t);
    bool FollowRequest(ULONGLONG);
    void DrainCommands();
    bool Defer(tabs::Command);
    void MoveTab(int);
    void ScrollBy(int pixels);
    void ScrollRepeat(bool first);
    void Shift(size_t from, size_t to);
    void RefreshMotion();
    bool InReorderBand(POINT) const;
    void BeginReorder(POINT);
    void Reorder(POINT);
    void EndReorder(bool commit);
    void ReorderScroll();
    void RefreshHot();
    void Tooltip(header::TargetId target);
    std::wstring TooltipText(header::TargetId target) const;
    void BeginRestore(const ViewState &);
    void ApplyRestore();
    void CancelRestore();
    bool IsViewInput(const MSG &) const;
    bool Shortcut(UINT key, bool ctrl, bool shift, bool alt, bool repeated);
    bool BackspaceTravels(bool repeated);
    void RetryHeader();
    void NavigationDuty();
    void DetachDuty();
    ULONGLONG CommittedId() const { return active_ < tabs_.size() ? tabs_[active_].id : 0; }
    ULONGLONG DisplayedId() const { return switching_.Displayed(CommittedId()); }
    size_t DisplayedIndex() const { const int i = Index(DisplayedId()); return i >= 0 ? static_cast<size_t>(i) : active_; }
    void NewTab(const Pidl *folder = nullptr);
    void Close(size_t,const std::vector<BYTE>* expected=nullptr);
    void Reopen();
    void History(int);
    void HistoryMenu(bool, POINT);
    void Menu(int, POINT);
    void LimitMemory();
    void SaveSession();
    void FlushSession();
    void LoadSession();
    bool SaveWindowSession();
    bool LoadWindowSession();
    void RemoveWindowSession();
    void TabChanged();
    void RefreshMetadata();
    void AcceptMetadata(ULONGLONG);
    void Timer();
    void StartDrag(size_t);
    void DetachTab(size_t);
    bool Import(Tab, bool);
    int Hit(POINT) const;
    int Index(ULONGLONG id) const {for(size_t i=0;i<tabs_.size();++i)if(tabs_[i].id==id)return static_cast<int>(i);return -1;}
    bool Ready() const {return enabled_&&!tearingDown_&&strip_&&rendererReady_&&IsWindowVisible(strip_)&&header::IsCurrent(frame_,layout_.allocation);}
    void CancelPointer();
    void SystemMenu(bool);
    void FocusTarget(header::TargetId);
    POINT MenuAnchor(header::TargetId) const;
    void InvokeTarget(header::TargetId);
    header::TargetId ChildTarget(LONG) const;
    LONG TargetChild(header::TargetId) const;
    bool ValidChild(VARIANT v) const {return v.vt==VT_I4&&v.lVal>=0&&v.lVal<=static_cast<LONG>(tabs_.size()*2+2);}
    void Mouse(UINT, WPARAM, LPARAM);
    bool BeginStartupGuard();
    void CompleteStartupGuard();
    void LegacyBand(bool);
    void BindFolderDrop();
    HRESULT ChildName(LONG, BSTR *);
    bool ChildRect(LONG, RECT &) const;
    HRESULT AccessTypeInfo();

  public:
    void OwnMvpLifetime() { selfHosted_ = true; if(enabled_)LegacyBand(true); }
    Controller()
    {
        objectCount++;
    }
    ~Controller()
    {
        CancelMetadata(metadataCookie_);
        Detach();
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
        *n = static_cast<LONG>(tabs_.size()*2 + 2);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT, IDispatch **) override;
    HRESULT STDMETHODCALLTYPE get_accName(VARIANT v, BSTR *p) override
    {
        return ValidChild(v)?ChildName(v.lVal,p):E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE get_accValue(VARIANT v, BSTR *p) override
    {
        if (!p)
            return E_POINTER;
        *p = nullptr;
        return ValidChild(v)?S_FALSE:E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT v, BSTR *p) override
    {
        if (!p)
            return E_POINTER;
        *p=nullptr;if(!ValidChild(v))return E_INVALIDARG;if(v.lVal)return S_FALSE;
        *p = SysAllocString(L"Folders in this File Explorer window");return *p?S_OK:E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE get_accRole(VARIANT v, VARIANT *p) override
    {
        if (!p)
            return E_POINTER;
        VariantInit(p);
        if(!ValidChild(v))return E_INVALIDARG;
        p->vt = VT_I4;
        p->lVal = v.lVal > 0
                      ? (ChildTarget(v.lVal).kind==header::TargetKind::Tab ? ROLE_SYSTEM_PAGETAB : ROLE_SYSTEM_PUSHBUTTON)
                      : ROLE_SYSTEM_PAGETABLIST;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accState(VARIANT, VARIANT *) override;
    HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT v, BSTR *p) override
    {
        if (!p)
            return E_POINTER;
        *p = nullptr;
        return ValidChild(v)?S_FALSE:E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR *p, VARIANT v, long *n) override
    {
        if (p)
            *p = nullptr;
        if (n)
            *n = 0;
        return ValidChild(v)?S_FALSE:E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT v, BSTR *p) override
    {
        if (!p)
            return E_POINTER;
        *p=nullptr;if(!ValidChild(v))return E_INVALIDARG;
        const auto target=ChildTarget(v.lVal);std::wstring text;
        if(target.kind==header::TargetKind::NewTab)text=L"Ctrl+T";
        if(target.kind==header::TargetKind::Tab){const int index=Index(target.tab);if(index<8)text=L"Ctrl+"+std::to_wstring(index+1);else if(index==static_cast<int>(tabs_.size()-1))text=L"Ctrl+9";}
        if(target.kind==header::TargetKind::Close&&target.tab==DisplayedId())text=L"Ctrl+W";
        if(text.empty())return S_FALSE;*p=SysAllocString(text.c_str());return *p?S_OK:E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT *p) override
    {
        if (!p)
            return E_POINTER;
        VariantInit(p);
        if (GetFocus() == strip_)
        {
            p->vt = VT_I4;
            p->lVal = TargetChild(focusedTarget_);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT *p) override
    {
        if (!p)
            return E_POINTER;
        VariantInit(p);
        if(const auto shown=DisplayedId()){p->vt = VT_I4;p->lVal=TargetChild({header::TargetKind::Tab,shown});}
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT v, BSTR *p) override
    {
        if (!p)
            return E_POINTER;
        *p=nullptr;if(!ValidChild(v))return E_INVALIDARG;if(!v.lVal)return S_FALSE;
        const auto target=ChildTarget(v.lVal);
        *p = SysAllocString(target.kind==header::TargetKind::Tab?L"Switch":target.kind==header::TargetKind::Close?L"Close":L"Press");
        return *p?S_OK:E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE accSelect(long, VARIANT) override;
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
    LONG AccessibleChild(header::TargetId target) const {return TargetChild(target);}
};

// A retained child binds to the model identity. Its positional MSAA child ID
// may change as siblings close, but it can never become a neighboring tab.
class AccessibleTarget final : public IAccessible
{
    std::atomic<ULONG> refs_{1};Controller* owner_;header::TargetId target_;
    HRESULT Resolve(VARIANT input,VARIANT& child) const {VariantInit(&child);if(input.vt!=VT_I4||input.lVal!=CHILDID_SELF)return E_INVALIDARG;child.vt=VT_I4;child.lVal=owner_->AccessibleChild(target_);return child.lVal?S_OK:E_INVALIDARG;}
public:
    AccessibleTarget(Controller* owner,header::TargetId target):owner_(owner),target_(target){owner_->AddRef();}
    ~AccessibleTarget(){owner_->Release();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {if(!out)return E_POINTER;*out=nullptr;if(id!=IID_IUnknown&&id!=IID_IDispatch&&id!=IID_IAccessible)return E_NOINTERFACE;*out=static_cast<IAccessible*>(this);AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef() override {return ++refs_;}ULONG STDMETHODCALLTYPE Release() override {ULONG n=--refs_;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* out) override {return owner_->GetTypeInfoCount(out);}
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT i,LCID l,ITypeInfo** out) override {return owner_->GetTypeInfo(i,l,out);}
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID id,LPOLESTR* n,UINT c,LCID l,DISPID* out) override {return owner_->GetIDsOfNames(id,n,c,l,out);}
    HRESULT STDMETHODCALLTYPE Invoke(DISPID id,REFIID,LCID,WORD flags,DISPPARAMS* p,VARIANT* out,EXCEPINFO* e,UINT* a) override {ComPtr<ITypeInfo> type;HRESULT hr=GetTypeInfo(0,LOCALE_SYSTEM_DEFAULT,&type);return SUCCEEDED(hr)?DispInvoke(static_cast<IAccessible*>(this),type.Get(),id,flags,p,out,e,a):hr;}
    HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** out) override {if(!out)return E_POINTER;*out=static_cast<IAccessible*>(owner_);owner_->AddRef();return S_OK;}
    HRESULT STDMETHODCALLTYPE get_accChildCount(long* out) override {if(!out)return E_POINTER;*out=0;return S_OK;}
    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT,IDispatch** out) override {if(!out)return E_POINTER;*out=nullptr;return S_FALSE;}
#define FORWARD_ACCESS(method,type) HRESULT STDMETHODCALLTYPE method(VARIANT input,type* out) override {VARIANT child{};HRESULT hr=Resolve(input,child);return SUCCEEDED(hr)?owner_->method(child,out):hr;}
    FORWARD_ACCESS(get_accName,BSTR)
    FORWARD_ACCESS(get_accValue,BSTR)
    FORWARD_ACCESS(get_accDescription,BSTR)
    FORWARD_ACCESS(get_accRole,VARIANT)
    FORWARD_ACCESS(get_accState,VARIANT)
    FORWARD_ACCESS(get_accHelp,BSTR)
    FORWARD_ACCESS(get_accKeyboardShortcut,BSTR)
    FORWARD_ACCESS(get_accDefaultAction,BSTR)
#undef FORWARD_ACCESS
    HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR* out,VARIANT input,long* topic) override {VARIANT child{};HRESULT hr=Resolve(input,child);return SUCCEEDED(hr)?owner_->get_accHelpTopic(out,child,topic):hr;}
    HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT* out) override {if(!out)return E_POINTER;VariantInit(out);VARIANT focus{};owner_->get_accFocus(&focus);const auto child=owner_->AccessibleChild(target_);if(child&&focus.vt==VT_I4&&focus.lVal==child){out->vt=VT_I4;out->lVal=CHILDID_SELF;}return S_OK;}
    HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT* out) override {if(!out)return E_POINTER;VariantInit(out);return S_FALSE;}
    HRESULT STDMETHODCALLTYPE accSelect(long flags,VARIANT input) override {VARIANT child{};HRESULT hr=Resolve(input,child);return SUCCEEDED(hr)?owner_->accSelect(flags,child):hr;}
    HRESULT STDMETHODCALLTYPE accLocation(long* x,long* y,long* w,long* h,VARIANT input) override {VARIANT child{};HRESULT hr=Resolve(input,child);return SUCCEEDED(hr)?owner_->accLocation(x,y,w,h,child):hr;}
    HRESULT STDMETHODCALLTYPE accNavigate(long direction,VARIANT input,VARIANT* out) override {if(!out)return E_POINTER;VariantInit(out);VARIANT child{},next{};HRESULT hr=Resolve(input,child);if(FAILED(hr))return hr;hr=owner_->accNavigate(direction,child,&next);if(hr!=S_OK)return hr;ComPtr<IDispatch> sibling;hr=owner_->get_accChild(next,&sibling);if(SUCCEEDED(hr)&&sibling){out->vt=VT_DISPATCH;out->pdispVal=sibling.Detach();}return hr;}
    HRESULT STDMETHODCALLTYPE accHitTest(long x,long y,VARIANT* out) override {if(!out)return E_POINTER;VariantInit(out);VARIANT hit{};HRESULT hr=owner_->accHitTest(x,y,&hit);const auto child=owner_->AccessibleChild(target_);if(child&&hr==S_OK&&hit.vt==VT_I4&&hit.lVal==child){out->vt=VT_I4;out->lVal=CHILDID_SELF;return S_OK;}return S_FALSE;}
    HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT input) override {VARIANT child{};HRESULT hr=Resolve(input,child);return SUCCEEDED(hr)?owner_->accDoDefaultAction(child):hr;}
    HRESULT STDMETHODCALLTYPE put_accName(VARIANT,BSTR) override {return E_ACCESSDENIED;}
    HRESULT STDMETHODCALLTYPE put_accValue(VARIANT,BSTR) override {return E_ACCESSDENIED;}
};
HRESULT Controller::get_accChild(VARIANT child,IDispatch** out)
{
    if(!out)return E_POINTER;*out=nullptr;
    if(!ValidChild(child))return E_INVALIDARG;if(!child.lVal)return S_FALSE;
    *out=new(std::nothrow) AccessibleTarget(this,ChildTarget(child.lVal));return *out?S_OK:E_OUTOFMEMORY;
}

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
    if (frame_ && ownerThread_ != GetCurrentThreadId())
    {
        // COM apartment ownership is a hard boundary, not an invitation to touch
        // another window thread. SetSite(NULL) is expected on the owning STA.
        patch::Log(L"ExplorerTabs", L"Rejected teardown from a different apartment.");
        return;
    }
    tearingDown_ = true;
    CancelMetadata(metadataCookie_);
    headerRetryDeadline_=0;
    if (!tabs_.empty())
        FlushSession();
    Enable(false);
    if (events_ && eventCookie_)
        events_->Unadvise(eventCookie_);
    eventCookie_ = 0;
    events_.Reset();
    if (frame_ && frameHooked_)
    {
        KillTimer(frame_, MaintenanceTimer);
        KillTimer(frame_, SessionTimer);
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
        wc.lpszClassName = TabWindowClass();
        wc.style = CS_DBLCLKS;
        RegisterClassExW(&wc);
        dpi_ = GetDpiForWindow(frame_);
        RefreshAppearance();
        strip_ = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_NOREDIRECTIONBITMAP, TabWindowClass(), L"File Explorer tabs",
                                 WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS, 0, 0, 0, 0, frame_, nullptr,
                                 module, this);
        if (!strip_)
        {
            CompleteStartupGuard();
            return;
        }
        headerAttached_=header::Attach(frame_);
        if(!headerAttached_){DestroyWindow(strip_);strip_=nullptr;CompleteStartupGuard();SetPropW(frame_,L"Win10NativePatch.TabsRejected.v2",reinterpret_cast<HANDLE>(3));return;}
        enabled_ = true;
        SystemMenu(true);
        RegisterMetadata(metadataCookie_, frame_);
        SetPropW(frame_, patch::TabsActiveProperty, this);
        HRESULT drop = RegisterDragDrop(strip_, static_cast<IDropTarget *>(this));
        if (FAILED(drop))
            patch::Log(L"ExplorerTabs", L"OLE tab-strip drop target registration failed: " + std::to_wstring(drop));
        keyboard_ = SetWindowsHookExW(WH_GETMESSAGE, KeyProc, nullptr, ownerThread_);
        RefreshTravel();
        Layout();
        if(!enabled_||!strip_)return;
        headerRetryDeadline_=0;
        hasUsableSession_=true;
        if(selfHosted_)LegacyBand(true);
        if (!LoadWindowSession()&&(patch::LoadConfig().flags & patch::RestoreTabs) && !sessionRestored.exchange(true))
            LoadSession();
        RefreshMetadata();
        NotifyWinEvent(EVENT_OBJECT_CREATE, strip_, OBJID_CLIENT, CHILDID_SELF);
    }
    else
    {
        // Publish cancellation before any Shell/COM cleanup can reenter us.
        enabled_=false;
        // A tab dragged when the header goes away returns to its place, so
        // the flushed session keeps the order the user last confirmed.
        if(reordering_)EndReorder(false);
        SystemMenu(false);
        if(menuDepth_)EndMenu();
        rendererReady_=false;
        hotTarget_={};pressedTarget_={};pressedButton_=0;
        if(strip_){ShowWindow(strip_,SW_HIDE);if(GetCapture()==strip_)ReleaseCapture();}
        renderer_.Reset();
        LegacyBand(false);
        CancelMetadata(metadataCookie_);
        for(auto& t:tabs_)t.metadataPending=false;
        Remember();
        FlushSession();
        pending_.reset();
        switching_.Abandon();
        busyShown_ = false;
        detached_.reset();
        CancelRestore();
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
        enabled_=false;
        layout_={};
        if(headerAttached_){header::Detach(frame_,!closingFrame_);headerAttached_=false;}
        if (tooltip_)
        {
            DestroyWindow(tooltip_);
            tooltip_ = nullptr;
        }
        tipTarget_ = {};
        lockedWidth_ = 0;
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
void Controller::RefreshAppearance()
{
    renderer_.ThemeChanged();
    RefreshMotion();
    if (!folderIcon_)
    {
        SHSTOCKICONINFO info{sizeof(info)};
        if (SUCCEEDED(SHGetStockIconInfo(SIID_FOLDER, SHGSI_ICON | SHGSI_SMALLICON, &info)))
            folderIcon_ = info.hIcon;
    }
}
void Controller::LegacyBand(bool hide)
{
    if(!automation_)return;
    if(hide)
    {
        DWORD owned{},size=sizeof(owned);
        if(legacyHidden_ || RegGetValueW(HKEY_CURRENT_USER,patch::SettingsKey,L"MvpReplaceOwnedQt",
                RRF_RT_REG_DWORD,nullptr,&owned,&size)!=ERROR_SUCCESS || owned!=1)return;
        bool visible=false;
        EnumChildWindows(frame_,[](HWND child,LPARAM value)->BOOL{
            wchar_t name[128]{},parent[64]{};
            GetClassNameW(child,name,128);GetClassNameW(GetParent(child),parent,64);
            if(IsWindowVisible(child)&&wcsncmp(name,L"WindowsForms10.Window.",22)==0&&
               wcscmp(parent,L"ReBarWindow32")==0){*reinterpret_cast<bool*>(value)=true;return FALSE;}
            return TRUE;
        },reinterpret_cast<LPARAM>(&visible));
        if(!visible)return;
    }
    else if(!legacyHidden_ || closingFrame_)return;
    VARIANT clsid{},show{},size{};
    clsid.vt=VT_BSTR;clsid.bstrVal=SysAllocString(L"{d2bf470e-ed1c-487f-a333-2bd8835eb6ce}");
    if(!clsid.bstrVal)return;
    show.vt=VT_BOOL;show.boolVal=hide?VARIANT_FALSE:VARIANT_TRUE;
    HRESULT result=automation_->ShowBrowserBar(&clsid,&show,&size);
    VariantClear(&clsid);
    if(SUCCEEDED(result))
    {
        legacyHidden_=hide;
        if(hide)SetPropW(frame_,L"Win10NativePatch.LegacyQtHidden.v1",reinterpret_cast<HANDLE>(1));
        else RemovePropW(frame_,L"Win10NativePatch.LegacyQtHidden.v1");
    }
}
void Controller::Layout(bool transient)
{
    if (!enabled_ || !strip_ || layingOut_)
        return;
    struct LayoutGuard {bool& active;LayoutGuard(bool& value):active(value){active=true;}~LayoutGuard(){active=false;}} layoutGuard(layingOut_);
    if(IsIconic(frame_))return;
    UINT now = GetDpiForWindow(frame_);
    if (now != dpi_)
    {
        dpi_ = now;
        RefreshAppearance();
    }
    const int space=MulDiv(30,static_cast<int>(dpi_),96);
    header::Allocation allocation;
    if(!header::Allocate(frame_,strip_,MulDiv(92,static_cast<int>(dpi_),96)+space*2,allocation,transient)){
        patch::Log(L"ExplorerTabs",L"Native header allocation was unavailable; ordinary Explorer chrome was restored.");
        SetPropW(frame_,L"Win10NativePatch.TabsRejected.v2",reinterpret_cast<HANDLE>(4));
        if(!tearingDown_&&!closingFrame_&&!headerRetryDeadline_)headerRetryDeadline_=GetTickCount64()+2000;
        Enable(false);
        if(headerRetryDeadline_)SetTimer(frame_,MaintenanceTimer,100,nullptr);
        return;
    }
    timing::Mark("layout", transient ? 1 : 0);
    std::vector<std::uint64_t> identities;
    identities.reserve(tabs_.size());
    for(const auto& tab:tabs_)identities.push_back(tab.id);
    const auto shown=DisplayedId();
    // A newly shown tab scrolls into view; otherwise the user's scroll stays.
    auto next=header::BuildLayout(allocation,identities,shown,{scroll_,lockedWidth_,shown!=revealedId_});
    // When the row starts to overflow (a narrower window, more tabs), the shown
    // tab is scrolled into view, as Firefox does on its overflow event.
    if(next.overflowing&&!layout_.overflowing&&shown==revealedId_&&!reordering_)
        next=header::BuildLayout(allocation,identities,shown,{scroll_,lockedWidth_,true});
    revealedId_=shown;scroll_=next.scroll;
    const RECT area=allocation.bounds;
    // Moving the frame reflows with identical bounds; the child moves with it.
    if(!EqualRect(&area,&placed_)||!IsWindowVisible(strip_))
        SetWindowPos(strip_, HWND_TOP,area.left,area.top,area.right-area.left,area.bottom-area.top,SWP_NOACTIVATE);
    placed_=area;
    // A synchronous native reflow can invalidate a candidate while positioning
    // its child. Consumers reject that candidate until the queued update runs.
    layout_=std::move(next);
    ++renderEpoch_;
    rendererReady_=false;
    if(header::IsCurrent(frame_,layout_.allocation)) {
        if(!Render()){HeaderFailed();return;}
        if(rendererReady_)ShowWindow(strip_,SW_SHOWNOACTIVATE);
    } else ShowWindow(strip_,SW_HIDE);
    if(strip_)InvalidateRect(strip_, nullptr, FALSE);
}
void Controller::HeaderFailed()
{
    if(!enabled_||!frame_)return;
    rendererReady_=false;
    if(strip_)ShowWindow(strip_,SW_HIDE);
    SetPropW(frame_,L"Win10NativePatch.TabsRejected.v2",reinterpret_cast<HANDLE>(5));
    if(!tearingDown_&&!closingFrame_&&!headerRetryDeadline_)headerRetryDeadline_=GetTickCount64()+2000;
    Enable(false);
    if(headerRetryDeadline_)SetTimer(frame_,MaintenanceTimer,100,nullptr);
}
bool Controller::Render()
{
    if(!enabled_||!strip_)return true;
    if(rendering_)return true;
    struct DrawGuard{bool& busy;DrawGuard(bool& value):busy(value){busy=true;}~DrawGuard(){busy=false;}} drawGuard(rendering_);
    if(!header::IsCurrent(frame_,layout_.allocation)) {
        rendererReady_=false;ShowWindow(strip_,SW_HIDE);return true;
    }
    const HWND window=strip_,frame=frame_;
    const auto epoch=renderEpoch_;
    header::VisualState visual;
    visual.layout=layout_;
    visual.active=GetForegroundWindow()==frame||GetActiveWindow()==frame;
    visual.focused=GetFocus()==window;
    visual.busyTab=busyShown_?DisplayedId():0;
    visual.canCreate=tabs_.size()<MaxTabs;
    visual.hot=hotTarget_;visual.pressed=pressedTarget_;visual.focus=focusedTarget_;
    BOOL systemMotion=TRUE;
    if(!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&systemMotion,0))systemMotion=TRUE;
    visual.animate=motion_&&systemMotion;
    if(reordering_){visual.dragTab=dragTab_;visual.dragX=dragX_;}
    // Keep metadata-owned HICONs alive across graphics/COM calls and use one
    // copied model/layout for the complete draw.
    std::vector<std::shared_ptr<Icon>> heldIcons;
    for(const auto& tab:tabs_) {
        heldIcons.push_back(tab.icon);
        visual.tabs.push_back({tab.id,tab.metadataGeneration,tab.title,tab.icon?tab.icon->handle:folderIcon_});
    }
    const HRESULT result=renderer_.Paint(window,visual);
    if(result==S_OK)timing::Mark("render", epoch, visual.layout.active);
    if(!enabled_||strip_!=window||frame_!=frame)return true;
    if(FAILED(result)) {
        rendererReady_=false;
        if(strip_==window)ShowWindow(window,SW_HIDE);
        patch::Log(L"ExplorerTabs",L"Owned header composition failed: "+std::to_wstring(result));
        return false;
    }
    // Paint once more when the compositor's animations end so closed tabs
    // and surfaces scrolled out of view are released.
    if(const unsigned pending=renderer_.PendingMs()){
        if(result==S_OK)timing::Mark("motion", pending, epoch);
        SetTimer(window,MotionTimer,pending+16,nullptr);
    }
    rendererReady_=enabled_&&strip_==window&&frame_==frame&&epoch==renderEpoch_&&
        layout_.allocation.owner==visual.layout.allocation.owner&&
        layout_.allocation.revision==visual.layout.allocation.revision&&
        header::IsCurrent(frame,visual.layout.allocation);
    if(!rendererReady_)PostMessageW(frame,header::ChangedMessage(),0,0);
    return true;
}
void Controller::Paint()
{
    const HWND window=strip_;
    PAINTSTRUCT paint{};
    BeginPaint(window,&paint);
    const bool success=Render();
    EndPaint(window,&paint);
    if(!success)HeaderFailed();
}
void Controller::Remember()
{
    // Until a pending restore runs, the view does not show this tab's state
    // yet; the saved entry stays authoritative.
    if (tabs_.empty() || active_ >= tabs_.size() || pending_ || restoreState_)
        return;
    const auto id=tabs_[active_].id;
    ComPtr<IShellBrowser> browser=browser_;
    auto s = Capture(browser.Get());
    const int index=Index(id);
    if(index<0||pending_)return;
    auto &t = tabs_[index];

    if (!t.history.empty() && Equal(s.folder, t.history[t.position].folder))
    {
        s.label=t.history[t.position].label;
        t.history[t.position] = std::move(s);
    }
}
bool Controller::Navigate(ULONGLONG id, size_t position, bool changeTab, bool remember)
{
    if (pending_ || !browser_)
        return false;
    int index=Index(id);
    if (index<0 || position >= tabs_[index].history.size())
        return false;
    if(remember)Remember();
    CancelRestore();
    index=Index(id);
    if(!enabled_||!browser_||pending_||index<0||position>=tabs_[index].history.size())return false;
    Pending pending;
    pending.id = id;
    pending.position = position;
    pending.changeTab = changeTab;
    pending.expected = tabs_[index].history[position].folder;
    pending.start = GetTickCount64();
    pending_ = std::move(pending);
    busyShown_ = false;
    // The strip shows the destination before the Shell starts building the
    // new view on this thread; the composition commit does not wait for it.
    if(changeTab){switching_.Request(id,CommittedId(),0);Layout();}
    if(!pending_||pending_->id!=id)return false;
    const Pidl destination=pending_->expected;
    ComPtr<IShellBrowser> browser=browser_;
    timing::Mark("browse", id, position);
    HRESULT hr = browser->BrowseObject(destination.p,
                                        SBSP_SAMEBROWSER | SBSP_ABSOLUTE | SBSP_CREATENOHISTORY | SBSP_PLAYNOSOUND);
    if (FAILED(hr))
    {
        if(pending_&&pending_->id==id)pending_.reset();
        switching_.Forget(id);
        Layout();
        patch::Log(L"ExplorerTabs", L"Shell navigation rejected: " + std::to_wstring(hr));
        return false;
    }
    if(!enabled_||!frame_)return false;
    SetTimer(frame_, MaintenanceTimer, 100, nullptr);
    PostMessageW(frame_, SyncMessage, 0, 0);
    return true;
}
bool Controller::Activate(size_t index)
{
    if (index >= tabs_.size())
        return false;
    const auto id=tabs_[index].id;
    const auto position=tabs_[index].position;
    timing::Mark("activate", index, id);
    switch(switching_.Request(id,CommittedId(),pending_?pending_->id:0))
    {
    case tabs::RequestAction::None:
        // Choosing the shown tab again brings it fully into view.
        revealedId_=0;
        Layout();
        return true;
    case tabs::RequestAction::Wait:
        // The navigation in flight is followed by this one when it arrives.
        Layout();
        return true;
    case tabs::RequestAction::Navigate:
        break;
    }
    if(Navigate(id,position,true))return true;
    switching_.Forget(id);
    Layout();
    return false;
}
bool Controller::FollowRequest(ULONGLONG id)
{
    // The tab that just arrived was only passed through: its saved state is
    // neither captured from the fresh view nor restored into it.
    const int index=Index(id);
    return index>=0&&Navigate(id,tabs_[index].position,true,false);
}
bool Controller::Defer(tabs::Command command)
{
    if(switching_.Enqueue(command))return true;
    patch::Log(L"ExplorerTabs",L"Too many tab commands were waiting for a folder; the newest one was ignored.");
    return false;
}
void Controller::DrainCommands()
{
    while(enabled_&&!pending_)
    {
        const auto command=switching_.Next();
        if(!command)return;
        const int index=Index(command->tab);
        switch(command->kind)
        {
        case tabs::CommandKind::NewTab: NewTab(); break;
        case tabs::CommandKind::Duplicate:
            if(index>=0){Pidl folder=tabs_[index].history[tabs_[index].position].folder;NewTab(&folder);}
            break;
        case tabs::CommandKind::Close: if(index>=0)Close(static_cast<size_t>(index)); break;
        case tabs::CommandKind::Reopen: Reopen(); break;
        case tabs::CommandKind::Back: History(-1); break;
        case tabs::CommandKind::Forward: History(1); break;
        }
    }
}
void Controller::BeginRestore(const ViewState& state)
{
    CancelRestore();
    if(!browser_||!frame_)return;
    restoreState_=state;
    restoreDeadline_=GetTickCount64()+1500;
    ComPtr<IShellView> view;
    if(SUCCEEDED(browser_->QueryActiveShellView(&view)))
    {
        view->GetWindow(&restoreView_);
        viewEvents_=ViewEvents::Connect(view.Get(),frame_,RestoreMessage);
    }
    if(!restoreState_)return;
    // Items already listed: restore at once; otherwise the view's enumeration
    // event or the fallback deadline applies it.
    if(RestoreItemsListed(view.Get(),*restoreState_))PostMessageW(frame_,RestoreMessage,0,0);
    SetTimer(frame_,MaintenanceTimer,100,nullptr);
}
void Controller::CancelRestore()
{
    if(viewEvents_)viewEvents_->Disconnect();
    viewEvents_.Reset();
    restoreState_.reset();
    restoreView_=nullptr;
    restoreDeadline_=0;
}
void Controller::ApplyRestore()
{
    // One pass, when the view is ready (or at the fallback deadline). It
    // compares before it sets, so a view that already shows the state is left
    // alone.
    if(!restoreState_)return;
    const ViewState state=std::move(*restoreState_);
    ComPtr<IShellBrowser> browser=browser_;
    CancelRestore();
    timing::Mark("restore");
    Restore(browser.Get(),state);
}bool Controller::IsViewInput(const MSG& message) const
{
    if(!restoreView_||!(message.hwnd==restoreView_||IsChild(restoreView_,message.hwnd)))return false;
    switch(message.message)
    {
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_LBUTTONDBLCLK:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        return true;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        switch(message.wParam)
        {
        case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_LWIN: case VK_RWIN:
        case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL: case VK_LMENU: case VK_RMENU:
            return false;
        default:
            return true;
        }
    default:
        return false;
    }
}
void Controller::Synchronize()
{
    if (tearingDown_ || !browser_)
        return;
    const HWND frame=frame_;ComPtr<IShellBrowser> browser=browser_;
    if (pending_ && !tabs_.empty())
    {
        // Only the folder decides arrival; the full view state is not needed.
        const auto arrivedFolder=CurrentFolder(browser.Get());
        if(tearingDown_||!frame_||frame_!=frame||!pending_||!Equal(arrivedFolder,pending_->expected))
            return;
        const int index=Index(pending_->id);
        if (index<0)
        {
            pending_.reset();
            switching_.Abandon();
            Layout();
            return;
        }
        active_ = static_cast<size_t>(index);
        auto& tab=tabs_[active_];
        timing::Mark("commit", tab.id);
        tab.position = pending_->position;
        pending_.reset();
        busyShown_=false;
        if(const auto next=switching_.Arrived(tab.id);next&&FollowRequest(next))
        {
            RefreshTravel();
            TabChanged();
            return;
        }
        BeginRestore(tabs_[active_].history[tabs_[active_].position]);
        RefreshTravel();
        TabChanged();
        DrainCommands();
        return;
    }
    auto s = Capture(browser.Get());
    if (!s.folder||tearingDown_||!frame_||frame_!=frame||browser_.Get()!=browser.Get())
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
        return;
    {
        auto &t = tabs_[active_];
        if (!Equal(t.history[t.position].folder, s.folder))
        {
            // Navigated inside the tab (address bar, double-click, history):
            // a restore aimed at the previous folder no longer applies.
            if(restoreState_&&!Equal(restoreState_->folder,s.folder))CancelRestore();
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
    if (tabs_.size() >= MaxTabs || tabs_.empty())
        return;
    if (pending_)
    {
        if (!folder)
            Defer({tabs::CommandKind::NewTab, 0});
        return;
    }
    const Pidl requested=folder?*folder:Pidl{};
    Remember();
    if(!enabled_||tabs_.empty()||pending_)return;
    Tab t;
    ViewState state;
    state.folder = requested ? requested : tabs_[active_].history[tabs_[active_].position].folder;
    t.title = CachedName(state);
    t.history.push_back(std::move(state));
    ULONGLONG id = t.id;
    tabs_.push_back(std::move(t));
    if (!Navigate(id, 0, true)) {const int index=Index(id);if(index>=0)tabs_.erase(tabs_.begin()+index);}
    TabChanged();
}
void Controller::Close(size_t index,const std::vector<BYTE>* expected)
{
    if (index >= tabs_.size())
        return;
    if (pending_)
    {
        // A detach hand-off re-checks its own snapshot later instead.
        if (!expected)
            Defer({tabs::CommandKind::Close, tabs_[index].id});
        return;
    }
    if(expected&&Serialize(tabs_[index],frame_)!=*expected)return;
    if (tabs_.size() == 1)
    {
        if (frame_)
            PostMessageW(frame_, WM_SYSCOMMAND, SC_CLOSE, 0);
        return;
    }
    const auto id=tabs_[index].id;
    Remember();
    int current=Index(id);
    if(!enabled_||current<0||pending_)return;
    if(expected&&Serialize(tabs_[current],frame_)!=*expected)return;
    index=static_cast<size_t>(current);
    if (index == active_)
    {
        size_t target = index + 1 < tabs_.size() ? index + 1 : index - 1;
        if (!Activate(target))
            return;
    }
    current=Index(id);
    if(!enabled_||current<0)return;
    if(expected&&Serialize(tabs_[current],frame_)!=*expected)return;
    index=static_cast<size_t>(current);
    closed_.push_back(tabs_[index]);
    if (closed_.size() > 32)
        closed_.erase(closed_.begin());
    switching_.Forget(id);
    tabs_.erase(tabs_.begin() + static_cast<ptrdiff_t>(index));
    if (active_ > index)
        active_--;
    else if (active_ >= tabs_.size())
        active_ = tabs_.size() - 1;
    TabChanged();
}
void Controller::Reopen()
{
    if (closed_.empty() || tabs_.size() >= MaxTabs)
        return;
    if (pending_)
    {
        Defer({tabs::CommandKind::Reopen, 0});
        return;
    }
    Tab t = std::move(closed_.back());
    closed_.pop_back();
    t.id = nextId++;
    auto id = t.id;
    auto position = t.position;
    tabs_.push_back(std::move(t));
    if (!Navigate(id, position, true))
    {
        const int index=Index(id);if(index>=0){closed_.push_back(std::move(tabs_[index]));tabs_.erase(tabs_.begin()+index);}
    }
    TabChanged();
}
void Controller::History(int direction)
{
    if (tabs_.empty())
        return;
    if (pending_)
    {
        Defer({direction < 0 ? tabs::CommandKind::Back : tabs::CommandKind::Forward, 0});
        return;
    }
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
        const auto shown=DisplayedId();
        if(!TargetChild(focusedTarget_)&&shown)focusedTarget_={header::TargetKind::Tab,shown};
        NotifyWinEvent(EVENT_OBJECT_SELECTION, strip_, OBJID_CLIENT,shown?TargetChild({header::TargetKind::Tab,shown}):CHILDID_SELF);
        NotifyWinEvent(EVENT_OBJECT_REORDER, strip_, OBJID_CLIENT, CHILDID_SELF);
    }
    UpdateNavigationButtons();
    SaveSession();
}
void Controller::RefreshMetadata()
{
    if(!enabled_||!frame_)return;
    // Names and icons load for tabs on screen first; the queue is bounded.
    std::vector<Tab*> order;order.reserve(tabs_.size());
    for(auto& tab:tabs_){const auto* shown=layout_.Find(tab.id);if(shown&&shown->Visible())order.push_back(&tab);}
    for(auto& tab:tabs_){const auto* shown=layout_.Find(tab.id);if(!shown||!shown->Visible())order.push_back(&tab);}
    for(Tab* entry:order)
    {
        auto& tab=*entry;
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
            NotifyWinEvent(EVENT_OBJECT_NAMECHANGE,strip_,OBJID_CLIENT,TargetChild({header::TargetKind::Tab,tab->id}));
            NotifyWinEvent(EVENT_OBJECT_NAMECHANGE,strip_,OBJID_CLIENT,TargetChild({header::TargetKind::Close,tab->id}));
        }
    }
    RefreshMetadata();
}
void Controller::SaveSession()
{
    // Tab changes come in bursts; the registry write waits for 1.5 s of quiet.
    if (!hasUsableSession_ || tabs_.empty() || !frame_)
        return;
    sessionDirty_ = true;
    SetTimer(frame_, SessionTimer, SessionDelayMs, nullptr);
}
void Controller::FlushSession()
{
    if (frame_)
        KillTimer(frame_, SessionTimer);
    sessionDirty_ = false;
    if (!hasUsableSession_ || tabs_.empty())
        return;
    timing::Mark("save-begin", tabs_.size());
    struct SaveMark {~SaveMark(){timing::Mark("save-end");}} saveMark;
    // Every open tab is kept even when long histories or large selections
    // exceed the budget. Live in-memory history is unchanged.
    const auto bytes = EncodeSession(tabs_, active_, SessionBudget);
    if (bytes.empty())
        return;
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SessionKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) ==
        ERROR_SUCCESS)
    {
        RegSetValueExW(key, L"LastSession", 0, REG_BINARY, bytes.data(), static_cast<DWORD>(bytes.size()));
        RegCloseKey(key);
    }
}
std::wstring WindowSessionName(HWND frame)
{
    return L"Window."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(reinterpret_cast<ULONG_PTR>(frame));
}
ULONGLONG ProcessBirth()
{
    FILETIME created{},exited{},kernel{},user{};if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))return 0;
    ULARGE_INTEGER value{};value.LowPart=created.dwLowDateTime;value.HighPart=created.dwHighDateTime;return value.QuadPart;
}
bool Controller::SaveWindowSession()
{
    if(!frame_||closingFrame_||tabs_.empty()||ownerThread_!=GetCurrentThreadId())return false;
    if(pending_){Synchronize();if(pending_)return false;}
    const HWND window=frame_;Remember();if(frame_!=window||tabs_.empty()||active_>=tabs_.size())return false;
    const auto nonce=static_cast<ULONG_PTR>(GetTickCount64()^reinterpret_cast<ULONG_PTR>(this));
    Writer writer;writer.number(PacketMagic);writer.number(DWORD{4});writer.number(ProcessBirth());writer.number(static_cast<ULONGLONG>(reinterpret_cast<ULONG_PTR>(frame_)));writer.number(static_cast<ULONGLONG>(nonce));
    writer.number(static_cast<DWORD>(tabs_.size()));writer.number(static_cast<DWORD>(active_));
    for(const auto& tab:tabs_)writer.tab(tab);if(!writer.ok)return false;
    HKEY key{};if(RegCreateKeyExW(HKEY_CURRENT_USER,SessionKey,0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS)return false;
    const auto name=WindowSessionName(frame_);
    const bool saved=RegSetValueExW(key,name.c_str(),0,REG_BINARY,writer.bytes.data(),static_cast<DWORD>(writer.bytes.size()))==ERROR_SUCCESS;
    RegCloseKey(key);return saved&&SetPropW(frame_,WindowSessionProperty,reinterpret_cast<HANDLE>(nonce));
}
void Controller::RemoveWindowSession()
{
    if(!frame_)return;HKEY key{};
    if(RegOpenKeyExW(HKEY_CURRENT_USER,SessionKey,0,KEY_SET_VALUE,&key)==ERROR_SUCCESS){RegDeleteValueW(key,WindowSessionName(frame_).c_str());RegCloseKey(key);}
    RemovePropW(frame_,WindowSessionProperty);
}
bool Controller::LoadWindowSession()
{
    const auto nonce=reinterpret_cast<ULONG_PTR>(GetPropW(frame_,WindowSessionProperty));if(!nonce)return false;
    const auto name=WindowSessionName(frame_);DWORD size{};
    if(RegGetValueW(HKEY_CURRENT_USER,SessionKey,name.c_str(),RRF_RT_REG_BINARY,nullptr,nullptr,&size)!=ERROR_SUCCESS||size>MaxPacket||size<40)return false;
    std::vector<BYTE> bytes(size);if(RegGetValueW(HKEY_CURRENT_USER,SessionKey,name.c_str(),RRF_RT_REG_BINARY,nullptr,bytes.data(),&size)!=ERROR_SUCCESS)return false;
    Reader reader{bytes.data(),size};if(reader.number<DWORD>()!=PacketMagic||reader.number<DWORD>()!=4||reader.number<ULONGLONG>()!=ProcessBirth()||reader.number<ULONGLONG>()!=reinterpret_cast<ULONG_PTR>(frame_)||reader.number<ULONGLONG>()!=nonce)return false;
    const auto count=reader.number<DWORD>(),active=reader.number<DWORD>();if(!count||count>MaxTabs||active>=count)return false;
    std::vector<Tab> loaded;for(DWORD i=0;i<count&&reader.ok;++i)loaded.push_back(reader.tab());if(!reader.ok||reader.left||loaded.size()!=count)return false;
    // Keep the snapshot until its actual current location is ready. This also
    // avoids replacing a valid old session merely because a provider is late.
    const HWND window=frame_;ComPtr<IShellBrowser> browser=browser_;
    auto current=Capture(browser.Get());
    if(!enabled_||tearingDown_||frame_!=window||browser_.Get()!=browser.Get()||reinterpret_cast<ULONG_PTR>(GetPropW(window,WindowSessionProperty))!=nonce)return true;
    if(!current.folder){SetTimer(frame_,MaintenanceTimer,100,nullptr);return true;}
    auto& selected=loaded[active];
    if(!Equal(current.folder,selected.history[selected.position].folder)) {
        selected.history.erase(selected.history.begin()+static_cast<ptrdiff_t>(selected.position+1),selected.history.end());
        selected.history.push_back(std::move(current));selected.position=selected.history.size()-1;
    }
    tabs_=std::move(loaded);active_=active;RemoveWindowSession();
    TabChanged();return true;
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
    if (!Ready()||tabs_.empty() || pending_)
        return;
    const Tab t = tabs_[active_];
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
    {auto label=CachedName(t.history[entries[i]]);for(size_t at=0;(at=label.find(L'&',at))!=std::wstring::npos;at+=2)label.insert(at,1,L'&');
        AppendMenuW(menu, MF_STRING, 100 + static_cast<UINT_PTR>(i), label.c_str());}
    ++menuDepth_;
    UINT selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, frame_, nullptr);
    --menuDepth_;
    DestroyMenu(menu);
    const int index=Index(t.id);
    if (!Ready()||pending_||index<0||selected<100||selected-100>=entries.size())return;
    const auto& destination=t.history[entries[selected-100]].folder;
    // A position alone is not a history destination after a nested navigation.
    const auto& current=tabs_[index];
    if(current.history.size()!=t.history.size())return;
    for(size_t i=0;i<t.history.size();++i)if(!Equal(current.history[i].folder,t.history[i].folder))return;
    if(Equal(current.history[entries[selected-100]].folder,destination))Navigate(t.id,entries[selected-100],false);
}
int Controller::Hit(POINT point) const
{
    if(!rendererReady_||!header::IsCurrent(frame_,layout_.allocation))return -1;
    const auto target=layout_.Hit(point);
    if(target.kind==header::TargetKind::Tab||target.kind==header::TargetKind::Close)
        for(size_t i=0;i<tabs_.size();++i)if(tabs_[i].id==target.tab)return static_cast<int>(i);
    if(target.kind==header::TargetKind::NewTab)return -2;
    if(target.kind==header::TargetKind::Overflow)return -3;
    if(target.kind==header::TargetKind::ScrollLeft||target.kind==header::TargetKind::ScrollRight)return -4;
    return -1;
}
void Controller::Menu(int hit, POINT point)
{
    if(!Ready())return;
    const auto owner=layout_.allocation.owner;
    const auto id=hit>=0&&static_cast<size_t>(hit)<tabs_.size()?tabs_[hit].id:0;
    std::vector<ULONGLONG> identities;
    HMENU menu = CreatePopupMenu();
    constexpr UINT New = 1, CloseTab = 2, ReopenTab = 3, Duplicate = 4, DetachTabCommand = 5, Options = 6;
    if (hit == -3)
    {
        for (size_t i = 0; i < tabs_.size(); i++)
        {
            identities.push_back(tabs_[i].id);
            auto label=tabs_[i].title;for(size_t at=0;(at=label.find(L'&',at))!=std::wstring::npos;at+=2)label.insert(at,1,L'&');
            AppendMenuW(menu, MF_STRING | (i == DisplayedIndex() ? MF_CHECKED : 0), 100 + static_cast<UINT_PTR>(i),
                        label.c_str());
        }
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
    AppendMenuW(menu, MF_STRING, Options, L"Options...");
    ++menuDepth_;
    UINT selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, frame_, nullptr);
    --menuDepth_;
    DestroyMenu(menu);
    if(!Ready()||layout_.allocation.owner!=owner)return;
    hit=Index(id);
    if (selected >= 100 && selected - 100 < identities.size())
    {
        const int target=Index(identities[selected-100]);if(target>=0)Activate(target);
        return;
    }
    if (selected == New)
        NewTab();
    else if (selected == CloseTab && hit >= 0)
        Close(hit);
    else if (selected == ReopenTab)
        Reopen();
    else if (selected == Duplicate && hit >= 0 && pending_)
        Defer({tabs::CommandKind::Duplicate, tabs_[hit].id});
    else if (selected == Duplicate && hit >= 0)
    {
        Pidl folder = tabs_[hit].history[tabs_[hit].position].folder;
        NewTab(&folder);
    }
    else if (selected == DetachTabCommand && hit >= 0)
        DetachTab(hit);
    else if (selected == Options)
        patch::OpenOptions(frame_);
}
void Controller::CancelPointer()
{
    // Escape, a lost capture or a disabled header put a dragged tab back.
    if(reordering_){
        timing::Mark("reordercancel",(Ready()?1:0)|(strip_&&GetCapture()==strip_?2:0)|(rendererReady_?4:0),static_cast<ULONGLONG>(dragPointer_.x));
        EndReorder(false);
    }
    pressedTarget_={};pressedButton_=0;
    if(strip_)KillTimer(strip_,ScrollTimer);
    if(strip_&&GetCapture()==strip_)ReleaseCapture();
    if(strip_)InvalidateRect(strip_,nullptr,FALSE);
}
void Controller::SystemMenu(bool install)
{
    if(!install) {
        if(systemMenu_&&IsMenu(systemMenu_))for(UINT id:{focusMenuId_,focusMenuId_+1}) {
            MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_DATA;
            if(GetMenuItemInfoW(systemMenu_,id,FALSE,&item)&&item.dwItemData==reinterpret_cast<ULONG_PTR>(this))DeleteMenu(systemMenu_,id,MF_BYCOMMAND);
        }
        systemMenu_=nullptr;focusMenuId_=0;return;
    }
    systemMenu_=GetSystemMenu(frame_,FALSE);if(!systemMenu_)return;
    for(UINT id=0x1ED0;id<0xE000;id+=16)if(GetMenuState(systemMenu_,id,MF_BYCOMMAND)==static_cast<UINT>(-1)&&GetMenuState(systemMenu_,id+1,MF_BYCOMMAND)==static_cast<UINT>(-1)){focusMenuId_=id;break;}
    if(!focusMenuId_){systemMenu_=nullptr;return;}
    MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_ID|MIIM_FTYPE|MIIM_DATA;item.fType=MFT_SEPARATOR;item.wID=focusMenuId_+1;item.dwItemData=reinterpret_cast<ULONG_PTR>(this);
    InsertMenuItemW(systemMenu_,GetMenuItemCount(systemMenu_),TRUE,&item);
    item.fMask=MIIM_ID|MIIM_STRING|MIIM_DATA;item.wID=focusMenuId_;item.dwTypeData=const_cast<wchar_t*>(L"Focus folder &tabs");
    InsertMenuItemW(systemMenu_,GetMenuItemCount(systemMenu_),TRUE,&item);
}
void Controller::FocusTarget(header::TargetId target)
{
    if(!Ready()||!TargetChild(target))return;
    focusedTarget_=target;
    SetFocus(strip_);
    NotifyWinEvent(EVENT_OBJECT_FOCUS,strip_,OBJID_CLIENT,TargetChild(target));
    InvalidateRect(strip_,nullptr,FALSE);
}
POINT Controller::MenuAnchor(header::TargetId target) const
{
    RECT bounds{};ChildRect(TargetChild(target),bounds);return {bounds.left,bounds.bottom};
}
void Controller::InvokeTarget(header::TargetId target)
{
    if(!Ready())return;
    int index=Index(target.tab);
    if(target.kind==header::TargetKind::Tab&&index>=0)Activate(index);
    else if(target.kind==header::TargetKind::Close&&index>=0)Close(index);
    else if(target.kind==header::TargetKind::NewTab)NewTab();
    else if(target.kind==header::TargetKind::Overflow)Menu(-3,MenuAnchor(target));
    else if(target.kind==header::TargetKind::ScrollLeft)ScrollBy(-layout_.tabWidth);
    else if(target.kind==header::TargetKind::ScrollRight)ScrollBy(layout_.tabWidth);
}
void Controller::ScrollBy(int pixels)
{
    if(!Ready()||!layout_.overflowing||!pixels)return;
    const int next=std::clamp(scroll_+pixels,0,layout_.maxScroll);
    if(next==scroll_)return;
    scroll_=next;revealedId_=DisplayedId();
    // A dragged tab stays under the pointer while the row scrolls beneath it.
    if(reordering_)Reorder(dragPointer_);
    else{Layout();RefreshHot();}
    RefreshMetadata();
}
void Controller::Shift(size_t from,size_t to)
{
    // Moves one tab, keeping the others in order and the shown tab committed
    // by identity.
    if(from>=tabs_.size()||to>=tabs_.size()||from==to)return;
    const ULONGLONG committed=CommittedId();
    const auto begin=tabs_.begin()+static_cast<std::ptrdiff_t>(std::min(from,to));
    const auto end=tabs_.begin()+static_cast<std::ptrdiff_t>(std::max(from,to))+1;
    if(from<to)std::rotate(begin,begin+1,end);
    else std::rotate(begin,end-1,end);
    if(const int index=Index(committed);index>=0)active_=static_cast<size_t>(index);
}
void Controller::RefreshMotion()
{
    // The Options "Animate tabs" switch. Windows' own "Animate controls and
    // elements inside windows" is read at each paint: its change broadcast
    // does not reliably reach every frame.
    motion_=!(patch::LoadConfig().flags&patch::NoTabAnimations);
}
void Controller::BeginReorder(POINT point)
{
    const auto* geometry=layout_.Find(pressedTarget_.tab);
    const int index=Index(pressedTarget_.tab);
    if(!geometry||index<0)return;
    reordering_=true;
    dragTab_=pressedTarget_.tab;
    dragOrigin_=static_cast<size_t>(index);
    // Where inside the tab it was grabbed, so it does not jump to the pointer.
    dragGrab_=dragStart_.x-layout_.tabViewport.left+scroll_-geometry->x;
    hotTarget_={};
    Tooltip({});
    Reorder(point);
}
bool Controller::InReorderBand(POINT point) const
{
    // The strip plus half its height on every side, so a slightly unsteady
    // hand keeps reordering while a deliberate pull leaves the strip.
    RECT band{};GetClientRect(strip_,&band);
    const int margin=(band.bottom-band.top)/2;
    InflateRect(&band,margin,margin);
    return PtInRect(&band,point)!=FALSE;
}
void Controller::Reorder(POINT point)
{
    if(!reordering_||!strip_)return;
    if(!InReorderBand(point)) {
        // Leaving the strip: the others settle back and the tab continues as
        // an ordinary Shell drag (another window's strip, or a new window).
        const auto source=dragTab_;
        CancelPointer();
        if(const int index=Index(source);index>=0)StartDrag(static_cast<size_t>(index));
        return;
    }
    dragPointer_=point;
    const int width=std::max(1,layout_.tabWidth),last=static_cast<int>(tabs_.size()-1);
    dragX_=std::clamp(static_cast<int>(point.x-layout_.tabViewport.left)+scroll_-dragGrab_,0,last*width);
    // The tab takes the place its centre is over; the others slide aside.
    const int target=std::clamp((dragX_+width/2)/width,0,last),from=Index(dragTab_);
    const bool move=from>=0&&from!=target;
    if(move)Shift(static_cast<size_t>(from),static_cast<size_t>(target));
    // A new order or scroll position needs a layout; otherwise only the
    // dragged tab moves.
    if(move||scroll_!=layout_.scroll)Layout();
    else if(!Render())HeaderFailed();
    if(!reordering_)return;
    // Near either end of an overflowing row, the row scrolls under the tab.
    const int edge=MulDiv(header::ControlDip,static_cast<int>(dpi_),96);
    const bool nearEdge=layout_.overflowing&&(point.x<layout_.tabViewport.left+edge||point.x>=layout_.tabViewport.right-edge);
    if(nearEdge&&!dragScrolling_&&strip_){dragScrolling_=true;SetTimer(strip_,ScrollTimer,16,nullptr);}
}
void Controller::ReorderScroll()
{
    const int edge=MulDiv(header::ControlDip,static_cast<int>(dpi_),96),step=MulDiv(8,static_cast<int>(dpi_),96);
    int delta=0;
    if(reordering_&&layout_.overflowing) {
        if(dragPointer_.x<layout_.tabViewport.left+edge)delta=-step;
        else if(dragPointer_.x>=layout_.tabViewport.right-edge)delta=step;
    }
    if(!delta){dragScrolling_=false;if(strip_)KillTimer(strip_,ScrollTimer);return;}
    ScrollBy(delta);
}
void Controller::EndReorder(bool commit)
{
    if(!reordering_)return;
    reordering_=false;dragScrolling_=false;
    if(strip_)KillTimer(strip_,ScrollTimer);
    const int at=Index(std::exchange(dragTab_,0));
    const bool moved=at>=0&&static_cast<size_t>(at)!=dragOrigin_;
    if(moved&&!commit&&dragOrigin_<tabs_.size())Shift(static_cast<size_t>(at),dragOrigin_);
    if(!enabled_||tearingDown_||!strip_)return;
    // The released tab glides into its slot on the reorder curve.
    if(moved&&commit)TabChanged();
    else Layout();
}
void Controller::ScrollRepeat(bool first)
{
    // Press-and-hold repeats at the keyboard's delay and rate.
    if(!strip_||(pressedTarget_.kind!=header::TargetKind::ScrollLeft&&pressedTarget_.kind!=header::TargetKind::ScrollRight)){
        if(strip_)KillTimer(strip_,ScrollTimer);
        return;
    }
    if(first||hotTarget_==pressedTarget_)
        ScrollBy(pressedTarget_.kind==header::TargetKind::ScrollLeft?-layout_.tabWidth:layout_.tabWidth);
    int delay{},speed{};
    SystemParametersInfoW(SPI_GETKEYBOARDDELAY,0,&delay,0);SystemParametersInfoW(SPI_GETKEYBOARDSPEED,0,&speed,0);
    const UINT interval=first?static_cast<UINT>(250*(std::clamp(delay,0,3)+1)):static_cast<UINT>(1000.0/(2.5+std::clamp(speed,0,31)*27.5/31.0));
    SetTimer(strip_,ScrollTimer,interval,nullptr);
}
void Controller::RefreshHot()
{
    POINT point{};
    if(!strip_||!GetCursorPos(&point))return;
    ScreenToClient(strip_,&point);
    RECT client{};GetClientRect(strip_,&client);
    const auto target=PtInRect(&client,point)?layout_.Hit(point):header::TargetId{};
    if(target!=hotTarget_){hotTarget_=target;InvalidateRect(strip_,nullptr,FALSE);}
    Tooltip(target);
}
void Controller::Tooltip(header::TargetId target)
{
    if(!strip_)return;
    if(!tooltip_) {
        tooltip_=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_NOPREFIX|TTS_ALWAYSTIP,
                                 CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,strip_,nullptr,module,nullptr);
        if(!tooltip_)return;
        TTTOOLINFOW tool{sizeof(tool)};tool.uFlags=TTF_SUBCLASS|TTF_TRANSPARENT;tool.hwnd=strip_;tool.uId=1;
        tool.lpszText=LPSTR_TEXTCALLBACKW;
        SendMessageW(tooltip_,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tool));
        SendMessageW(tooltip_,TTM_SETMAXTIPWIDTH,0,MulDiv(480,static_cast<int>(dpi_),96));
    }
    if(target==tipTarget_)return;
    tipTarget_=target;
    SendMessageW(tooltip_,TTM_POP,0,0);
    TTTOOLINFOW tool{sizeof(tool)};tool.hwnd=strip_;tool.uId=1;
    RECT bounds{};
    if(target.kind==header::TargetKind::Tab||target.kind==header::TargetKind::Close) {
        if(const auto* tab=layout_.Find(target.tab))IntersectRect(&bounds,&tab->bounds,&layout_.tabViewport);
    } else if(target.kind==header::TargetKind::NewTab)bounds=layout_.newTab;
    else if(target.kind==header::TargetKind::Overflow)bounds=layout_.overflow;
    else if(target.kind==header::TargetKind::ScrollLeft)bounds=layout_.scrollLeft;
    else if(target.kind==header::TargetKind::ScrollRight)bounds=layout_.scrollRight;
    tool.rect=bounds;
    SendMessageW(tooltip_,TTM_NEWTOOLRECTW,0,reinterpret_cast<LPARAM>(&tool));
}
std::wstring Controller::TooltipText(header::TargetId target) const
{
    switch(target.kind) {
    case header::TargetKind::NewTab: return L"New tab (Ctrl+T)";
    case header::TargetKind::Overflow: return L"List all tabs";
    case header::TargetKind::ScrollLeft: return L"Scroll tabs left";
    case header::TargetKind::ScrollRight: return L"Scroll tabs right";
    case header::TargetKind::Close: return L"Close tab (Ctrl+W)";
    case header::TargetKind::Tab: break;
    default: return {};
    }
    const int index=Index(target.tab);
    if(index<0)return {};
    const auto& tab=tabs_[index];
    std::wstring text=tab.title;
    if(tab.position<tab.history.size()&&tab.history[tab.position].folder) {
        PWSTR path{};
        if(SUCCEEDED(SHGetNameFromIDList(tab.history[tab.position].folder.p,SIGDN_DESKTOPABSOLUTEEDITING,&path))&&path) {
            if(text!=path)text+=L"\n"+std::wstring(path);
            CoTaskMemFree(path);
        }
    }
    return text;
}
void Controller::Mouse(UINT message, WPARAM, LPARAM data)
{
    if(!Ready()){CancelPointer();return;}
    POINT point{GET_X_LPARAM(data), GET_Y_LPARAM(data)};
    int hit = Hit(point);
    if (message == WM_MOUSEMOVE)
    {
        if(reordering_){Reorder(point);return;}
        const auto target=layout_.Hit(point);
        if (hotTarget_!=target)
        {
            hotTarget_=target;
            InvalidateRect(strip_, nullptr, FALSE);
        }
        Tooltip(target);
        TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, strip_, 0};
        TrackMouseEvent(&t);
        if (pressedButton_==WM_LBUTTONDOWN&&pressedTarget_.kind==header::TargetKind::Tab&& (GetKeyState(VK_LBUTTON) & 0x8000) && !inDrag_)
        {
            if (abs(point.x - dragStart_.x) >= GetSystemMetrics(SM_CXDRAG) ||
                abs(point.y - dragStart_.y) >= GetSystemMetrics(SM_CYDRAG))
            {
                // Within the strip a drag reorders in place; a tab pulled
                // straight out (or the only tab) starts the Shell drag.
                if(tabs_.size()>1&&InReorderBand(point))BeginReorder(point);
                else {
                    const auto source=pressedTarget_.tab;
                    CancelPointer();
                    const int index=Index(source);if(index>=0)StartDrag(index);
                }
            }
        }
        return;
    }
    if (message == WM_LBUTTONDOWN||message==WM_MBUTTONDOWN)
    {
        CancelPointer();
        FocusTarget(layout_.Hit(point));
        pressedButton_=message;
        pressedTarget_=layout_.Hit(point);hotTarget_=pressedTarget_;
        InvalidateRect(strip_,nullptr,FALSE);
        dragStart_ = point;
        SetCapture(strip_);
        if(message==WM_LBUTTONDOWN&&hit==-4){ScrollRepeat(true);return;}
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
        if(reordering_) {
            // Dropping keeps the new order and shows the dragged tab.
            const ULONGLONG dropped=dragTab_;
            EndReorder(true);
            CancelPointer();
            if(const int index=Index(dropped);index>=0)Activate(static_cast<size_t>(index));
            return;
        }
        const auto was=pressedTarget_;
        const bool valid=pressedButton_==WM_LBUTTONDOWN&&GetCapture()==strip_&&was==layout_.Hit(point);
        // Capture-change cleanup can run synchronously during ReleaseCapture.
        // Preserve the intended ordinary up action before releasing it.
        CancelPointer();
        // Closing with the mouse keeps tab widths until the pointer leaves the
        // strip, so the next close button lands under it (Firefox).
        if(valid&&was.kind==header::TargetKind::Close&&!layout_.overflowing)lockedWidth_=layout_.tabWidth;
        // Scroll arrows act on press and repeat while held; release only stops them.
        const bool arrow=was.kind==header::TargetKind::ScrollLeft||was.kind==header::TargetKind::ScrollRight;
        if(valid&&!arrow)InvokeTarget(was);
        return;
    }
    if (message == WM_MBUTTONUP)
    {
        const auto was=pressedTarget_;
        const auto now=layout_.Hit(point);
        const bool valid=pressedButton_==WM_MBUTTONDOWN&&GetCapture()==strip_&&was.tab&&was.tab==now.tab&&was==now;
        CancelPointer();
        if(valid&&!layout_.overflowing)lockedWidth_=layout_.tabWidth;
        if(valid)InvokeTarget({header::TargetKind::Close,was.tab});
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
    self->AddRef();
    struct Reference {Controller* owner;~Reference(){owner->Release();}} reference{self};
    switch (m)
    {
    case WM_NCHITTEST:
    {
        POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
        ScreenToClient(h,&point);
        if(self->Hit(point)==-1)return HTTRANSPARENT;
        break;
    }
    case WM_PAINT:
        self->Paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
    case WM_LBUTTONDBLCLK:
        self->Mouse(m, w, l);
        return 0;
    case WM_MOUSELEAVE:
        self->hotTarget_={};
        self->Tooltip({});
        if(self->lockedWidth_){self->lockedWidth_=0;self->Layout();}
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        // While overflowing, a wheel notch moves one tab and a horizontal
        // touchpad swipe moves in proportion. Otherwise the wheel is Explorer's.
        if(self->layout_.overflowing){
            const int delta=GET_WHEEL_DELTA_WPARAM(w);
            self->ScrollBy(header::ScrollStep(self->layout_,m==WM_MOUSEWHEEL?-delta:delta));
            return 0;
        }
        break;
    case WM_TIMER:
        if(w==ScrollTimer){if(self->reordering_)self->ReorderScroll();else self->ScrollRepeat(false);return 0;}
        if(w==MotionTimer){KillTimer(h,MotionTimer);if(!self->Render())self->HeaderFailed();return 0;}
        break;
    case WM_NOTIFY:
    {
        auto* info=reinterpret_cast<NMTTDISPINFOW*>(l);
        if(info&&info->hdr.hwndFrom==self->tooltip_&&info->hdr.code==TTN_GETDISPINFOW) {
            static thread_local std::wstring text;
            text=self->TooltipText(self->tipTarget_);
            info->lpszText=text.data();
            return 0;
        }
        break;
    }
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        self->CancelPointer();
        break;
    case WM_GETOBJECT:
        if (static_cast<LONG>(l) == OBJID_CLIENT)
            return LresultFromObject(IID_IAccessible, w, static_cast<IAccessible *>(self));
        break;
    case WM_SETFOCUS:
        if(!self->TargetChild(self->focusedTarget_)&&!self->tabs_.empty())self->focusedTarget_={header::TargetKind::Tab,self->DisplayedId()};
        NotifyWinEvent(EVENT_OBJECT_FOCUS, h, OBJID_CLIENT,self->TargetChild(self->focusedTarget_));
        InvalidateRect(h,nullptr,FALSE);
        break;
    case WM_KILLFOCUS:
        InvalidateRect(h,nullptr,FALSE);
        break;
    case WM_GETDLGCODE:
    {
        const auto* key=reinterpret_cast<MSG*>(l);
        return DLGC_WANTARROWS|((key&&(key->wParam==VK_SPACE||key->wParam==VK_RETURN||key->wParam==VK_APPS))?DLGC_WANTMESSAGE:0);
    }
    case WM_CONTEXTMENU:
    {
        const auto target=self->focusedTarget_;
        POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
        if(point.x==-1&&point.y==-1)point=self->MenuAnchor(target);
        const int index=self->Index(target.tab);
        self->Menu(target.kind==header::TargetKind::Overflow?-3:index,point);
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_LEFT || w==VK_RIGHT)
        {
            LONG child=self->TargetChild(self->focusedTarget_);
            if(self->focusedTarget_.kind==header::TargetKind::Tab) {
                int index=self->Index(self->focusedTarget_.tab)+(w==VK_RIGHT?1:-1);
                if(index>=0&&static_cast<size_t>(index)<self->tabs_.size()) {
                    auto target=header::TargetId{header::TargetKind::Tab,self->tabs_[index].id};
                    self->FocusTarget(target);self->InvokeTarget(target);
                }
            } else {
                child+=w==VK_RIGHT?1:-1;
                auto target=self->ChildTarget(child);if(target.kind!=header::TargetKind::None)self->FocusTarget(target);
            }
            return 0;
        }
        if(w==VK_SPACE||w==VK_RETURN){self->InvokeTarget(self->focusedTarget_);return 0;}
        if(w==VK_ESCAPE){self->CancelPointer();return 0;}
        if (w == VK_DELETE)
        {
            const int index=self->Index(self->focusedTarget_.tab);if(index>=0)self->Close(index);
            return 0;
        }
        if (w == VK_APPS || (w == VK_F10 && (GetKeyState(VK_SHIFT) & 0x8000)))
        {
            const auto target=self->focusedTarget_;
            self->Menu(target.kind==header::TargetKind::Overflow?-3:self->Index(target.tab),self->MenuAnchor(target));
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
    if(m==WM_NCHITTEST) {
        // Explorer and DWM get first refusal for every caption button, border
        // and native region. Only our current, presented targets become client.
        result=DefSubclassProc(h,m,w,l);
        if(result==HTCAPTION&&self->strip_&&self->rendererReady_&&IsWindowVisible(self->strip_)) {
            POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
            ScreenToClient(self->strip_,&point);
            if(self->Hit(point)!=-1)result=HTCLIENT;
        }
        handled=true;
    }
    else if (m == patch::mvp::DetachMessage() && self->selfHosted_)
    {
        const auto priorSnapshot=GetPropW(h,WindowSessionProperty);
        const bool saved=self->SaveWindowSession();
        const auto request=patch::mvp::DetachRequest(w);
        if(request&&reinterpret_cast<ULONG_PTR>(GetPropW(reinterpret_cast<HWND>(l),patch::mvp::StopRequest))!=request){
            if(saved&&!priorSnapshot&&self->frame_==h&&self->selfHosted_)self->RemoveWindowSession();
            self->Release();return 0;
        }
        if(patch::mvp::PreserveSession(w)&&!saved){self->Release();return 0;}
        const bool owned=std::exchange(self->selfHosted_,false);
        self->Detach();
        if(owned)self->Release();
        handled = true;
        result = 1;
    }
    else if (m == patch::SettingsChangedMessage())
    {
        self->RefreshMotion();
        self->Enable((patch::LoadConfig().flags & patch::ExplorerTabs) != 0);
        handled = true;
    }
    else if(m==WM_SYSCOMMAND&&self->focusMenuId_&&(w&0xFFF0)==self->focusMenuId_) {
        if(self->Ready()&&!self->tabs_.empty())self->FocusTarget({header::TargetKind::Tab,self->DisplayedId()});
        handled=true;
    }
    else if (m == SyncMessage)
    {
        self->Synchronize();
        handled = true;
    }
    else if(m==header::ChangedMessage()) {self->Layout();handled=true;}
    else if (m == IconMessage)
    {
        if(static_cast<ULONGLONG>(l)==self->metadataCookie_)self->AcceptMetadata(static_cast<ULONGLONG>(w));
        handled=true;
    }
    else if (m == RestoreMessage)
    {
        self->ApplyRestore();
        handled = true;
    }
    else if (m == WM_TIMER && w == MaintenanceTimer)
    {
        self->Timer();
        handled = true;
    }
    else if (m == WM_TIMER && w == SessionTimer)
    {
        self->FlushSession();
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
            if(m!=WM_WINDOWPOSCHANGED)self->lockedWidth_=0;
            header::Invalidate(h);
            result = DefSubclassProc(h, m, w, l);
            self->Layout(true);
            handled = true;
        }
        else if (m == WM_THEMECHANGED || m == WM_SETTINGCHANGE || m == WM_SYSCOLORCHANGE || m == WM_DWMCOLORIZATIONCOLORCHANGED)
        {
            header::Invalidate(h);
            result = DefSubclassProc(h, m, w, l);
            self->RefreshAppearance();
            self->Layout();
            handled = true;
        }
        else if(m==WM_ACTIVATE||m==WM_NCACTIVATE) {
            result=DefSubclassProc(h,m,w,l);
            if(m==WM_ACTIVATE&&LOWORD(w)==WA_INACTIVE) {
                self->hotTarget_={};self->CancelPointer();
            }
            if(self->strip_)InvalidateRect(self->strip_,nullptr,FALSE);
            handled=true;
        }
        else if (m == WM_CLOSE || (m == WM_ENDSESSION && w))
        {
            self->Remember();
            self->FlushSession();
        }
        else if (m == WM_NCDESTROY)
        {
            self->RemoveWindowSession();
            self->closingFrame_=true;
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
    self->AddRef();
    struct Reference {Controller* owner;~Reference(){owner->Release();}} reference{self};
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
                    POINT point{drop->rcButton.left,drop->rcButton.bottom};
                    ClientToScreen(hdr->hwndFrom,&point);
                    self->HistoryMenu(drop->iItem == self->backCommand_, point);
                    return TBDDRET_DEFAULT;
                }
            }
        }
        if (m == WM_CONTEXTMENU)
        {
            POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            HWND toolbar = FindWindowExW(h, nullptr, TOOLBARCLASSNAMEW, nullptr);
            if(point.x==-1&&point.y==-1&&toolbar) {
                const auto hot=SendMessageW(toolbar,TB_GETHOTITEM,0,0);RECT bounds{};
                if(hot>=0&&SendMessageW(toolbar,TB_GETITEMRECT,hot,reinterpret_cast<LPARAM>(&bounds))) {point={bounds.left,bounds.bottom};ClientToScreen(toolbar,&point);}
            }
            POINT local = point;
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
// Tab shortcuts follow Firefox where Explorer has no binding of its own.
// Each row names its chord; 'repeats' lets auto-repeat step through tabs.
bool Controller::Shortcut(UINT key, bool ctrl, bool shift, bool alt, bool repeated)
{
    using Action = bool (*)(Controller&, UINT key, bool repeated);
    struct Binding { UINT first, last; bool ctrl, shift, alt, repeats; Action run; };
    static constexpr auto step=[](Controller& c,int delta){
        if(c.tabs_.empty())return true;
        const auto count=static_cast<long long>(c.tabs_.size());
        c.Activate(static_cast<size_t>(((static_cast<long long>(c.DisplayedIndex())+delta)%count+count)%count));
        return true;
    };
    static const Binding bindings[]{
        {'T','T',true,false,false,false,[](Controller& c,UINT,bool){c.NewTab();return true;}},
        {'T','T',true,true,false,false,[](Controller& c,UINT,bool){c.Reopen();return true;}},
        {'W','W',true,false,false,false,[](Controller& c,UINT,bool){c.Close(c.DisplayedIndex());return true;}},
        {VK_TAB,VK_TAB,true,false,false,true,[](Controller& c,UINT,bool){return step(c,1);}},
        {VK_TAB,VK_TAB,true,true,false,true,[](Controller& c,UINT,bool){return step(c,-1);}},
        {VK_NEXT,VK_NEXT,true,false,false,true,[](Controller& c,UINT,bool){return step(c,1);}},
        {VK_PRIOR,VK_PRIOR,true,false,false,true,[](Controller& c,UINT,bool){return step(c,-1);}},
        {VK_NEXT,VK_NEXT,true,true,false,true,[](Controller& c,UINT,bool){c.MoveTab(1);return true;}},
        {VK_PRIOR,VK_PRIOR,true,true,false,true,[](Controller& c,UINT,bool){c.MoveTab(-1);return true;}},
        {'1','9',true,false,false,true,[](Controller& c,UINT k,bool){
            if(!c.tabs_.empty())c.Activate(k=='9'?c.tabs_.size()-1:k-'1');return true;}},
        {VK_LEFT,VK_LEFT,false,false,true,false,[](Controller& c,UINT,bool){c.History(-1);return true;}},
        {VK_RIGHT,VK_RIGHT,false,false,true,false,[](Controller& c,UINT,bool){c.History(1);return true;}},
        {VK_BACK,VK_BACK,false,false,false,true,[](Controller& c,UINT,bool r){return c.BackspaceTravels(r);}},
    };
    for(const auto& b:bindings)
    {
        if(key<b.first||key>b.last||ctrl!=b.ctrl||shift!=b.shift||alt!=b.alt)continue;
        // A held chord that should act once is still swallowed, so Explorer
        // never sees half of it.
        return !b.repeats&&repeated?true:b.run(*this,key,repeated);
    }
    return false;
}
bool Controller::BackspaceTravels(bool repeated)
{
    // Explorer editors share DirectUI hosts. Ask the actual folder view for its
    // focus path rather than guessing an editor class name or taking over the
    // whole frame.
    ComPtr<IShellView> view;HWND viewWindow{};const HWND focus=GetFocus();
    if(browser_&&SUCCEEDED(browser_->QueryActiveShellView(&view)))view->GetWindow(&viewWindow);
    GUITHREADINFO info{sizeof(info)};GetGUIThreadInfo(ownerThread_,&info);
    wchar_t name[64]{};GetClassNameW(focus,name,64);
    const bool editor=info.hwndCaret||!_wcsicmp(name,L"Edit")||!_wcsnicmp(name,L"RichEdit",8);
    if(!viewWindow||(focus!=viewWindow&&!IsChild(viewWindow,focus))||editor)return false;
    if(!repeated)History(-1);
    return true;
}
void Controller::MoveTab(int direction)
{
    // Moves the shown tab one place without wrapping, as Firefox does. Tabs
    // are tracked by identity, so a navigation in flight is unaffected.
    const size_t from=DisplayedIndex();
    if(from>=tabs_.size())return;
    const size_t to=direction<0?from-1:from+1;
    if((direction<0&&from==0)||to>=tabs_.size())return;
    Shift(from,to);
    TabChanged();
}
LRESULT CALLBACK Controller::KeyProc(int code, WPARAM remove, LPARAM data)
{
    if (code >= 0 && remove == PM_REMOVE)
    {
        auto *message = reinterpret_cast<MSG *>(data);
        HWND frame = GetAncestor(message->hwnd, GA_ROOT);
        Controller* self{};
        {std::lock_guard lock(controllersMutex);const auto found=controllers.find(frame);if(found!=controllers.end())self=found->second;}
        if (self && self->enabled_)
        {
            self->AddRef();
            struct Reference {Controller* owner;~Reference(){owner->Release();}} reference{self};
            bool eat = false;
            if (message->message == WM_KEYDOWN || message->message == WM_SYSKEYDOWN)
            {
                const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0,
                           alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
                const UINT key = static_cast<UINT>(message->wParam);
                if(key==VK_ESCAPE)self->CancelPointer();
                eat = self->Shortcut(key, ctrl, shift, alt, (message->lParam & (1ll << 30)) != 0);
            }
            else if (message->message == WM_XBUTTONUP)
            {
                self->History(GET_XBUTTON_WPARAM(message->wParam) == XBUTTON1 ? -1 : 1);
                eat = true;
            }
            else if (message->message == WM_XBUTTONDOWN)
                eat = true;
            if (!eat && self->restoreState_ && self->IsViewInput(*message))
                self->CancelRestore();
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
    {
        timing::Mark("beforenav");
        Remember();
    }
    else if (id == DISPID_NAVIGATECOMPLETE2 || id == DISPID_DOCUMENTCOMPLETE)
    {
        timing::Mark(id == DISPID_NAVIGATECOMPLETE2 ? "navcomplete" : "doccomplete");
        PostMessageW(frame_, SyncMessage, 0, 0);
    }
    else if (id == DISPID_NAVIGATEERROR)
    {
        timing::Mark("naverror");
        pending_.reset();
        switching_.Abandon();
        busyShown_ = false;
        CancelRestore();
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
bool Controller::Import(Tab tab, bool replaceInitial)
{
    if (!enabled_ || pending_ || tab.history.empty() || tabs_.size() >= MaxTabs)
        return false;
    tab.id = nextId++;
    tab.sourceFrame = nullptr;
    tab.sourceId = 0;
    tab.sourceProcess = 0;
    const auto current=replaceInitial?CurrentFolder(browser_.Get()):Pidl{};
    if(!enabled_||pending_||!frame_)return false;
    if (replaceInitial && tabs_.size() == 1 && Equal(current, tab.history[tab.position].folder))
    {
        tabs_[0] = std::move(tab);
        active_ = 0;
        BeginRestore(tabs_[0].history[tabs_[0].position]);
        TabChanged();
        return true;
    }
    auto id = tab.id;
    size_t position = tab.position;
    tabs_.push_back(std::move(tab));
    if (!Navigate(id, position, true))
    {
        const int index=Index(id);if(index>=0)tabs_.erase(tabs_.begin()+index);
        return false;
    }
    TabChanged();
    return true;
}
void Controller::DetachTab(size_t index)
{
    if(index>=tabs_.size()||pending_||detached_||!enabled_)return;
    const auto source=tabs_[index].id;Remember();
    const int position=Index(source);if(!enabled_||position<0||pending_)return;
    Detached request;request.id=source;request.start=GetTickCount64();
    request.packet=Serialize(tabs_[position],frame_);
    request.expected=tabs_[position].history[tabs_[position].position].folder;
    if(request.packet.empty())return;
    HRESULT hr=CoCreateInstance(CLSID_ShellBrowserWindow,nullptr,CLSCTX_LOCAL_SERVER,IID_PPV_ARGS(&request.created));
    SHANDLE_PTR handle{};
    if(SUCCEEDED(hr))hr=request.created->get_HWND(&handle);
    request.window=reinterpret_cast<HWND>(handle);
    if(FAILED(hr)||!request.window||request.window==frame_||!ExplorerRoot(request.window)||!enabled_||Index(source)<0){if(request.created&&request.window!=frame_)request.created->Quit();return;}
    VARIANT destination{},empty{};destination.vt=VT_ARRAY|VT_UI1;
    destination.parray=SafeArrayCreateVector(VT_UI1,0,ILGetSize(request.expected.p));
    if(!destination.parray){request.created->Quit();return;}
    void* bytes{};hr=SafeArrayAccessData(destination.parray,&bytes);
    if(SUCCEEDED(hr)){memcpy(bytes,request.expected.p,ILGetSize(request.expected.p));SafeArrayUnaccessData(destination.parray);}
    ComPtr<IWebBrowser2> created=request.created;
    if(SUCCEEDED(hr))hr=created->Navigate2(&destination,&empty,&empty,&empty,&empty);
    VariantClear(&destination);
    if(SUCCEEDED(hr))hr=created->put_Visible(VARIANT_TRUE);
    if(FAILED(hr)||!enabled_||Index(source)<0){created->Quit();return;}
    detached_=std::move(request);SetTimer(frame_,MaintenanceTimer,100,nullptr);
}
void Controller::StartDrag(size_t index)
{
    if (index >= tabs_.size() || pending_ || inDrag_)
        return;
    const ULONGLONG source = tabs_[index].id;
    Remember();
    const int resolved=Index(source);if(!enabled_||resolved<0||pending_)return;index=static_cast<size_t>(resolved);
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
        const HRESULT dropped=DoDragDrop(data.Get(), sourceObject.Get(), DROPEFFECT_MOVE | DROPEFFECT_COPY, &effect);
        inDrag_ = false;
        timing::Mark("dragdrop", effect, (dropped==DRAGDROP_S_DROP?1:0)|(sourceObject->released?2:0)|(sourceObject->escaped?4:0));
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
                timing::Mark("dragout", target == frame_ ? 1 : 0, (pending_ ? 1 : 0) | (detached_ ? 2 : 0));
                if (target != frame_)
                    DetachTab(position);
            }
        }
    }
    else
        inDrag_ = false;
    Release();
}
void Controller::RetryHeader()
{
    if(!headerRetryDeadline_)return;
    if(tearingDown_||closingFrame_||!(patch::LoadConfig().flags&patch::ExplorerTabs)||GetTickCount64()>=headerRetryDeadline_)headerRetryDeadline_=0;
    else Enable(true);
}
void Controller::NavigationDuty()
{
    if(!pending_)return;
    Synchronize();
    if(!pending_)return;
    const ULONGLONG elapsed=GetTickCount64()-pending_->start;
    if(elapsed>15000)
    {
        pending_.reset();
        switching_.Abandon();
        busyShown_=false;
        Synchronize();
        Layout();
        patch::Log(L"ExplorerTabs", L"The requested folder did not become ready within 15 seconds. The current "
                                    L"Explorer location was retained.");
        return;
    }
    // A slow folder earns a busy mark on its tab; a quick one never flashes it.
    if(!busyShown_&&tabs::SwitchModel::Busy(true,elapsed)){busyShown_=true;if(strip_)InvalidateRect(strip_,nullptr,FALSE);}
}
void Controller::DetachDuty()
{
    if(!detached_)return;
    // All borrowed storage is copied before COM or another window can
    // reenter this owner. Only the automation object's actual HWND accepts.
    const Detached attempt=*detached_;
    ComPtr<IServiceProvider> provider;ComPtr<IShellBrowser> targetBrowser;
    const HWND target=attempt.window;
    const auto sameRequest=[&]{return enabled_&&detached_&&detached_->id==attempt.id&&
        detached_->window==attempt.window&&detached_->created.Get()==attempt.created.Get();};
    if(!pending_&&active_<tabs_.size()&&tabs_[active_].id==attempt.id)Remember();
    int source=Index(attempt.id);
    if(sameRequest()&&!pending_&&(source<0||Serialize(tabs_[source],frame_)!=attempt.packet)) {
        detached_.reset();patch::Log(L"ExplorerTabs",L"The source tab changed while its new window was opening; the source was kept.");
    }
    bool ready=sameRequest()&&!pending_&&!attempt.transferred&&IsWindow(target)&&GetPropW(target,patch::TabsActiveProperty)&&
        SUCCEEDED(attempt.created.As(&provider))&&
        SUCCEEDED(provider->QueryService(SID_STopLevelBrowser,IID_PPV_ARGS(&targetBrowser)));
    if(ready&&Equal(CurrentFolder(targetBrowser.Get()),attempt.expected)&&sameRequest()&&!pending_&&
       (source=Index(attempt.id))>=0&&Serialize(tabs_[source],frame_)==attempt.packet)
    {
        COPYDATASTRUCT data{PacketMagic,static_cast<DWORD>(attempt.packet.size()),const_cast<BYTE*>(attempt.packet.data())};
        DWORD_PTR accepted{};
        const bool transferred=SendMessageTimeoutW(target,WM_COPYDATA,reinterpret_cast<WPARAM>(frame_),reinterpret_cast<LPARAM>(&data),SMTO_ABORTIFHUNG|SMTO_BLOCK,1000,&accepted)&&accepted;
        if(transferred&&sameRequest())detached_->transferred=true;
    }
    if(sameRequest()&&detached_->transferred&&!pending_) {
        source=Index(attempt.id);
        if(source<0)detached_.reset();
        else if(Serialize(tabs_[source],frame_)!=attempt.packet)detached_.reset();
        else if(!detached_->closeRequested) {
            const bool last=tabs_.size()==1;
            Close(source,&attempt.packet);
            if(sameRequest()) {
                if(Index(attempt.id)<0)detached_.reset();
                else if(last)detached_->closeRequested=true;
            }
        }
    }
    if(detached_&&GetTickCount64()-detached_->start>10000) {
        detached_.reset();patch::Log(L"ExplorerTabs",L"The created window did not accept the detached tab. The original tab was kept.");
    }
}
void Controller::Timer()
{
    RetryHeader();
    if (tabs_.empty() && startupPending_)
        Synchronize();
    if (startupPending_ && GetTickCount64() - startupStart_ >= 10000)
        CompleteStartupGuard();
    NavigationDuty();
    if (restoreState_ && GetTickCount64() >= restoreDeadline_)
        ApplyRestore();
    DetachDuty();
    if(enabled_&&frame_&&GetPropW(frame_,WindowSessionProperty))LoadWindowSession();
    if (frame_ && !startupPending_ && !pending_ && !restoreState_ && !detached_ && !headerRetryDeadline_&&!GetPropW(frame_,WindowSessionProperty))
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
    timing::Mark("dragenter", dropPrivate_ ? 1 : 0, *effect);
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
    if(layout_.overflowing) {
        const int edge=MulDiv(header::ControlDip,static_cast<int>(dpi_),96),step=MulDiv(8,static_cast<int>(dpi_),96);
        if(local.x<layout_.tabViewport.left+edge)ScrollBy(-step);
        else if(local.x>=layout_.tabViewport.right-edge)ScrollBy(step);
    }
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
            timing::Mark("droptab", *effect, (tab ? 1 : 0) | (pending_ ? 2 : 0) | (enabled_ ? 4 : 0));
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
header::TargetId Controller::ChildTarget(LONG child) const
{
    if(child<=0)return {};
    if(child<=static_cast<LONG>(tabs_.size()*2)){const auto& tab=tabs_[(child-1)/2];return {child%2?header::TargetKind::Tab:header::TargetKind::Close,tab.id};}
    if(child==static_cast<LONG>(tabs_.size()*2+1))return {header::TargetKind::NewTab,0};
    if(child==static_cast<LONG>(tabs_.size()*2+2))return {header::TargetKind::Overflow,0};
    return {};
}
LONG Controller::TargetChild(header::TargetId target) const
{
    if(target.kind==header::TargetKind::NewTab)return static_cast<LONG>(tabs_.size()*2+1);
    if(target.kind==header::TargetKind::Overflow)return static_cast<LONG>(tabs_.size()*2+2);
    const int index=Index(target.tab);
    if(index<0)return 0;
    return target.kind==header::TargetKind::Tab?index*2+1:target.kind==header::TargetKind::Close?index*2+2:0;
}
HRESULT Controller::ChildName(LONG child, BSTR *out)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;
    std::wstring name;
    if (child == 0)
        name = L"File Explorer tabs";
    else if (child > 0 && static_cast<size_t>(child) <= tabs_.size()*2)
        name = (child%2?L"":L"Close ")+tabs_[(child-1)/2].title;
    else if (child == static_cast<LONG>(tabs_.size()*2 + 1))
        name = L"New tab";
    else if (child == static_cast<LONG>(tabs_.size()*2 + 2))
        name = L"All tabs and options";
    else
        return E_INVALIDARG;
    *out = SysAllocString(name.c_str());
    return *out ? S_OK : E_OUTOFMEMORY;
}
bool Controller::ChildRect(LONG child, RECT &out) const
{
    if(!strip_||!rendererReady_||!header::IsCurrent(frame_,layout_.allocation))
        return false;
    if (child == 0)
        out=layout_.viewport;
    else if(child>0&&static_cast<size_t>(child)<=tabs_.size()*2){
        const auto* tab=layout_.Find(tabs_[(child-1)/2].id);
        if(!tab)return false;
        const RECT part=child%2?tab->body:tab->close;
        if(!IntersectRect(&out,&part,&layout_.tabViewport))out={};
    }
    else if (child == static_cast<LONG>(tabs_.size()*2 + 1))
        out=layout_.newTab;
    else if (child == static_cast<LONG>(tabs_.size()*2 + 2))
        out=layout_.overflow;
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
    if(!ValidChild(v))return E_INVALIDARG;
    out->vt = VT_I4;
    out->lVal = STATE_SYSTEM_FOCUSABLE;
    const LONG child=v.lVal;const auto target=ChildTarget(child);
    if (target.kind==header::TargetKind::Tab)
    {
        out->lVal |= STATE_SYSTEM_SELECTABLE;
        if (target.tab==DisplayedId())
            out->lVal |= STATE_SYSTEM_SELECTED;
        const auto* tab=layout_.Find(target.tab);
        if(!tab||!tab->Visible()||!header::IsCurrent(frame_,layout_.allocation))
            out->lVal |= STATE_SYSTEM_OFFSCREEN;
    }
    if (!enabled_ || !strip_)
        out->lVal |= STATE_SYSTEM_INVISIBLE;
    RECT bounds{};if(child&&(!ChildRect(child,bounds)||IsRectEmpty(&bounds)))out->lVal|=STATE_SYSTEM_OFFSCREEN;
    if(!Ready()||(target.kind==header::TargetKind::NewTab&&tabs_.size()>=MaxTabs))out->lVal|=STATE_SYSTEM_UNAVAILABLE;
    if(busyShown_&&(child==0||target.tab==DisplayedId()))out->lVal|=STATE_SYSTEM_BUSY;
    if (GetFocus() == strip_ && (child==0||target==focusedTarget_))
        out->lVal |= STATE_SYSTEM_FOCUSED;
    return S_OK;
}
HRESULT Controller::accLocation(long *x, long *y, long *w, long *h, VARIANT v)
{
    if (!x || !y || !w || !h)
        return E_POINTER;
    *x=*y=*w=*h=0;if(!ValidChild(v))return E_INVALIDARG;
    RECT r{};if(!ChildRect(v.lVal,r))return S_FALSE;
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
    if(!ValidChild(from))return E_INVALIDARG;
    LONG child = from.lVal;
    LONG count = static_cast<LONG>(tabs_.size()*2 + 2), next = 0;
    if (direction == NAVDIR_FIRSTCHILD&&child==0)
        next = 1;
    else if (direction == NAVDIR_LASTCHILD&&child==0)
        next = count;
    else if (child>0&&(direction == NAVDIR_NEXT || direction == NAVDIR_RIGHT))
        next = child + 1;
    else if (child>0&&(direction == NAVDIR_PREVIOUS || direction == NAVDIR_LEFT))
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
    out->vt = VT_I4;
    out->lVal=Ready()?TargetChild(layout_.Hit(p)):CHILDID_SELF;
    return S_OK;
}
HRESULT Controller::accDoDefaultAction(VARIANT v)
{
    if (!ValidChild(v)||!v.lVal)
        return E_INVALIDARG;
    if(!Ready())return S_FALSE;
    const auto target=ChildTarget(v.lVal);
    if(target.kind==header::TargetKind::NewTab&&tabs_.size()>=MaxTabs)return S_FALSE;
    InvokeTarget(target);return S_OK;
}
HRESULT Controller::accSelect(long flags,VARIANT child)
{
    if(!ValidChild(child))return E_INVALIDARG;
    if(!flags)return S_OK;
    if(!child.lVal){if(flags!=SELFLAG_TAKEFOCUS)return E_INVALIDARG;if(!Ready())return S_FALSE;SetFocus(strip_);return S_OK;}
    const auto target=ChildTarget(child.lVal);
    if(flags&~(SELFLAG_TAKEFOCUS|SELFLAG_TAKESELECTION))return E_INVALIDARG;
    if((flags&SELFLAG_TAKESELECTION)&&target.kind!=header::TargetKind::Tab)return E_INVALIDARG;
    if(!Ready())return S_FALSE;
    if(flags&SELFLAG_TAKEFOCUS)FocusTarget(target);
    if(flags&SELFLAG_TAKESELECTION)return Activate(Index(target.tab))?S_OK:S_FALSE;
    return S_OK;
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
    {
        // LastSession budget: every tab survives, detail is given up in steps.
        struct SyntheticItem { USHORT cb; BYTE data[196]; USHORT end; };
        auto item = [](BYTE seed) {
            SyntheticItem raw{};
            raw.cb = sizeof(raw.cb) + sizeof(raw.data);
            std::memset(raw.data, seed, sizeof(raw.data));
            return Pidl(reinterpret_cast<PCIDLIST_ABSOLUTE>(&raw));
        };
        auto decode = [](const std::vector<BYTE> &bytes, std::vector<Tab> &tabs, DWORD &active) {
            Reader reader{bytes.data(), bytes.size()};
            if (reader.number<DWORD>() != PacketMagic || reader.number<DWORD>() != 3)
                return false;
            const auto count = reader.number<DWORD>();
            active = reader.number<DWORD>();
            for (DWORD i = 0; i < count && reader.ok; ++i)
                tabs.push_back(reader.tab());
            return reader.ok && !reader.left && tabs.size() == count && active < count;
        };
        auto build = [&](size_t count, size_t entries, size_t selected) {
            std::vector<Tab> tabs(count);
            for (size_t i = 0; i < tabs.size(); ++i)
                for (size_t e = 0; e < entries; ++e)
                {
                    ViewState entry;
                    entry.folder = item(static_cast<BYTE>(e + 1));
                    entry.focused = item(static_cast<BYTE>(e + 2));
                    entry.anchor = item(static_cast<BYTE>(e + 3));
                    for (size_t k = 0; k < selected; ++k)
                        entry.selected.push_back(item(static_cast<BYTE>(k + 7)));
                    tabs[i].history.push_back(std::move(entry));
                    tabs[i].position = entries - 1;
                }
            return tabs;
        };
        const size_t active = 3;
        auto trimmed = build(64, 60, 2);
        auto bytes = EncodeSession(trimmed, active, SessionBudget);
        std::vector<Tab> decoded;
        DWORD decodedActive{};
        const bool decodedOk = decode(bytes, decoded, decodedActive);
        checks.emplace_back("session_budget_trims_background_history",
                            !bytes.empty() && bytes.size() <= SessionBudget && decodedOk && decodedActive == active &&
                                decoded[active].history.size() == 60 && decoded[0].history.size() == BackgroundHistory + 1 &&
                                decoded[0].position == BackgroundHistory && decoded[0].history.back().selected.size() == 2 &&
                                decoded[0].history.front().selected.empty());
        auto heavy = build(MaxTabs, 20, 100);
        bytes = EncodeSession(heavy, active, SessionBudget);
        decoded.clear();
        const bool heavyOk = decode(bytes, decoded, decodedActive);
        checks.emplace_back("session_budget_keeps_every_tab",
                            !bytes.empty() && bytes.size() <= SessionBudget && heavyOk && decoded.size() == MaxTabs &&
                                decoded[0].history.size() == 1 && decoded[0].history[0].selected.empty() &&
                                decoded[active].history[0].selected.size() == 100);
        auto light = build(64, 3, 1);
        bytes = EncodeSession(light, active, SessionBudget);
        decoded.clear();
        checks.emplace_back("session_small_keeps_full_detail",
                            decode(bytes, decoded, decodedActive) && decoded[5].history.size() == 3 &&
                                decoded[5].history[0].selected.size() == 1);
    }
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
