#include <windows.h>
#include <string>
#include <cstring>
#include <iostream>
int wmain(int argc,wchar_t** argv) {
    if(argc!=3){std::wcerr<<L"Usage: NativePatchModelTests <ExplorerTabs.dll> <report.json>\n";return 2;}
    HMODULE dll=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!dll){std::wcerr<<L"Cannot load test DLL: "<<GetLastError()<<L"\n";return 2;}
    auto address=GetProcAddress(dll,"RunModelTests");using Test=HRESULT(WINAPI*)(const wchar_t*);Test run{};static_assert(sizeof(run)==sizeof(address));std::memcpy(&run,&address,sizeof(run));
    HRESULT result=run?run(argv[2]):E_NOINTERFACE;FreeLibrary(dll);
    std::wcout<<L"Explorer model test HRESULT: 0x"<<std::hex<<static_cast<unsigned long>(result)<<L"\n";
    return result==S_OK?0:1;
}
