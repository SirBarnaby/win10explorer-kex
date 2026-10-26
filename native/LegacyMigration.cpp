#include "LegacyMigration.h"
#include <xmllite.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <memory>
#include <algorithm>
#include <cerrno>

namespace migration {
namespace {
constexpr wchar_t TouchpadKey[]=L"Software\\Microsoft\\Windows\\CurrentVersion\\PrecisionTouchPad";
constexpr const wchar_t* Names[]={L"ThreeFingerSlideEnabled",L"ThreeFingerLeft",L"ThreeFingerUp",L"ThreeFingerRight",L"ThreeFingerDown",L"ThreeFingerLeftKeyParams",L"ThreeFingerUpKeyParams",L"ThreeFingerRightKeyParams",L"ThreeFingerDownKeyParams"};
struct Node { std::wstring kind,name,text;std::vector<std::unique_ptr<Node>> children; };
const Node* Child(const Node* n,const wchar_t* kind){if(n)for(auto& c:n->children)if(c->kind==kind)return c.get();return nullptr;}
const Node* Field(const Node* n,const wchar_t* name) {
    n=Child(n,L"DCT");if(!n)return nullptr;
    for(auto& e:n->children)if(e->kind==L"En") {
        const Node* key=nullptr;const Node* value=nullptr;
        for(auto& c:e->children){if(c->name==L"Key")key=c.get();if(c->name==L"Value")value=c.get();}
        if(key&&key->text==name)return value;
    }return nullptr;
}
bool Parse(IStream* stream,std::unique_ptr<Node>& root) {
    IXmlReader* reader{};if(FAILED(CreateXmlReader(__uuidof(IXmlReader),reinterpret_cast<void**>(&reader),nullptr)))return false;
    reader->SetProperty(XmlReaderProperty_DtdProcessing,DtdProcessing_Prohibit);
    HRESULT hr=reader->SetInput(stream);std::vector<Node*> stack;size_t count=0;XmlNodeType type{};
    while(SUCCEEDED(hr)&&(hr=reader->Read(&type))==S_OK) {
        if(type==XmlNodeType_Element) {
            if(++count>32000||stack.size()>=32){hr=E_FAIL;break;}
            const wchar_t* text{};UINT length{};reader->GetLocalName(&text,&length);
            auto n=std::make_unique<Node>();n->kind.assign(text,length);
            if(reader->MoveToAttributeByName(L"N",nullptr)==S_OK){reader->GetValue(&text,&length);n->name.assign(text,length);reader->MoveToElement();}
            Node* raw=n.get();if(stack.empty()){if(root){hr=E_FAIL;break;}root=std::move(n);}else stack.back()->children.push_back(std::move(n));
            if(!reader->IsEmptyElement())stack.push_back(raw);
        } else if(type==XmlNodeType_EndElement) {if(stack.empty()){hr=E_FAIL;break;}stack.pop_back();}
        else if(type==XmlNodeType_Text||type==XmlNodeType_CDATA||type==XmlNodeType_Whitespace) {
            if(stack.empty())continue;const wchar_t* text{};UINT length{};reader->GetValue(&text,&length);
            if(stack.back()->text.size()+length>4096){hr=E_FAIL;break;}stack.back()->text.append(text,length);
        }
    }
    reader->Release();return hr==S_FALSE&&root&&stack.empty();
}
bool Snapshot(const Node* n,Value& v) {
    const Node* exists=Field(n,L"Exists");const Node* kind=Field(n,L"Kind");const Node* number=Field(n,L"Value");
    if(!exists||exists->kind!=L"B")return false;
    if(exists->text==L"false"){v={};return true;}
    if(exists->text!=L"true"||!kind||kind->text!=L"DWord"||!number||(number->kind!=L"I32"&&number->kind!=L"U32"&&number->kind!=L"I64"))return false;
    wchar_t* end{};errno=0;long long parsed=wcstoll(number->text.c_str(),&end,10);
    if(errno||!end||*end||end==number->text.c_str()||parsed<INT32_MIN||parsed>UINT32_MAX)return false;
    v={1,static_cast<DWORD>(parsed)};return true;
}
bool Current(size_t i,Value& v) {
    DWORD type{},size=sizeof(DWORD),value{};LONG r=RegGetValueW(HKEY_CURRENT_USER,TouchpadKey,Names[i],RRF_RT_REG_DWORD,&type,&value,&size);
    if(r==ERROR_FILE_NOT_FOUND||r==ERROR_PATH_NOT_FOUND){v={};return true;}
    if(r!=ERROR_SUCCESS||size!=sizeof(DWORD))return false;v={1,value};return true;
}
bool Decode(const Node* root,Legacy& out,const std::array<Value,9>* current=nullptr) {
    if(!root||root->kind!=L"Objs")return false;const Node* object=Child(root,L"Obj");
    const Node* version=Field(object,L"Version"),*status=Field(object,L"Status"),*owned=Field(object,L"QTOwned"),*changes=Child(Field(object,L"Changes"),L"LST");
    if(!version||version->text!=L"1.0.0"||!status||status->text!=L"Installed"||!owned||owned->kind!=L"B"||!changes)return false;
    out.found=true;out.qtOwned=owned->text==L"true";
    if(current)out.baseline=*current;
    else for(size_t i=0;i<out.baseline.size();++i)if(!Current(i,out.baseline[i]))return false;
    std::array<bool,9> seen{};
    for(auto& change:changes->children) {
        const Node* path=Field(change.get(),L"Path"),*name=Field(change.get(),L"Name");
        if(!path||!name||_wcsicmp(path->text.c_str(),TouchpadKey))continue;
        auto found=std::find_if(std::begin(Names),std::end(Names),[&](const wchar_t* n){return name->text==n;});
        if(found==std::end(Names))continue;size_t i=found-std::begin(Names);if(seen[i])return false;seen[i]=true;
        Value before{},after{};if(!Snapshot(Field(change.get(),L"Before"),before)||!Snapshot(Field(change.get(),L"After"),after))return false;
        // Import the old original only while this value still belongs to v1.
        if(out.baseline[i].present==after.present&&(!after.present||out.baseline[i].value==after.value))out.baseline[i]=before;
    }return true;
}
std::wstring StatePath(){PWSTR local{};if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local)))return {};std::wstring path=std::wstring(local)+L"\\Win10NativePatch\\installation.clixml";CoTaskMemFree(local);return path;}
}
bool ReadLegacy(const std::wstring& path,Legacy& result,std::wstring& error) {
    result={};WIN32_FILE_ATTRIBUTE_DATA data{};if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&data)) {
        if(GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND)return true;
        error=L"The previous installation journal could not be read.";return false;
    }
    if(data.nFileSizeHigh||data.nFileSizeLow>4*1024*1024){error=L"The previous installation journal exceeds the supported size.";return false;}
    IStream* stream{};HRESULT hr=SHCreateStreamOnFileEx(path.c_str(),STGM_READ|STGM_SHARE_DENY_WRITE,FILE_ATTRIBUTE_NORMAL,FALSE,nullptr,&stream);
    std::unique_ptr<Node> root;bool ok=SUCCEEDED(hr)&&Parse(stream,root)&&Decode(root.get(),result);if(stream)stream->Release();
    if(!ok)error=L"The previous installation journal is not a supported, complete v1 record. It has been left unchanged.";return ok;
}
bool PrepareLegacyImport(std::wstring& error) {
    DWORD size{};LONG r=RegGetValueW(HKEY_CURRENT_USER,patch::SettingsKey,L"GestureCycle",RRF_RT_REG_BINARY,nullptr,nullptr,&size);
    if(r==ERROR_SUCCESS)return true;
    Legacy legacy;if(!ReadLegacy(StatePath(),legacy,error))return false;if(!legacy.found)return true;
    // Preserve the original journal before any migration. CopyFile is fail-if-exists.
    std::wstring source=StatePath();std::wstring backup=source+L".before-native-v2";
    if(!CopyFileW(source.c_str(),backup.c_str(),TRUE)&&GetLastError()!=ERROR_FILE_EXISTS){error=L"The v1 recovery journal could not be backed up.";return false;}
    HKEY key{};r=RegCreateKeyExW(HKEY_CURRENT_USER,patch::SettingsKey,0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr);
    if(r==ERROR_SUCCESS){r=RegSetValueExW(key,L"LegacyGestureBaseline",0,REG_BINARY,reinterpret_cast<const BYTE*>(legacy.baseline.data()),sizeof(legacy.baseline));if(r==ERROR_SUCCESS)r=RegFlushKey(key);RegCloseKey(key);}
    if(r!=ERROR_SUCCESS){error=L"The previous gesture baseline could not be saved.";return false;}return true;
}
int SelfTest(const std::wstring& report) {
    unsigned passed=0,total=0;auto check=[&](bool ok){++total;if(ok)++passed;};
    const char* xml="<Objs><Obj><DCT><En><S N='Key'>Version</S><S N='Value'>1.0.0</S></En><En><S N='Key'>Status</S><S N='Value'>Installed</S></En><En><S N='Key'>QTOwned</S><B N='Value'>true</B></En><En><S N='Key'>Changes</S><Obj N='Value'><LST/></Obj></En></DCT></Obj></Objs>";
    auto parse=[&](const char* data,std::unique_ptr<Node>& root){IStream* s=SHCreateMemStream(reinterpret_cast<const BYTE*>(data),static_cast<UINT>(strlen(data)));if(!s)return false;bool ok=Parse(s,root);s->Release();return ok;};
    std::unique_ptr<Node> root;Legacy out;check(parse(xml,root));check(Decode(root.get(),out)&&out.found&&out.qtOwned);
    root.reset();check(!parse("<!DOCTYPE Objs [<!ENTITY e SYSTEM 'file:///C:/Windows/win.ini'>]><Objs>&e;</Objs>",root));
    root.reset();check(!parse("<Objs><Obj>",root));
    root.reset();check(parse("<Objs><Obj/></Objs>",root)&&!Decode(root.get(),out));
    std::string fixture=xml;size_t marker=fixture.find("<LST/>");
    fixture.replace(marker,6,"<LST><Obj><DCT><En><S N='Key'>Path</S><S N='Value'>Software\\Microsoft\\Windows\\CurrentVersion\\PrecisionTouchPad</S></En><En><S N='Key'>Name</S><S N='Value'>ThreeFingerLeft</S></En><En><S N='Key'>Before</S><Obj N='Value'><DCT><En><S N='Key'>Exists</S><B N='Value'>true</B></En><En><S N='Key'>Kind</S><S N='Value'>DWord</S></En><En><S N='Key'>Value</S><I32 N='Value'>123</I32></En></DCT></Obj></En><En><S N='Key'>After</S><Obj N='Value'><DCT><En><S N='Key'>Exists</S><B N='Value'>true</B></En><En><S N='Key'>Kind</S><S N='Value'>DWord</S></En><En><S N='Key'>Value</S><I32 N='Value'>65535</I32></En></DCT></Obj></En></DCT></Obj></LST>");
    root.reset();std::array<Value,9> simulated{};simulated[1]={1,65535};
    check(parse(fixture.c_str(),root)&&Decode(root.get(),out,&simulated)&&out.baseline[1].present&&out.baseline[1].value==123);
    simulated[1]={1,7};check(Decode(root.get(),out,&simulated)&&out.baseline[1].value==7);
    Legacy live;std::wstring error;bool liveFound=GetFileAttributesW(StatePath().c_str())!=INVALID_FILE_ATTRIBUTES;
    if(liveFound)check(ReadLegacy(StatePath(),live,error)&&live.found);
    std::string json="{\"success\":"+std::string(passed==total?"true":"false")+",\"passed\":"+std::to_string(passed)+",\"total\":"+std::to_string(total)+",\"legacyJournalFound\":"+(liveFound?"true":"false")+",\"legacyJournalReadOnly\":true,\"liveSettingsModified\":false}\n";
    HANDLE file=CreateFileW(report.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);if(file==INVALID_HANDLE_VALUE)return 2;DWORD written{};bool ok=WriteFile(file,json.data(),static_cast<DWORD>(json.size()),&written,nullptr)&&written==json.size();CloseHandle(file);return ok&&passed==total?0:1;
}
}
