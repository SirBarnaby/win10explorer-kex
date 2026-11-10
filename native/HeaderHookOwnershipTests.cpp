#include <windows.h>
#include <cstring>
#include <iostream>
#include "vendor/minhook/include/MinHook.h"
namespace {
using Function=int(WINAPI*)(int);
Function original{};
__declspec(noinline) int WINAPI OwnedTarget(int value){return value*3+7;}
int WINAPI Detour(int value){return original(value)+20;}
bool Patch(void* address,const BYTE* bytes,size_t count){
    DWORD protect{};if(!VirtualProtect(address,count,PAGE_EXECUTE_READWRITE,&protect))return false;
    memcpy(address,bytes,count);DWORD ignored{};VirtualProtect(address,count,protect,&ignored);
    FlushInstructionCache(GetCurrentProcess(),address,count);return true;
}
}
int main(){
    auto* target=reinterpret_cast<void*>(OwnedTarget);
    BYTE pristine[16]{},installed[16]{},other[16]{};memcpy(pristine,target,sizeof(pristine));
    bool good=MH_Initialize()==MH_OK&&MH_CreateHook(target,reinterpret_cast<void*>(Detour),reinterpret_cast<void**>(&original))==MH_OK;
    good=good&&MH_EnableHook(target)==MH_OK;memcpy(installed,target,sizeof(installed));
    Function volatile invoke=OwnedTarget;
    const bool works=good&&invoke(5)==42;
    memcpy(other,installed,sizeof(other));other[1]^=1; // A later writer changes its detour destination.
    const bool changed=Patch(target,other,sizeof(other));
    const bool refused=changed&&MH_DisableHook(target)==MH_ERROR_UNSUPPORTED_FUNCTION&&!memcmp(target,other,sizeof(other));
    Patch(target,installed,sizeof(installed));
    const bool restored=MH_DisableHook(target)==MH_OK&&!memcmp(target,pristine,sizeof(pristine))&&invoke(5)==22;
    // An extension present before re-enable must also retain its bytes.
    memcpy(other,pristine,sizeof(other));other[0]^=1;Patch(target,other,sizeof(other));
    const bool enableRefused=MH_EnableHook(target)==MH_ERROR_UNSUPPORTED_FUNCTION&&!memcmp(target,other,sizeof(other));
    Patch(target,pristine,sizeof(pristine));
    std::cout<<"ownedHookWorks="<<works<<" laterWriterPreserved="<<refused<<" exactRestore="<<restored<<" priorWriterPreserved="<<enableRefused<<'\n';
    return works&&refused&&restored&&enableRefused?0:1;
}
