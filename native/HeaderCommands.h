#pragma once
#include <windows.h>
#include <oleacc.h>
#include <wrl/client.h>
#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>
namespace header {
using Microsoft::WRL::ComPtr;
bool Action(long role) {
    return role==ROLE_SYSTEM_PUSHBUTTON||role==ROLE_SYSTEM_CHECKBUTTON||role==ROLE_SYSTEM_RADIOBUTTON||
        role==ROLE_SYSTEM_BUTTONDROPDOWN||role==ROLE_SYSTEM_BUTTONMENU||role==ROLE_SYSTEM_BUTTONDROPDOWNGRID||
        role==ROLE_SYSTEM_COMBOBOX||role==ROLE_SYSTEM_PAGETAB||role==ROLE_SYSTEM_LINK||role==ROLE_SYSTEM_MENUITEM;
}
struct Command { std::wstring name; long role{},state{}; RECT rect{}; bool hasState{},hasRect{}; };
struct Inspection {
    bool collectNames=true;
    // Diagnostic opt-in: include semantic labels and containers as well as actions.
    bool collectVisualContent=false;
    std::vector<Command> commands;
    std::vector<ComPtr<IUnknown>> identities;
    std::unordered_set<IUnknown*> visited;
    unsigned nodes{},errors{}; bool truncated{};
    void Read(IAccessible* object,VARIANT child) {
        ++nodes;
        VARIANT role{},state{};
        HRESULT roleResult=object->get_accRole(child,&role);
        if(SUCCEEDED(roleResult)&&role.vt==VT_I4&&(collectVisualContent||Action(role.lVal))) {
            Command c; c.role=role.lVal;
            BSTR name{};
            if(collectNames&&SUCCEEDED(object->get_accName(child,&name))&&name)c.name.assign(name,SysStringLen(name));
            SysFreeString(name);
            c.hasState=SUCCEEDED(object->get_accState(child,&state))&&state.vt==VT_I4;
            if(c.hasState)c.state=state.lVal;else ++errors;
            long x{},y{},width{},height{};
            c.hasRect=SUCCEEDED(object->accLocation(&x,&y,&width,&height,child))&&width>0&&height>0;
            if(c.hasRect)c.rect={x,y,x+width,y+height};
            else if(!c.hasState||!(c.state&(STATE_SYSTEM_INVISIBLE|STATE_SYSTEM_OFFSCREEN)))++errors;
            commands.push_back(std::move(c));
        } else if(FAILED(roleResult))++errors;
        VariantClear(&role);VariantClear(&state);
    }
    void Walk(IAccessible* object,unsigned depth=0) {
        if(depth>24||nodes>=8192){truncated=true;return;}
        ComPtr<IUnknown> identity;
        if(FAILED(object->QueryInterface(IID_PPV_ARGS(&identity)))){++errors;return;}
        if(!visited.insert(identity.Get()).second)return;
        identities.push_back(identity);
        VARIANT self{};self.vt=VT_I4;self.lVal=CHILDID_SELF;Read(object,self);
        long count{};
        if(FAILED(object->get_accChildCount(&count))||count<0){++errors;return;}
        if(!count)return;
        long requested=std::min<long>(count,8192-static_cast<long>(nodes));
        if(requested<count)truncated=true;
        std::vector<VARIANT> children(requested);long obtained{};
        HRESULT result=AccessibleChildren(object,0,requested,children.data(),&obtained);
        if(FAILED(result)||obtained!=requested)++errors;
        for(long i=0;i<obtained&&i<requested;++i) {
            auto& child=children[i];
            if(nodes>=8192){truncated=true;break;}
            if(child.vt==VT_DISPATCH&&child.pdispVal) {
                ComPtr<IAccessible> nested;
                if(SUCCEEDED(child.pdispVal->QueryInterface(IID_PPV_ARGS(&nested))))Walk(nested.Get(),depth+1);
                else ++errors;
            } else if(child.vt==VT_I4) Read(object,child);
            else ++errors;
        }
        for(auto& child:children)VariantClear(&child);
    }
};
}
