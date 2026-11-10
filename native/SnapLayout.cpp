#include "SnapLayout.h"
#include <algorithm>
#include <vector>

namespace snap {
namespace {
bool Valid(std::uint32_t share){return share>0&&share<10000;}
struct Record {DWORD size=sizeof(Record),version=1,share=DefaultShare,reserved=0;};
}
RECT Tile(const RECT& work,bool right,int row,std::uint32_t leftShare){
    if(!Valid(leftShare))leftShare=DefaultShare;
    const auto width=static_cast<std::int64_t>(work.right)-work.left;
    const LONG boundary=static_cast<LONG>(work.left+width*leftShare/10000);
    RECT result=work;
    if(right)result.left=boundary;else result.right=boundary;
    const LONG middle=static_cast<LONG>(work.top+(static_cast<std::int64_t>(work.bottom)-work.top)/2);
    if(row<0)result.bottom=middle;else if(row>0)result.top=middle;
    return result;
}
std::optional<std::uint32_t> SharedSplit(const RECT& left,const RECT& right,const RECT& work){
    const auto width=static_cast<std::int64_t>(work.right)-work.left;
    if(width<=1||work.bottom<=work.top||left.left!=work.left||right.right!=work.right||
       left.top!=work.top||right.top!=work.top||left.bottom!=work.bottom||right.bottom!=work.bottom||
       left.right!=right.left||left.right<=work.left||left.right>=work.right)return {};
    auto share=static_cast<std::uint32_t>(((static_cast<std::int64_t>(left.right)-work.left)*10000+width-1)/width);
    return Valid(share)?std::optional<std::uint32_t>(share):std::nullopt;
}
std::wstring DisplayIdentity(HMONITOR monitor){
    MONITORINFOEXW info{};info.cbSize=sizeof(info);
    if(!monitor||!GetMonitorInfoW(monitor,&info))return {};
    auto interfaceName=[&]()->std::wstring{
        DISPLAY_DEVICEW device{};device.cb=sizeof(device);
        if(EnumDisplayDevicesW(info.szDevice,0,&device,EDD_GET_DEVICE_INTERFACE_NAME)&&device.DeviceID[0])return device.DeviceID;
        return {};
    };
    for(int attempt=0;attempt<3;++attempt){
        UINT32 pathCount{},modeCount{};
        if(GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,&pathCount,&modeCount)!=ERROR_SUCCESS)return interfaceName();
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        LONG status=QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,&pathCount,paths.data(),&modeCount,modes.data(),nullptr);
        if(status==ERROR_INSUFFICIENT_BUFFER)continue;
        if(status!=ERROR_SUCCESS)return interfaceName();
        std::wstring identity;
        for(UINT32 i=0;i<pathCount;++i){
            const auto& path=paths[i];
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header={DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,sizeof(source),path.sourceInfo.adapterId,path.sourceInfo.id};
            if(DisplayConfigGetDeviceInfo(&source.header)!=ERROR_SUCCESS||_wcsicmp(source.viewGdiDeviceName,info.szDevice))continue;
            DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
            target.header={DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME,sizeof(target),path.targetInfo.adapterId,path.targetInfo.id};
            if(DisplayConfigGetDeviceInfo(&target.header)!=ERROR_SUCCESS||!target.monitorDevicePath[0])return {};
            if(!identity.empty()&&_wcsicmp(identity.c_str(),target.monitorDevicePath))return {};
            identity=target.monitorDevicePath;
        }
        return identity.empty()?interfaceName():identity;
    }
    return {};
}
std::uint32_t Preferences::Load(const std::wstring& display) const{
    if(display.empty())return DefaultShare;
    Record record{};DWORD bytes=sizeof(record);
    if(RegGetValueW(HKEY_CURRENT_USER,key_.c_str(),display.c_str(),RRF_RT_REG_BINARY,nullptr,&record,&bytes)!=ERROR_SUCCESS||
       bytes!=sizeof(record)||record.size!=sizeof(record)||record.version!=1||record.reserved||!Valid(record.share))return DefaultShare;
    return record.share;
}
bool Preferences::Save(const std::wstring& display,std::uint32_t leftShare) const{
    if(display.empty()||!Valid(leftShare))return false;
    Record before{};DWORD bytes=sizeof(before);
    LONG status=RegGetValueW(HKEY_CURRENT_USER,key_.c_str(),display.c_str(),RRF_RT_REG_BINARY,nullptr,&before,&bytes);
    if(status==ERROR_MORE_DATA)return false;
    if(status==ERROR_SUCCESS&&before.version>1)return false;
    HKEY key{};
    if(RegCreateKeyExW(HKEY_CURRENT_USER,key_.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS)return false;
    Record record;record.share=leftShare;
    status=RegSetValueExW(key,display.c_str(),0,REG_BINARY,reinterpret_cast<const BYTE*>(&record),sizeof(record));
    RegCloseKey(key);return status==ERROR_SUCCESS;
}
}
