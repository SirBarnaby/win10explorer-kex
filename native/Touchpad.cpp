// Native port of AppDock.at365's descriptor-backed settings cache (MIT).
// Copyright (c) 2026 yukichi-7sai. See licenses/AppDock-MIT.txt.
// Changes: Win10-only native C++, compare-before-restore enable cycles and v1 migration.
#include "Patch.h"
#include <servprov.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <memory>
#include <sstream>

namespace patch { namespace {
constexpr wchar_t TouchpadKey[]=L"Software\\Microsoft\\Windows\\CurrentVersion\\PrecisionTouchPad";
constexpr wchar_t JournalName[]=L"GestureCycle";
constexpr DWORD JournalMagic=0x32545057;
// Baseline/import order: mode, actions Left/Up/Right/Down, KeyParams L/U/R/D.
struct Value {DWORD present{},value{};bool operator==(const Value& o)const{return present==o.present&&(!present||value==o.value);}};
using Snapshot=std::array<Value,9>;
struct Journal {DWORD magic=JournalMagic,size=sizeof(Journal),version=2,requestedEnable=0,legacyRollback=0;Snapshot original{},owned{},restoreTarget{};std::array<DWORD,9> originalEffective{};};
struct Attempt {bool valid=false,hadJournal=false,hadImport=false;Snapshot before{},after{},imported{};Journal previous{},importedJournal{};std::array<DWORD,9> effectiveBefore{};};
Attempt lastAttempt;
const std::array<std::wstring,9>& Names(){static const std::array<std::wstring,9> n={L"ThreeFingerSlideEnabled",L"ThreeFingerLeft",L"ThreeFingerUp",L"ThreeFingerRight",L"ThreeFingerDown",L"ThreeFingerLeftKeyParams",L"ThreeFingerUpKeyParams",L"ThreeFingerRightKeyParams",L"ThreeFingerDownKeyParams"};return n;}
constexpr std::array<size_t,9> WriteOrder={5,6,7,8,1,2,3,4,0};
struct Failure:std::runtime_error {std::wstring detail;explicit Failure(std::wstring t):runtime_error("Touchpad settings failure"),detail(std::move(t)){};};
void Require(bool ok,const wchar_t* text){if(!ok)throw Failure(text);}
struct Key {HKEY h{};~Key(){if(h)RegCloseKey(h);}Key(const wchar_t* path,REGSAM access,bool create=false){LONG r=create?RegCreateKeyExW(HKEY_CURRENT_USER,path,0,nullptr,0,access,nullptr,&h,nullptr):RegOpenKeyExW(HKEY_CURRENT_USER,path,0,access,&h);if(r!=ERROR_SUCCESS)h=nullptr;}};
Value ReadValue(HKEY h,const wchar_t* name) {Value v;DWORD type{},size=sizeof(DWORD);LONG r=RegQueryValueExW(h,name,nullptr,&type,reinterpret_cast<BYTE*>(&v.value),&size);if(r==ERROR_FILE_NOT_FOUND)return {};Require(r==ERROR_SUCCESS&&type==REG_DWORD&&size==sizeof(DWORD),L"A touchpad setting has an unsupported registry format.");v.present=1;return v;}
Snapshot ReadValues(){Key key(TouchpadKey,KEY_QUERY_VALUE);Require(key.h!=nullptr,L"Precision Touchpad settings were not found.");Snapshot values;for(size_t i=0;i<9;i++)values[i]=ReadValue(key.h,Names()[i].c_str());return values;}
void WriteValue(HKEY h,size_t i,const Value& v){LONG r=v.present?RegSetValueExW(h,Names()[i].c_str(),0,REG_DWORD,reinterpret_cast<const BYTE*>(&v.value),sizeof(v.value)):RegDeleteValueW(h,Names()[i].c_str());Require(r==ERROR_SUCCESS||(!v.present&&r==ERROR_FILE_NOT_FOUND),L"Windows could not save a touchpad setting.");}
bool ValidSnapshot(const Snapshot& s){return std::all_of(s.begin(),s.end(),[](Value v){return v.present<=1;});}
bool ReadJournal(Journal& j){Key k(SettingsKey,KEY_QUERY_VALUE);if(!k.h)return false;DWORD type{},size=sizeof(j);LONG r=RegQueryValueExW(k.h,JournalName,nullptr,&type,reinterpret_cast<BYTE*>(&j),&size);if(r==ERROR_FILE_NOT_FOUND)return false;Require(r==ERROR_SUCCESS&&type==REG_BINARY&&size==sizeof(j)&&j.magic==JournalMagic&&j.version==2&&j.size==sizeof(j)&&j.requestedEnable<=1&&j.legacyRollback<=1&&ValidSnapshot(j.original)&&ValidSnapshot(j.owned)&&ValidSnapshot(j.restoreTarget),L"The saved touchpad restore record is not readable. It has been preserved.");return true;}
void SaveJournal(const Journal& j){Key k(SettingsKey,KEY_SET_VALUE|KEY_QUERY_VALUE,true);Require(k.h&&RegSetValueExW(k.h,JournalName,0,REG_BINARY,reinterpret_cast<const BYTE*>(&j),sizeof(j))==ERROR_SUCCESS&&RegFlushKey(k.h)==ERROR_SUCCESS,L"The original touchpad settings could not be safely backed up.");}
void RetireJournal(const Journal& j){Key k(SettingsKey,KEY_SET_VALUE|KEY_QUERY_VALUE,true);if(k.h&&RegSetValueExW(k.h,L"LastGestureCycle",0,REG_BINARY,reinterpret_cast<const BYTE*>(&j),sizeof(j))==ERROR_SUCCESS&&RegFlushKey(k.h)==ERROR_SUCCESS){RegDeleteValueW(k.h,JournalName);RegDeleteValueW(k.h,L"LegacyGestureBaseline");}}
struct Segment{size_t offset,size;uint32_t rva;};
struct Image {
    std::vector<BYTE> bytes;uint64_t base{};std::vector<Segment> segments;
    template<class T>T read(size_t at)const{Require(at<=bytes.size()&&sizeof(T)<=bytes.size()-at,L"A Windows settings descriptor is out of bounds.");T n;std::memcpy(&n,bytes.data()+at,sizeof(n));return n;}
    explicit Image(const wchar_t* name){wchar_t system[32768];UINT n=GetSystemDirectoryW(system,32768);Require(n&&n<32768,L"The Windows system directory is unavailable.");std::wstring path=std::wstring(system)+L"\\"+name;HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);Require(file!=INVALID_HANDLE_VALUE,L"A Windows settings binary could not be read.");LARGE_INTEGER len{};bool valid=GetFileSizeEx(file,&len)&&len.QuadPart>=512&&len.QuadPart<128*1024*1024;DWORD got{};if(valid){bytes.resize(static_cast<size_t>(len.QuadPart));valid=ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&got,nullptr)&&got==bytes.size();}CloseHandle(file);Require(valid,L"A Windows settings binary has an unsupported size.");Require(read<WORD>(0)==0x5a4d,L"Invalid Windows settings PE header.");size_t pe=read<DWORD>(0x3c);Require(pe>=64&&pe<=bytes.size()-264&&read<DWORD>(pe)==0x4550&&read<WORD>(pe+4)==0x8664,L"Only the verified Windows x64 settings format is supported.");size_t count=read<WORD>(pe+6),opt=pe+24,size=read<WORD>(pe+20),table=opt+size;Require(read<WORD>(opt)==0x20b&&count>=1&&count<=96&&size>=112&&table<=bytes.size()-count*40,L"Unsupported Windows settings PE layout.");base=read<uint64_t>(opt+24);for(size_t i=0;i<count;i++){size_t at=table+i*40,off=read<DWORD>(at+20),length=read<DWORD>(at+16);Require(off<=bytes.size()&&length<=bytes.size()-off,L"Invalid Windows settings PE section.");if(length)segments.push_back({off,length,read<DWORD>(at+12)});}}
    std::set<uint64_t> addresses(const std::wstring& text)const{std::set<uint64_t> found;size_t len=(text.size()+1)*sizeof(wchar_t);for(const auto& s:segments)if(s.size>=len)for(size_t at=s.offset;at<=s.offset+s.size-len;at+=2)if(!std::memcmp(bytes.data()+at,text.c_str(),len))found.insert(base+s.rva+at-s.offset);return found;}
};
std::array<int,9> Profile(){Image image(L"twinui.dll");auto paths=image.addresses(TouchpadKey);Require(!paths.empty(),L"The installed Windows touchpad cache format is not supported.");std::array<int,9> ids{};std::set<int> used;for(size_t i=0;i<9;i++){auto names=image.addresses(Names()[i]);std::set<int> matches;for(const auto& s:image.segments)for(size_t at=s.offset+16;at+24<=s.offset+s.size;at+=8)if(names.count(image.read<uint64_t>(at))&&paths.count(image.read<uint64_t>(at-8))){auto id=image.read<uint64_t>(at-16);if(id>0&&id<256&&image.read<uint64_t>(at+8)<=0xffffffffULL&&!image.read<uint64_t>(at+16))matches.insert(static_cast<int>(id));}Require(matches.size()==1,L"A Windows touchpad cache descriptor is missing or ambiguous.");ids[i]=*matches.begin();Require(used.insert(ids[i]).second,L"Windows touchpad cache setting IDs overlap.");}
    Image handler(L"SettingsHandlers_nt.dll");auto modes=handler.addresses(Names()[0]);for(size_t i=0;i<4;i++){auto keys=handler.addresses(Names()[5+i]),actions=handler.addresses(Names()[1+i]);unsigned matches=0;for(const auto&s:handler.segments)for(size_t at=s.offset;at+40<=s.offset+s.size;at+=8)if(keys.count(handler.read<uint64_t>(at))&&actions.count(handler.read<uint64_t>(at+8))&&handler.read<DWORD>(at+20)==0xffff&&modes.count(handler.read<uint64_t>(at+24))&&handler.read<DWORD>(at+32)==0xffff)matches++;Require(matches==1,L"The installed Windows gesture handler is not a verified profile.");}return ids;}
struct __declspec(uuid("4214f6fa-eb36-4e2f-9ca2-23fdc1832df7")) ISettingsCache: IUnknown {virtual HRESULT STDMETHODCALLTYPE OnSettingChanged(int)=0;virtual HRESULT STDMETHODCALLTYPE GetBOOL(int,BOOL*)=0;virtual HRESULT STDMETHODCALLTYPE GetDWORD(int,DWORD*)=0;virtual HRESULT STDMETHODCALLTYPE RegisterForSettingChange(void*,DWORD*)=0;virtual HRESULT STDMETHODCALLTYPE UnregisterForSettingChange(DWORD)=0;};
struct Cache {
    bool uninit=false;IServiceProvider* shell{};ISettingsCache* cache{};std::array<int,9> ids;
    Cache():ids(Profile()){HRESULT init=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);Require(SUCCEEDED(init)||init==RPC_E_CHANGED_MODE,L"COM could not initialize the touchpad settings connection.");uninit=SUCCEEDED(init);CLSID clsid;CLSIDFromString(L"{c2f03a33-21f5-47fa-b4bb-156362a2f239}",&clsid);HRESULT hr=CoCreateInstance(clsid,nullptr,CLSCTX_LOCAL_SERVER,IID_PPV_ARGS(&shell));if(SUCCEEDED(hr)){GUID service;CLSIDFromString(L"{53660488-8855-460b-a9ab-5cfc6b5012ca}",&service);hr=shell->QueryService(service,__uuidof(ISettingsCache),reinterpret_cast<void**>(&cache));}if(FAILED(hr)){if(shell)shell->Release();shell=nullptr;if(uninit)CoUninitialize();uninit=false;throw Failure(L"Windows could not connect to the live touchpad settings cache.");}}
    ~Cache(){if(cache)cache->Release();if(shell)shell->Release();if(uninit)CoUninitialize();}
    std::array<DWORD,9> read(){std::array<DWORD,9> values{};for(size_t i=0;i<9;i++)Require(SUCCEEDED(cache->GetDWORD(ids[i],&values[i])),L"Windows could not read its effective gesture settings.");return values;}
    void refresh(const Snapshot& expected){for(size_t i:WriteOrder)Require(SUCCEEDED(cache->OnSettingChanged(ids[i])),L"Windows has not reloaded the touchpad settings. Sign out and sign in.");auto actual=read();for(size_t i=0;i<9;i++)Require(!expected[i].present||actual[i]==expected[i].value,L"Windows gesture cache readback differs from the saved settings.");}
};
bool PrecisionTouchpad(){UINT count{};if(GetRawInputDeviceList(nullptr,&count,sizeof(RAWINPUTDEVICELIST))!=0||count>4096)return false;std::vector<RAWINPUTDEVICELIST> devices(count);if(GetRawInputDeviceList(devices.data(),&count,sizeof(RAWINPUTDEVICELIST))==UINT(-1))return false;for(const auto& d:devices){RID_DEVICE_INFO info{};info.cbSize=sizeof(info);UINT size=sizeof(info);if(GetRawInputDeviceInfoW(d.hDevice,RIDI_DEVICEINFO,&info,&size)!=UINT(-1)&&info.dwType==RIM_TYPEHID&&info.hid.usUsagePage==0x0d&&info.hid.usUsage==5)return true;}return false;}
Snapshot Desired(){Snapshot s;for(auto&v:s)v={1,0xffff};for(size_t i=0;i<4;i++)s[5+i]={1,(GestureKeys[i]<<16)|TouchpadModifierEncoding};return s;}
bool AnyPrivate(const std::array<DWORD,9>& v){if(v[0]!=0xffff)return false;for(size_t i=0;i<4;i++)if(v[1+i]==0xffff&&v[5+i]==((GestureKeys[i]<<16)|TouchpadModifierEncoding))return true;return false;}
bool AllPrivate(const std::array<DWORD,9>& v){if(v[0]!=0xffff)return false;for(size_t i=0;i<4;i++)if(v[1+i]!=0xffff||v[5+i]!=((GestureKeys[i]<<16)|TouchpadModifierEncoding))return false;return true;}
bool AnyLegacy(const std::array<DWORD,9>& v){constexpr DWORD arrows[]={VK_LEFT,VK_UP,VK_RIGHT,VK_DOWN};if(v[0]!=0xffff)return false;for(size_t i=0;i<4;i++)if(v[1+i]==0xffff&&v[5+i]==((arrows[i]<<16)|TouchpadModifierEncoding))return true;return false;}
bool Matches(const Snapshot& expected,const std::array<DWORD,9>& actual){for(size_t i=0;i<9;i++)if(expected[i].present&&expected[i].value!=actual[i])return false;return true;}
void CompareWrite(const Snapshot& before,const Snapshot& after){Key key(TouchpadKey,KEY_QUERY_VALUE|KEY_SET_VALUE);Require(key.h!=nullptr,L"Windows touchpad settings are not writable.");std::vector<size_t> written;try{for(size_t i:WriteOrder){Require(ReadValue(key.h,Names()[i].c_str())==before[i],L"Touchpad settings changed while the update was being applied. Try again.");if(!(before[i]==after[i])){WriteValue(key.h,i,after[i]);written.push_back(i);}}Require(RegFlushKey(key.h)==ERROR_SUCCESS,L"Windows could not flush the touchpad settings.");}catch(...){for(auto p=written.rbegin();p!=written.rend();++p)try{if(ReadValue(key.h,Names()[*p].c_str())==after[*p])WriteValue(key.h,*p,before[*p]);}catch(...){}throw;}}
Snapshot RestorePlan(const Journal& j,const Snapshot& current){Snapshot target=current;bool userChanged=false;for(size_t i=0;i<4;i++)if(!(current[1+i]==j.owned[1+i])||!(current[5+i]==j.owned[5+i]))userChanged=true;bool retainCustom=userChanged&&current[0]==j.owned[0];for(size_t i=0;i<4;i++)if(current[1+i]==j.owned[1+i]&&current[5+i]==j.owned[5+i]){target[1+i]=retainCustom?Value{1,j.originalEffective[1+i]}:j.original[1+i];target[5+i]=j.original[5+i];}if(current[0]==j.owned[0]&&!retainCustom)target[0]=j.original[0];return target;}
bool ImportBaseline(Journal& j){Key key(SettingsKey,KEY_QUERY_VALUE);if(!key.h)return false;Snapshot imported{};DWORD type{},size=sizeof(imported);LONG r=RegQueryValueExW(key.h,L"LegacyGestureBaseline",nullptr,&type,reinterpret_cast<BYTE*>(&imported),&size);if(r==ERROR_FILE_NOT_FOUND)return false;Require(r==ERROR_SUCCESS&&type==REG_BINARY&&size==sizeof(imported)&&ValidSnapshot(imported),L"The legacy gesture backup is not a supported format.");j.original=imported;DWORD mode=imported[0].present?imported[0].value:1;j.originalEffective[0]=mode;constexpr DWORD presets[4][4]={{0,0,0,0},{1,2,1,3},{4,2,4,3},{15,16,14,17}};for(size_t i=0;i<4;i++){j.originalEffective[1+i]=mode==0xffff?(imported[1+i].present?imported[1+i].value:0):mode<4?presets[mode][i]:0;j.originalEffective[5+i]=imported[5+i].present?imported[5+i].value:0;}return true;}
} // namespace
bool VerifyGestureProfile(std::wstring& detail){try{auto ids=Profile();(void)ids;detail=L"The installed x64 Windows gesture descriptors match the supported native profile.";return true;}catch(const Failure&e){detail=e.detail;return false;}catch(...){detail=L"Windows gesture settings could not be inspected.";return false;}}
std::pair<unsigned,unsigned> RunGestureModelSelfTests(){
    unsigned passed=0,total=0;auto test=[&](bool v){total++;if(v)passed++;};
    Journal j;j.original[0]={1,0xffff};j.originalEffective[0]=0xffff;
    constexpr DWORD originalActions[]={10,11,12,13};
    for(size_t i=0;i<4;i++){j.original[1+i]={1,originalActions[i]};j.originalEffective[1+i]=originalActions[i];}
    j.owned=Desired();auto effective=std::array<DWORD,9>{};for(size_t i=0;i<9;i++)effective[i]=j.owned[i].value;
    test(AllPrivate(effective)&&AnyPrivate(effective)&&!AnyLegacy(effective));
    auto partial=effective;partial[1]=4;test(!AllPrivate(partial)&&AnyPrivate(partial));
    Snapshot restored=RestorePlan(j,j.owned);test(restored==j.original);
    Snapshot edited=j.owned;edited[1]={1,4};restored=RestorePlan(j,edited);
    test(restored[1]==edited[1]&&restored[5]==edited[5]&&restored[2]==j.original[2]);
    edited=j.owned;edited[0]={1,2};restored=RestorePlan(j,edited);test(restored[0]==edited[0]);
    Journal preset=j;preset.original[0]={1,1};preset.originalEffective[1]=1;preset.originalEffective[2]=2;preset.originalEffective[3]=1;preset.originalEffective[4]=3;
    edited=preset.owned;edited[1]={1,4};restored=RestorePlan(preset,edited);
    test(restored[0].value==0xffff&&restored[1].value==4&&restored[2].value==2&&restored[3].value==1&&restored[4].value==3);
    Snapshot legacy=j.owned;constexpr DWORD arrows[]={VK_LEFT,VK_UP,VK_RIGHT,VK_DOWN};for(size_t i=0;i<4;i++)legacy[5+i].value=(arrows[i]<<16)|TouchpadModifierEncoding;
    for(size_t i=0;i<9;i++)effective[i]=legacy[i].value;test(AnyLegacy(effective)&&!AnyPrivate(effective));
    Journal rollback=j;rollback.original=legacy;rollback.originalEffective=effective;
    test(RestorePlan(rollback,j.owned)==legacy);
    test(j.original[1].value==10&&j.original[5].present==0);
    return {passed,total};
}
GestureStatus ReadGestureStatus(){
    GestureStatus s;s.precisionTouchpad=PrecisionTouchpad();
    try{
        Journal j;bool cycle=ReadJournal(j);
        if(!cycle){
            s.state=EffectiveState::Disabled;s.detail=s.precisionTouchpad?L"Three-finger snapping is off.":L"No Precision Touchpad was detected.";
            if(s.precisionTouchpad)try{Cache cache;auto effective=cache.read();s.privateBindingsActive=AnyPrivate(effective);if(s.privateBindingsActive||AnyLegacy(effective)){s.state=EffectiveState::Pending;s.detail=s.privateBindingsActive?L"Windows still has native private gesture bindings without a completed restore record.":L"Windows currently uses the previous Ctrl+Alt+Windows+Arrow gesture route. Native migration is not complete.";}}catch(const Failure&e){s.state=EffectiveState::Unavailable;s.detail=e.detail;}
            return s;
        }
        try{
            Cache cache;auto effective=cache.read();s.privateBindingsActive=AnyPrivate(effective)||j.requestedEnable;
            bool legacy=AnyLegacy(effective)&&(j.requestedEnable||j.legacyRollback);
            if(j.requestedEnable)s.state=AllPrivate(effective)?EffectiveState::Enabled:EffectiveState::Pending;
            else s.state=!s.privateBindingsActive&&!legacy&&Matches(j.restoreTarget,effective)?EffectiveState::Disabled:EffectiveState::Pending;
            if(s.state==EffectiveState::Enabled)s.detail=L"Windows is sending the four private three-finger snapping shortcuts.";
            else if(s.state==EffectiveState::Disabled)s.detail=L"The prior Windows gesture assignments are restored.";
            else if(legacy)s.detail=L"Windows still uses the previous arrow gesture route. Keep its helper until a verified native cutover or sign-in reload.";
            else s.detail=j.requestedEnable?L"Windows gesture settings changed or are pending. Review the touchpad assignments.":L"The restore settings are saved. Sign out and sign in to finish disabling gestures.";
        }catch(const Failure&e){s.privateBindingsActive=true;s.state=EffectiveState::Pending;s.detail=e.detail;}
        return s;
    }catch(const Failure&e){s.state=EffectiveState::Unavailable;s.detail=e.detail;try{Cache cache;s.privateBindingsActive=AnyPrivate(cache.read());}catch(...){s.privateBindingsActive=true;}return s;}
    catch(...){s.privateBindingsActive=true;s.detail=L"Touchpad status is unavailable; private inputs remain reserved.";return s;}
}
bool ConfigureGestures(bool enable,GestureStatus& status,std::wstring& error){
    HANDLE mutex=CreateMutexW(nullptr,FALSE,L"Local\\Win10NativePatch.Gestures.v2");
    if(!mutex){error=L"Windows could not lock gesture settings.";return false;}
    DWORD wait=WaitForSingleObject(mutex,3000);
    if(wait!=WAIT_OBJECT_0&&wait!=WAIT_ABANDONED){CloseHandle(mutex);error=L"Another gesture update is running.";return false;}
    bool success=false;
    lastAttempt=Attempt{};
    try{
        Journal j;bool existing=ReadJournal(j);
        lastAttempt.hadJournal=existing;lastAttempt.previous=j;
        Journal importProbe;if(ImportBaseline(importProbe)){lastAttempt.hadImport=true;lastAttempt.imported=importProbe.original;lastAttempt.importedJournal=importProbe;}
        if(!enable&&!existing&&ImportBaseline(j)){
            // Explicit disable during a v1-to-v2 migration also restores the
            // imported original assignments, even before any native enable.
            j.owned=Desired();constexpr DWORD arrows[]={VK_LEFT,VK_UP,VK_RIGHT,VK_DOWN};
            for(size_t i=0;i<4;i++)j.owned[5+i].value=(arrows[i]<<16)|TouchpadModifierEncoding;
            existing=true;
        }
        if(!enable&&!existing){status=ReadGestureStatus();success=true;}
        else if(enable){
            Require(PrecisionTouchpad(),L"No Precision Touchpad was detected on this PC.");
            Cache cache;auto effectiveBefore=cache.read();
            if(existing&&j.requestedEnable){status=ReadGestureStatus();Require(status.state==EffectiveState::Enabled,L"Windows gesture assignments changed. Disable gestures before enabling them again.");success=true;}
            else{
                Require(!existing||!AnyPrivate(effectiveBefore),L"A previous disable is pending. Sign out and sign in before enabling gestures again.");
                Snapshot before=ReadValues();lastAttempt.before=before;lastAttempt.effectiveBefore=effectiveBefore;
                Journal previous=j;bool preserveMigration=existing&&j.legacyRollback;
                j=Journal{};j.original=before;j.originalEffective=effectiveBefore;
                bool imported=ImportBaseline(j);
                if(preserveMigration){j.original=previous.original;j.originalEffective=previous.originalEffective;imported=true;}
                j.owned=Desired();j.restoreTarget=j.original;j.requestedEnable=1;SaveJournal(j);
                try{
                    // Reserve the private transport and its startup before
                    // Windows can emit the new keys. A crash between cache
                    // update and config commit therefore cannot strand input.
                    std::wstring runtimeError;
                    bool transportReady=ReconcileSnapRuntime(LoadConfig(),runtimeError);
                    Require(transportReady,runtimeError.empty()?L"The private gesture transport could not start.":runtimeError.c_str());
                    CompareWrite(before,j.owned);cache.refresh(j.owned);
                    if(imported){Key key(SettingsKey,KEY_SET_VALUE);if(key.h)RegDeleteValueW(key.h,L"LegacyGestureBaseline");}
                    lastAttempt.after=j.owned;lastAttempt.valid=true;success=true;
                }catch(...){
                    try{
                        Snapshot current=ReadValues();
                        // Restore the enable attempt's BEFORE state, including a
                        // working v1 route. Keep j.original as the true imported
                        // baseline; do not confuse rollback with uninstall.
                        Journal rollback=j;rollback.original=before;rollback.originalEffective=effectiveBefore;
                        j.requestedEnable=0;j.legacyRollback=AnyLegacy(effectiveBefore)?1:0;j.restoreTarget=RestorePlan(rollback,current);SaveJournal(j);
                        CompareWrite(current,j.restoreTarget);
                        try{cache.refresh(j.restoreTarget);if(!AnyPrivate(cache.read())&&!AnyLegacy(cache.read())&&!imported)RetireJournal(j);}catch(...){}
                    }catch(...){}
                    throw;
                }
            }
        }else{
            Snapshot current=ReadValues();lastAttempt.before=current;
            try{Cache cache;lastAttempt.effectiveBefore=cache.read();}catch(...){for(size_t i=0;i<9;i++)lastAttempt.effectiveBefore[i]=current[i].value;}
            j.requestedEnable=0;j.legacyRollback=0;j.restoreTarget=RestorePlan(j,current);SaveJournal(j);
            CompareWrite(current,j.restoreTarget);
            try{Cache cache;cache.refresh(j.restoreTarget);auto effective=cache.read();if(!AnyPrivate(effective)&&Matches(j.restoreTarget,effective))RetireJournal(j);}catch(const Failure&e){error=e.detail;}
            lastAttempt.after=j.restoreTarget;lastAttempt.valid=true;success=true;
        }
    }catch(const Failure&e){error=e.detail;}
    catch(...){error=L"The touchpad update failed. Its restore record has been preserved.";}
    ReleaseMutex(mutex);CloseHandle(mutex);status=ReadGestureStatus();NotifySettingsChanged();return success;
}
bool RollbackGestureAttempt(std::wstring& error){
    if(!lastAttempt.valid)return true;
    HANDLE mutex=CreateMutexW(nullptr,FALSE,L"Local\\Win10NativePatch.Gestures.v2");
    if(!mutex){error=L"Gesture rollback could not acquire its settings lock.";return false;}
    DWORD wait=WaitForSingleObject(mutex,3000);if(wait!=WAIT_OBJECT_0&&wait!=WAIT_ABANDONED){CloseHandle(mutex);error=L"Gesture rollback is waiting for another settings update.";return false;}
    bool ok=false;
    try{
        Snapshot current=ReadValues();Journal rollback;rollback.original=lastAttempt.before;rollback.originalEffective=lastAttempt.effectiveBefore;rollback.owned=lastAttempt.after;
        Snapshot target=RestorePlan(rollback,current);Journal recovery;
        bool hasCurrent=ReadJournal(recovery);if(!hasCurrent){if(lastAttempt.hadImport)recovery=lastAttempt.importedJournal;else{recovery.original=lastAttempt.before;recovery.originalEffective=lastAttempt.effectiveBefore;}recovery.owned=Desired();}
        recovery.requestedEnable=0;recovery.legacyRollback=AnyLegacy(lastAttempt.effectiveBefore)?1:0;recovery.restoreTarget=target;SaveJournal(recovery);CompareWrite(current,target);
        bool refreshed=false;try{Cache cache;cache.refresh(target);refreshed=Matches(target,cache.read());}catch(...){}
        if(lastAttempt.hadJournal)SaveJournal(lastAttempt.previous);
        else if(refreshed)RetireJournal(recovery);
        if(lastAttempt.hadImport){Key key(SettingsKey,KEY_QUERY_VALUE|KEY_SET_VALUE,true);Require(key.h!=nullptr,L"The migration backup could not be restored.");Snapshot stored{};DWORD size=sizeof(stored),type{};LONG r=RegQueryValueExW(key.h,L"LegacyGestureBaseline",nullptr,&type,reinterpret_cast<BYTE*>(&stored),&size);if(r==ERROR_FILE_NOT_FOUND)Require(RegSetValueExW(key.h,L"LegacyGestureBaseline",0,REG_BINARY,reinterpret_cast<const BYTE*>(&lastAttempt.imported),sizeof(lastAttempt.imported))==ERROR_SUCCESS&&RegFlushKey(key.h)==ERROR_SUCCESS,L"The migration backup could not be restored.");}
        lastAttempt.valid=false;ok=true;
    }catch(const Failure&e){error=e.detail;}catch(...){error=L"Gesture rollback remains pending; its original backup is preserved.";}
    ReleaseMutex(mutex);CloseHandle(mutex);NotifySettingsChanged();return ok;
}
int RunGestureProbe(const std::wstring& reportPath){
    std::wstring detail;bool profile=VerifyGestureProfile(detail),readable=false,legacy=false,privateActive=false;
    std::array<DWORD,9> effective{};std::array<int,9> ids{};
    if(profile)try{Cache cache;effective=cache.read();ids=cache.ids;readable=true;legacy=AnyLegacy(effective);privateActive=AnyPrivate(effective);}catch(...){}
    std::ostringstream text;text<<"{\"profileSupported\":"<<(profile?"true":"false")<<",\"liveCacheReadable\":"<<(readable?"true":"false")<<",\"precisionTouchpad\":"<<(PrecisionTouchpad()?"true":"false")<<",\"privateBindingsActive\":"<<(privateActive?"true":"false")<<",\"legacyBindingsActive\":"<<(legacy?"true":"false")<<",\"physicalGesturesTested\":false,\"readOnly\":true,\"cacheIds\":[";
    for(size_t i=0;i<9;i++)text<<(i?",":"")<<ids[i];text<<"],\"effective\":[";for(size_t i=0;i<9;i++)text<<(i?",":"")<<effective[i];text<<"]}\n";
    std::string json=text.str();HANDLE f=CreateFileW(reportPath.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);if(f==INVALID_HANDLE_VALUE)return 2;DWORD n{};bool ok=WriteFile(f,json.data(),static_cast<DWORD>(json.size()),&n,nullptr)&&n==json.size();CloseHandle(f);return ok?(profile&&readable?0:1):2;
}
}
