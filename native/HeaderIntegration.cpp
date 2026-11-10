#include "HeaderIntegration.h"
#include "vendor/minhook/include/MinHook.h"
#include <bcrypt.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <intrin.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace header {
namespace {
using BorderFn=HRESULT(WINAPI*)(void*,void**,const RECT*);
using BeginFn=HPAINTBUFFER(WINAPI*)(HDC,const RECT*,BP_BUFFERFORMAT,BP_PAINTPARAMS*,HDC*);
using EndFn=HRESULT(WINAPI*)(HPAINTBUFFER,BOOL);
thread_local std::uint64_t nextOwner{1};
struct Context {
    std::uint64_t owner{nextOwner++};
    RECT nativeBorder{};
    NativeGeometry native;
    int row{},appliedRow{-1};
    bool measured{},queued{},dirty{true},mutating{},captionLeased{},captionLost{},detaching{};
    DWORD originalCaptionBit{};
    std::uint64_t revision{1};
    HWINEVENTHOOK events{};
};
// A frame, its borrowed native callbacks and its WinEvents all belong to one STA.
thread_local std::map<HWND,Context> frames;
struct PaintOwner {HDC drawing{};HWND frame{};std::uint64_t owner{};};
thread_local std::map<HPAINTBUFFER,PaintOwner> paints;
// Only process-wide hook mutations take this mutex; native callbacks never do.
std::mutex hookMutex;
size_t hookUsers{};
PVOID* borderSlot{};
BorderFn originalBorder{};
BeginFn originalBegin{};
EndFn originalEnd{};
HMODULE themeModule{};
void* beginTarget{};
void* endTarget{};
bool themeReady{},hooksCreated{};
constexpr BYTE ribbonHash[]={0x94,0x1d,0xcf,0xcd,0x96,0xb3,0xd9,0x79,0x99,0x45,0xa5,0xab,0x1b,0x4d,0x4b,0x7d,0x68,0x59,0x68,0x2a,0x0a,0xf9,0xdf,0x38,0x22,0x35,0x68,0xd7,0xf3,0xc4,0x6a,0x2f};
constexpr BYTE themeHash[]={0x85,0xe2,0x67,0x87,0x28,0x2d,0x70,0xd2,0x03,0x02,0x6e,0x39,0x32,0x42,0xa8,0x61,0x6c,0xfd,0x83,0x2d,0x4e,0x68,0xdb,0x46,0x61,0x51,0x30,0x1b,0x60,0x63,0xaf,0x4e};

bool Image(HMODULE library,const BYTE (&expected)[32],std::vector<BYTE>* image=nullptr) {
    wchar_t path[32768]{};
    if(!library||!GetModuleFileNameW(library,path,32768))return false;
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};BYTE digest[32]{},buffer[65536];DWORD bytes{};
    bool ok=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0&&
        BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0;
    while(ok){
        if(!ReadFile(file,buffer,sizeof(buffer),&bytes,nullptr)){ok=false;break;}
        if(!bytes)break;
        if(image)image->insert(image->end(),buffer,buffer+bytes);
        ok=BCryptHashData(hash,buffer,bytes,0)>=0;
    }
    if(ok)ok=BCryptFinishHash(hash,digest,sizeof(digest),0)>=0;
    if(hash)BCryptDestroyHash(hash);
    if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);
    CloseHandle(file);
    return ok&&memcmp(digest,expected,sizeof(digest))==0;
}
bool PristineEntry(void* target,const std::vector<BYTE>& image) {
    const auto* base=reinterpret_cast<const BYTE*>(themeModule);
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    const auto* section=IMAGE_FIRST_SECTION(nt);
    const auto rva=reinterpret_cast<const BYTE*>(target)-base;
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i,++section){
        if(rva<section->VirtualAddress||rva+32>section->VirtualAddress+section->SizeOfRawData)continue;
        const size_t offset=section->PointerToRawData+static_cast<size_t>(rva-section->VirtualAddress);
        return offset+32<=image.size()&&!memcmp(target,image.data()+offset,32);
    }
    return false;
}
void Queue(HWND frame,Context& context) {
    if(context.mutating)return;
    if(!context.queued)context.queued=PostMessageW(frame,ChangedMessage(),0,0)!=FALSE;
}
bool CaptionOwned(HWND frame,Context& context) {
    if(context.captionLeased&&!(reinterpret_cast<ULONG_PTR>(GetPropW(frame,MAKEINTATOM(0xa914)))&WTNCA_NODRAWCAPTION)){
        // Another writer released this same bit. Its later value wins; abandon
        // caption sharing for this attachment instead of restoring/fighting it.
        context.captionLeased=false;context.captionLost=true;++context.revision;
        Queue(frame,context);
    }
    return context.captionLeased;
}
bool SameGeometry(const NativeGeometry& a,const NativeGeometry& b) {
    return EqualRect(&a.paint,&b.paint)&&EqualRect(&a.title,&b.title)&&
        EqualRect(&a.custom[0],&b.custom[0])&&EqualRect(&a.custom[1],&b.custom[1])&&
        a.windowSize.cx==b.windowSize.cx&&a.windowSize.cy==b.windowSize.cy&&
        a.clientOffset.x==b.clientOffset.x&&a.clientOffset.y==b.clientOffset.y&&a.dpi==b.dpi&&a.complete==b.complete;
}
bool FrameMetrics(HWND frame,NativeGeometry& native) {
    RECT window{};POINT origin{};
    if(!GetWindowRect(frame,&window)||!ClientToScreen(frame,&origin))return false;
    native.windowSize={window.right-window.left,window.bottom-window.top};
    native.clientOffset={origin.x-window.left,origin.y-window.top};
    native.dpi=GetDpiForWindow(frame);
    return native.dpi!=0;
}
HPAINTBUFFER WINAPI ObserveBegin(HDC dc,const RECT* rect,BP_BUFFERFORMAT format,BP_PAINTPARAMS* parameters,HDC* drawing) {
    const auto caller=reinterpret_cast<const BYTE*>(_ReturnAddress());
    HWND actual=WindowFromDC(dc);
    if(!actual)for(const auto& paint:paints)if(paint.second.drawing==dc){
        const auto current=frames.find(paint.second.frame);
        if(current!=frames.end()&&current->second.owner==paint.second.owner){actual=paint.second.frame;break;}
    }
    auto found=frames.find(actual);
    if(found!=frames.end()&&caller==reinterpret_cast<const BYTE*>(themeModule)+0x209eb&&rect){
        NativeGeometry native;
        // Exact UXTHEME NcPaintCaption public-API call. These are offsets within
        // this paint's metrics, not screen/pixel displacements.
        const auto* fields=reinterpret_cast<const BYTE*>(rect);
        memcpy(&native.paint,fields,sizeof(RECT));
        memcpy(&native.title,fields+0x40,sizeof(RECT));
        memcpy(&native.custom[0],fields+0x130,sizeof(RECT));
        memcpy(&native.custom[1],fields+0x140,sizeof(RECT));
        native.complete=FrameMetrics(actual,native);
        auto& context=found->second;
        if(context.dirty||!SameGeometry(context.native,native)){
            context.native=native;context.dirty=false;++context.revision;Queue(actual,context);
        }
    }
    const std::uint64_t owner=found!=frames.end()?found->second.owner:0;
    HPAINTBUFFER result=originalBegin(dc,rect,format,parameters,drawing);
    found=frames.find(actual);
    if(result&&drawing&&owner&&found!=frames.end()&&found->second.owner==owner)paints[result]={*drawing,actual,owner};
    return result;
}
HRESULT WINAPI ObserveEnd(HPAINTBUFFER paint,BOOL update) {
    paints.erase(paint);
    return originalEnd(paint,update);
}
bool Exchange(PVOID replacement,PVOID expected) {
    DWORD protect{};if(!VirtualProtect(borderSlot,sizeof(PVOID),PAGE_READWRITE,&protect))return false;
    const PVOID before=InterlockedCompareExchangePointer(borderSlot,replacement,expected);
    DWORD ignored{};VirtualProtect(borderSlot,sizeof(PVOID),protect,&ignored);
    return before==expected;
}
HRESULT WINAPI Border(void* self,void** client,const RECT* border) {
    const HWND frame=client&&*client?*reinterpret_cast<HWND*>(reinterpret_cast<BYTE*>(*client)+0x58):nullptr;
    auto found=frames.find(frame);RECT allocation{};int appliedRow{};std::uint64_t owner{};
    const bool controlled=found!=frames.end()&&border;
    if(controlled){
        auto& context=found->second;
        owner=context.owner;
        const bool changed=!context.measured||!EqualRect(&context.nativeBorder,border);
        context.nativeBorder=*border;context.measured=true;
        appliedRow=context.row;allocation=*border;allocation.top+=appliedRow;
        if(changed){++context.revision;Queue(frame,context);}
    }
    // Native self/client pointers are borrowed only for this callback.
    const HRESULT result=originalBorder(self,client,controlled?&allocation:border);
    found=frames.find(frame);
    if(controlled&&SUCCEEDED(result)&&found!=frames.end()&&found->second.owner==owner&&
       found->second.row==appliedRow)found->second.appliedRow=appliedRow;
    return result;
}
bool Install() {
    std::lock_guard lock(hookMutex);
    if(hookUsers){
        if(!borderSlot||*borderSlot!=reinterpret_cast<PVOID>(Border))return false;
        ++hookUsers;return true;
    }
    HMODULE retained{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&Border),&retained))return false;
    const HMODULE ribbon=GetModuleHandleW(L"UIRibbon.dll");
    if(!ribbon||!Image(ribbon,ribbonHash))return false;
    borderSlot=reinterpret_cast<PVOID*>(reinterpret_cast<BYTE*>(ribbon)+0x21a5f0+0x18);
    originalBorder=reinterpret_cast<BorderFn>(reinterpret_cast<BYTE*>(ribbon)+0x11900);
    if(*borderSlot!=reinterpret_cast<PVOID>(Border)&&
       (*borderSlot!=reinterpret_cast<PVOID>(originalBorder)||!Exchange(reinterpret_cast<PVOID>(Border),reinterpret_cast<PVOID>(originalBorder))))return false;
    themeModule=GetModuleHandleW(L"uxtheme.dll");
    std::vector<BYTE> image;
    themeReady=Image(themeModule,themeHash,&image);
    if(themeReady){
        beginTarget=reinterpret_cast<void*>(GetProcAddress(themeModule,"BeginBufferedPaint"));
        endTarget=reinterpret_cast<void*>(GetProcAddress(themeModule,"EndBufferedPaint"));
        if(!hooksCreated&&beginTarget&&endTarget&&PristineEntry(beginTarget,image)&&PristineEntry(endTarget,image)){
            const auto status=MH_Initialize();
            if(status==MH_OK||status==MH_ERROR_ALREADY_INITIALIZED){
                const auto first=MH_CreateHook(beginTarget,reinterpret_cast<void*>(ObserveBegin),reinterpret_cast<void**>(&originalBegin));
                const auto second=first==MH_OK?MH_CreateHook(endTarget,reinterpret_cast<void*>(ObserveEnd),reinterpret_cast<void**>(&originalEnd)):MH_UNKNOWN;
                hooksCreated=first==MH_OK&&second==MH_OK;
            }
        }
        if(hooksCreated){
            const auto end=MH_EnableHook(endTarget);
            const auto begin=(end==MH_OK||end==MH_ERROR_ENABLED)?MH_EnableHook(beginTarget):MH_UNKNOWN;
            themeReady=(end==MH_OK||end==MH_ERROR_ENABLED)&&(begin==MH_OK||begin==MH_ERROR_ENABLED);
            if(!themeReady){MH_DisableHook(beginTarget);MH_DisableHook(endTarget);}
        }else themeReady=false;
    }
    hookUsers=1;
    return true; // Unknown caption ownership still has the exact native row.
}
void Uninstall() {
    std::lock_guard lock(hookMutex);
    if(!hookUsers||--hookUsers)return;
    if(borderSlot)Exchange(reinterpret_cast<PVOID>(originalBorder),reinterpret_cast<PVOID>(Border));
    if(hooksCreated){
        // The vendored ownership check refuses to overwrite a later detour.
        // Keep trampolines and this DLL pinned for in-flight/chained callbacks.
        MH_DisableHook(beginTarget);MH_DisableHook(endTarget);
    }
    themeReady=false;
}
void Repaint(HWND frame) {
    RECT window{},buttons{};POINT client{};
    if(!GetWindowRect(frame,&window)||!ClientToScreen(frame,&client))return;
    const UINT dpi=GetDpiForWindow(frame);
    if(FAILED(DwmGetWindowAttribute(frame,DWMWA_CAPTION_BUTTON_BOUNDS,&buttons,sizeof(buttons))))
        buttons={0,0,window.right-window.left,MulDiv(30,static_cast<int>(dpi),96)};
    // Refresh only the native caption band, never the folder view/children.
    const RECT caption{window.left-client.x,window.top+buttons.top-client.y,
        window.right-client.x,window.top+buttons.bottom-client.y};
    RedrawWindow(frame,&caption,nullptr,RDW_INVALIDATE|RDW_FRAME|RDW_UPDATENOW|RDW_NOCHILDREN);
}
void Reflow(HWND frame) {
    SendMessageW(frame,WM_SETTINGCHANGE,SPI_SETNONCLIENTMETRICS,0);
    SendMessageW(frame,WM_THEMECHANGED,0,0);
}
bool Reserve(HWND frame,int pixels) {
    auto found=frames.find(frame);if(found==frames.end())return false;
    auto& context=found->second;
    if(context.row==pixels)return context.measured&&context.appliedRow==pixels;
    const bool wasMutating=context.mutating;
    const auto owner=context.owner;
    context.row=pixels;context.appliedRow=-1;++context.revision;context.mutating=true;
    if(pixels)SetPropW(frame,RowProperty,reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(pixels)));
    else RemovePropW(frame,RowProperty);
    Reflow(frame);
    found=frames.find(frame);if(found==frames.end()||found->second.owner!=owner)return false;
    found->second.mutating=wasMutating;
    // Native layout moves its children without repainting every exposed pixel.
    // Queue their paint only when the reserved height actually changes.
    RedrawWindow(frame,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
    return found->second.measured&&found->second.appliedRow==pixels;
}
bool CaptionLease(HWND frame,bool acquire) {
    auto found=frames.find(frame);if(found==frames.end())return false;
    auto& context=found->second;
    CaptionOwned(frame,context);
    if(acquire&&context.captionLost)return false;
    if(context.captionLeased==acquire)return true;
    if(acquire)context.originalCaptionBit=static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(GetPropW(frame,MAKEINTATOM(0xa914))))&WTNCA_NODRAWCAPTION;
    WTA_OPTIONS options{acquire?WTNCA_NODRAWCAPTION:context.originalCaptionBit,WTNCA_NODRAWCAPTION};
    const bool wasMutating=context.mutating;
    const auto owner=context.owner;
    context.mutating=true;
    const HRESULT result=SetWindowThemeAttribute(frame,WTA_NONCLIENT,&options,sizeof(options));
    found=frames.find(frame);if(found==frames.end()||found->second.owner!=owner)return false;
    found->second.mutating=wasMutating;
    if(FAILED(result))return false;
    found->second.captionLeased=acquire;
    Repaint(frame);
    return true;
}
void CALLBACK CommandMoved(HWINEVENTHOOK hook,DWORD,HWND window,LONG,LONG,DWORD,DWORD) {
    wchar_t name[64]{};GetClassNameW(window,name,64);if(wcscmp(name,L"NetUIHWND"))return;
    const HWND frame=GetAncestor(window,GA_ROOT);
    auto found=frames.find(frame);
    if(found!=frames.end()&&found->second.events==hook)Invalidate(frame);
}
}
UINT ChangedMessage() {
    static const UINT message=RegisterWindowMessageW(L"Win10NativePatch.HeaderLayoutChanged.v1");return message;
}
bool Attach(HWND frame) {
    DWORD process{};
    if(GetWindowThreadProcessId(frame,&process)!=GetCurrentThreadId()||process!=GetCurrentProcessId())return false;
    if(const auto found=frames.find(frame);found!=frames.end())return !found->second.detaching;
    if(!Install())return false;
    frames.emplace(frame,Context{});
    const auto events=SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE,EVENT_OBJECT_LOCATIONCHANGE,nullptr,
        CommandMoved,GetCurrentProcessId(),GetCurrentThreadId(),WINEVENT_OUTOFCONTEXT);
    if(!events){Detach(frame);return false;}
    frames.at(frame).events=events;
    const auto owner=frames.at(frame).owner;
    frames.at(frame).mutating=true;Reflow(frame);
    auto found=frames.find(frame);if(found==frames.end()||found->second.owner!=owner)return false;
    found->second.mutating=false;
    Repaint(frame);
    found=frames.find(frame);
    if(found==frames.end()||found->second.owner!=owner)return false;
    if(!IsWindow(frame)){Detach(frame,false);return false;}
    return true;
}
void Invalidate(HWND frame) {
    auto found=frames.find(frame);
    if(found==frames.end()||found->second.mutating)return;
    auto& context=found->second;
    if(!context.dirty){context.dirty=true;++context.revision;}
    Queue(frame,context);
}
bool IsCurrent(HWND frame,const Allocation& allocation) {
    const auto found=frames.find(frame);
    if(found!=frames.end()&&allocation.region==Region::Caption&&!CaptionOwned(frame,found->second))return false;
    if(found==frames.end()||found->second.dirty||allocation.region==Region::Unavailable||
       allocation.owner!=found->second.owner||allocation.revision!=found->second.revision)return false;
    NativeGeometry now;
    if(!FrameMetrics(frame,now))return false;
    const auto& previous=allocation.native;
    return now.windowSize.cx==previous.windowSize.cx&&now.windowSize.cy==previous.windowSize.cy&&
        now.clientOffset.x==previous.clientOffset.x&&now.clientOffset.y==previous.clientOffset.y&&now.dpi==allocation.dpi;
}
bool Allocate(HWND frame,HWND surface,int minimumWidth,Allocation& allocation,bool transient) {
    allocation={};
    auto found=frames.find(frame);
    if(found==frames.end()||!surface||!IsWindow(surface)||GetParent(surface)!=frame||
       GetWindowThreadProcessId(surface,nullptr)!=GetCurrentThreadId()||!borderSlot||
       *borderSlot!=reinterpret_cast<PVOID>(Border))return false;
    found->second.queued=false;
    const auto owner=found->second.owner;
    const bool wasMutating=found->second.mutating;
    found->second.mutating=true;
    struct Mutation {
        HWND frame;bool previous;std::uint64_t owner;
        ~Mutation(){auto current=frames.find(frame);if(current!=frames.end()&&current->second.owner==owner)current->second.mutating=previous;}
    } mutation{frame,wasMutating,owner};
    if(found->second.dirty)Repaint(frame);
    found=frames.find(frame);if(found==frames.end()||found->second.owner!=owner)return false;
    CaptionOwned(frame,found->second);
    RECT buttons{},slot{},client{};
    if(!GetClientRect(frame,&client))return false;
    bool caption=themeReady&&!found->second.captionLost&&!found->second.dirty&&
        SUCCEEDED(DwmGetWindowAttribute(frame,DWMWA_CAPTION_BUTTON_BOUNDS,&buttons,sizeof(buttons)))&&
        CaptionBounds(found->second.native,buttons,minimumWidth,slot);
    const UINT dpi=GetDpiForWindow(frame);
    if(!dpi)return false;
    int height=buttons.bottom-buttons.top;
    if(height<MulDiv(16,static_cast<int>(dpi),96)||height>MulDiv(56,static_cast<int>(dpi),96))height=MulDiv(30,static_cast<int>(dpi),96);
    // Mode hysteresis. Inside a size/position change the ribbon lays out its
    // caption controls asynchronously, so the caption can measure as "not
    // fitting" for that instant only. Switching to the row (and back on the
    // settled re-measure) reflows the whole ribbon twice per sizing step, which
    // stalls Explorer's sizing loop and breaks Windows' shared snap divider.
    // A transient pass therefore keeps the current mode and leaves the switch
    // to the queued, settled pass.
    const bool inCaption=found->second.captionLeased&&found->second.row==0;
    const bool inRow=found->second.row!=0;
    if(transient&&(inCaption||inRow)&&caption!=inCaption){
        found->second.queued=PostMessageW(frame,ChangedMessage(),0,0)!=FALSE;
        if(inRow)caption=false; // Stay in the row; its allocation below stays current.
        else{
            // Stay in the caption. Mark the geometry stale so IsCurrent rejects
            // this allocation: the strip stays hidden until the settled pass,
            // exactly as for any other stale geometry.
            auto& context=found->second;
            const RECT stale{context.native.title.left,buttons.top,
                std::min(context.native.title.right,buttons.left),buttons.bottom};
            if(stale.right>stale.left&&stale.bottom>stale.top){
                context.dirty=true;
                allocation.region=Region::Caption;
                allocation.bounds=Translate(stale,{-context.native.clientOffset.x,-context.native.clientOffset.y});
                allocation.native=context.native;allocation.owner=context.owner;
                allocation.revision=context.revision;allocation.dpi=dpi;
                return true;
            }
            // No usable caption slot at all: fall through to the ordinary switch.
        }
    }
    if(!caption&&!CaptionLease(frame,false))return false;
    if(!Reserve(frame,caption?0:height))return false;
    found=frames.find(frame);if(found==frames.end()||found->second.owner!=owner)return false;
    // Reflow/the public title setter may synchronously produce newer geometry.
    if(caption){
        if(!CaptionLease(frame,true))return false;
        found=frames.find(frame);if(found==frames.end()||found->second.owner!=owner)return false;
        if(found->second.dirty||!CaptionBounds(found->second.native,buttons,minimumWidth,slot)){
            if(!CaptionLease(frame,false)||!Reserve(frame,height))return false;
            caption=false;
        }
    }
    auto& context=frames.at(frame);
    if(!caption){
        if(!context.measured||client.right-client.left<minimumWidth)return false;
        slot={client.left,context.nativeBorder.top,client.right,context.nativeBorder.top+height};
        // Row allocation does not depend on missing caption geometry.
        if(context.dirty)context.native.complete=false;
        FrameMetrics(frame,context.native);context.dirty=false;
    }
    allocation.region=caption?Region::Caption:Region::AllocatedRow;
    allocation.bounds=slot;allocation.native=context.native;
    allocation.owner=context.owner;allocation.revision=context.revision;allocation.dpi=dpi;
    return slot.right>slot.left&&slot.bottom>slot.top;
}
void Detach(HWND frame,bool restore) {
    auto found=frames.find(frame);if(found==frames.end())return;
    if(found->second.detaching)return;
    found->second.detaching=true;
    if(found->second.events){UnhookWinEvent(found->second.events);found->second.events=nullptr;}
    if(restore){CaptionLease(frame,false);Reserve(frame,0);}
    frames.erase(frame);RemovePropW(frame,RowProperty);
    for(auto paint=paints.begin();paint!=paints.end();){
        if(paint->second.frame==frame)paint=paints.erase(paint);else ++paint;
    }
    Uninstall();
}
}
