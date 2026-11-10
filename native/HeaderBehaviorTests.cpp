// Runs only in the disposable guest, through real public input/accessibility.
#define wmain ExistingPhysicalSuite
#define Folder HeaderFolderPath
#include "HeaderInteractionTests.cpp"
#undef wmain
#undef Folder
#include <shlobj.h>
#include <uiautomation.h>
#include "MvpHostProtocol.h"
namespace {
std::wstring CurrentUrl(HWND f){return HeaderFolderPath(f);}
ComPtr<IAccessible> Access(HWND frame) {ComPtr<IAccessible> a;auto h=Strip(frame);if(h)AccessibleObjectFromWindow(h,OBJID_CLIENT,IID_PPV_ARGS(&a));return a;}
VARIANT Child(long n){VARIANT v{};v.vt=VT_I4;v.lVal=n;return v;}
void MouseButton(DWORD flag){INPUT i{};i.type=INPUT_MOUSE;i.mi.dwFlags=flag;SendInput(1,&i,sizeof(i));Pump(180);}
std::vector<Target> Tabs(HWND frame){auto all=Targets(frame);auto a=Access(frame);std::vector<Target> out;for(auto& t:all){VARIANT role{};if(a&&SUCCEEDED(a->get_accRole(Child(t.child),&role))&&role.vt==VT_I4&&role.lVal==ROLE_SYSTEM_PAGETAB)out.push_back(t);}return out;}
size_t TabCount(HWND frame){return Tabs(frame).size();}
Target Named(HWND frame,const std::wstring& name){for(auto& t:Targets(frame))if(t.name==name)return t;return {};}
POINT Center(RECT r){return {(r.left+r.right)/2,(r.top+r.bottom)/2};}
void Escape(){Key(VK_ESCAPE);Key(VK_ESCAPE,true);Pump(200);}
void Point(RECT r){auto p=Center(r);SetCursorPos(p.x,p.y);Pump(50);}
void DragPointer(RECT from,RECT to,bool cancel=false){
 auto a=Center(from),b=Center(to);SetCursorPos(a.x,a.y);MouseButton(MOUSEEVENTF_LEFTDOWN);
 for(int i=1;i<=20;++i){SetCursorPos(a.x+(b.x-a.x)*i/20,a.y+(b.y-a.y)*i/20);Pump(35);}
 if(cancel)Escape();MouseButton(MOUSEEVENTF_LEFTUP);Pump(700);
}
bool PopupClick(const std::wstring& label){
 HWND menu=FindWindowW(L"#32768",nullptr);ComPtr<IAccessible> accessible;
 if(!menu||FAILED(AccessibleObjectFromWindow(menu,OBJID_CLIENT,IID_PPV_ARGS(&accessible))))return false;
 long count{};accessible->get_accChildCount(&count);
 for(long i=1;i<=count;++i){BSTR name{};accessible->get_accName(Child(i),&name);std::wstring text=name?name:L"";SysFreeString(name);if(text.find(label)==std::wstring::npos)continue;long x{},y{},w{},h{};accessible->accLocation(&x,&y,&w,&h,Child(i));if(w>0&&h>0){PhysicalClick({x+w/2,y+h/2});return true;}}
 return false;
}
void ReopenPhysical(){Key(VK_CONTROL);Key(VK_SHIFT);Key('T');Key('T',true);Key(VK_SHIFT,true);Key(VK_CONTROL,true);Pump(700);}
std::vector<HWND> ExplorerWindows(){std::vector<HWND> result;ComPtr<IShellWindows> windows;if(FAILED(CoCreateInstance(CLSID_ShellWindows,nullptr,CLSCTX_ALL,IID_PPV_ARGS(&windows))))return result;long count{};windows->get_Count(&count);for(long i=0;i<count;++i){ComPtr<IDispatch> dispatch;ComPtr<IWebBrowser2> browser;if(SUCCEEDED(windows->Item(Child(i),&dispatch))&&SUCCEEDED(dispatch.As(&browser))){SHANDLE_PTR handle{};browser->get_HWND(&handle);result.push_back(reinterpret_cast<HWND>(handle));}}return result;}
void Type(const std::wstring& text){for(wchar_t c:text){INPUT input[2]{};for(auto& item:input){item.type=INPUT_KEYBOARD;item.ki.wScan=c;item.ki.dwFlags=KEYEVENTF_UNICODE;}input[1].ki.dwFlags|=KEYEVENTF_KEYUP;SendInput(2,input,sizeof(INPUT));}Pump(80);}
std::wstring FocusText(HWND frame){GUITHREADINFO info{sizeof(info)};GetGUIThreadInfo(GetWindowThreadProcessId(frame,nullptr),&info);wchar_t text[4096]{};SendMessageTimeoutW(info.hwndFocus,WM_GETTEXT,4096,reinterpret_cast<LPARAM>(text),SMTO_ABORTIFHUNG,1000,nullptr);if(*text)return text;ComPtr<IUIAutomation> automation;ComPtr<IUIAutomationElement> element;ComPtr<IUIAutomationValuePattern> pattern;if(SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&automation)))&&SUCCEEDED(automation->GetFocusedElement(&element))&&SUCCEEDED(element->GetCurrentPatternAs(UIA_ValuePatternId,IID_PPV_ARGS(&pattern)))){BSTR value{};pattern->get_CurrentValue(&value);std::wstring result=value?value:L"";SysFreeString(value);return result;}return L"";}
}
int wmain(int argc,wchar_t** argv){
 if(argc!=4||!Guest())return 2;
 SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
 CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);HWND frame=reinterpret_cast<HWND>(wcstoull(argv[1],nullptr,10));
 MouseButton(MOUSEEVENTF_LEFTUP|MOUSEEVENTF_RIGHTUP|MOUSEEVENTF_MIDDLEUP);
 for(int modifier:{VK_CONTROL,VK_SHIFT,VK_MENU,VK_LCONTROL,VK_RCONTROL,VK_LSHIFT,VK_RSHIFT,VK_LMENU,VK_RMENU,VK_LWIN,VK_RWIN})Key(static_cast<WORD>(modifier),true);Escape();
 std::wstring directory=argv[2],mode=argv[3];std::ofstream out(directory+L"\\behavior-"+mode+L".json");
 wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);std::wstring base=exe;base.resize(base.find_last_of(L'\\'));
 HMODULE lab=LoadLibraryW((base+L"\\HeaderLabHook.dll").c_str());
 HHOOK hook=SetWindowsHookExW(WH_GETMESSAGE,reinterpret_cast<HOOKPROC>(GetProcAddress(lab,"HeaderLabHook")),lab,GetWindowThreadProcessId(frame,nullptr));
 if(mode==L"verify"){
  bool good=true;out<<"{\"tabs\":[";
  const auto tabs=Tabs(frame);good=tabs.size()==3;
  const wchar_t* names[]={L"Alpha",L"Beta",L"Gamma"};const wchar_t* previous[]={nullptr,L"Alpha",L"Beta"};
  for(size_t i=0;i<tabs.size();++i){
   if(i)out<<',';auto a=Access(frame);const bool ordered=i<3&&tabs[i].name==names[i];if(a)a->accDoDefaultAction(Child(tabs[i].child));Pump(600);
   const auto actual=CurrentUrl(frame);const bool folder=i<3&&actual==L"file:///C:/NativeLabFolders/"+std::wstring(names[i]);bool history=true;
   if(i<3&&previous[i]){SendMessageW(frame,WM_APPCOMMAND,reinterpret_cast<WPARAM>(frame),MAKELPARAM(0,APPCOMMAND_BROWSER_BACKWARD));Pump(600);history=CurrentUrl(frame)==L"file:///C:/NativeLabFolders/"+std::wstring(previous[i]);SendMessageW(frame,WM_APPCOMMAND,reinterpret_cast<WPARAM>(frame),MAKELPARAM(0,APPCOMMAND_BROWSER_FORWARD));Pump(500);}
   good=good&&ordered&&folder&&history;out<<"{\"name\":"<<Json(tabs[i].name)<<",\"actualFolder\":"<<Json(actual)<<",\"order\":"<<(ordered?"true":"false")<<",\"folder\":"<<(folder?"true":"false")<<",\"history\":"<<(history?"true":"false")<<'}';
  }
  out<<"],\"success\":"<<(good?"true":"false")<<'}';UnhookWindowsHookEx(hook);FreeLibrary(lab);CoUninitialize();return good?0:1;
 }
 UINT apply=RegisterWindowMessageW(L"Win10NativePatch.HeaderLab.Apply");PostMessageW(frame,apply,64,0);Pump(1200);
 ShowWindow(frame,SW_RESTORE);SetWindowPos(frame,HWND_TOPMOST,100,70,1050,650,SWP_SHOWWINDOW);SetForegroundWindow(frame);Pump(300);
 Navigate(frame,L"C:\\NativeLabFolders\\Alpha");PhysicalClick({550,350});Chord('T');Navigate(frame,L"C:\\NativeLabFolders\\Beta");Chord('T');Navigate(frame,L"C:\\NativeLabFolders\\Gamma");
 GUITHREADINFO initialFocus{sizeof(initialFocus)};GetGUIThreadInfo(GetWindowThreadProcessId(frame,nullptr),&initialFocus);
 bool good=TabCount(frame)==3;out<<"{\"bootstrap\":"<<(good?"true":"false")<<",\"foreground\":"<<reinterpret_cast<ULONG_PTR>(GetForegroundWindow())<<",\"frame\":"<<reinterpret_cast<ULONG_PTR>(frame)<<",\"focusClass\":"<<Json(Class(initialFocus.hwndFocus))<<",\"attachState\":"<<reinterpret_cast<ULONG_PTR>(GetPropW(frame,L"Win10NativePatch.HeaderLab.State"))<<",\"rejected\":"<<reinterpret_cast<ULONG_PTR>(GetPropW(frame,L"Win10NativePatch.TabsRejected.v2"))<<",\"state\":";State(out,frame);out<<",\"checks\":[";bool first=true;
 auto check=[&](const char* name,bool pass){if(!first)out<<',';first=false;out<<"{\"name\":\""<<name<<"\",\"passed\":"<<(pass?"true":"false")<<'}';out.flush();good=good&&pass;};
 if(mode==L"prepare") {out<<"],\"success\":"<<(good?"true":"false")<<'}';UnhookWindowsHookEx(hook);FreeLibrary(lab);CoUninitialize();return good?0:1;}
 if(mode==L"drag"){
  DragPointer(Named(frame,L"Alpha").rect,Named(frame,L"Gamma").rect);
  auto reordered=Tabs(frame);check("physical-reorder-keeps-tab-identities",reordered.size()==3&&reordered[0].name==L"Beta"&&reordered[1].name==L"Gamma"&&reordered[2].name==L"Alpha");
  DragPointer(Named(frame,L"Beta").rect,Named(frame,L"Gamma").rect,true);
  auto cancelled=Tabs(frame);check("escape-cancels-physical-drag",cancelled.size()==3&&cancelled[0].name==L"Beta"&&cancelled[1].name==L"Gamma"&&cancelled[2].name==L"Alpha");
  auto before=ExplorerWindows();ShellExecuteW(nullptr,L"open",L"C:\\Windows\\explorer.exe",L"/n,C:\\NativeLabFolders\\Gamma",nullptr,SW_SHOWNORMAL);HWND target{};
  for(unsigned i=0;i<20&&!target;++i){Pump(100);for(HWND candidate:ExplorerWindows())if(candidate!=frame&&std::find(before.begin(),before.end(),candidate)==before.end())target=candidate;}
  HHOOK targetHook=target?SetWindowsHookExW(WH_GETMESSAGE,reinterpret_cast<HOOKPROC>(GetProcAddress(lab,"HeaderLabHook")),lab,GetWindowThreadProcessId(target,nullptr)):nullptr;
  if(target)PostMessageW(target,apply,64,0);Pump(800);
  SetWindowPos(frame,HWND_TOPMOST,10,80,600,650,SWP_SHOWWINDOW);SetWindowPos(target,HWND_TOPMOST,650,80,600,650,SWP_SHOWWINDOW);Pump(500);
  auto access=Access(frame);auto alpha=Named(frame,L"Alpha");if(access&&alpha.child)access->accDoDefaultAction(Child(alpha.child));Pump(500);alpha=Named(frame,L"Alpha");auto destination=Named(target,L"New tab");
  if(target&&alpha.child&&destination.child)DragPointer(alpha.rect,destination.rect);
  check("physical-cross-window-drop-moves-original",target&&TabCount(frame)==2&&!Named(frame,L"Alpha").child&&TabCount(target)==2&&Named(target,L"Alpha").child&&CurrentUrl(target)==L"file:///C:/NativeLabFolders/Alpha");
  out<<"],\"source\":";State(out,frame);out<<",\"target\":";State(out,target);out<<",\"success\":"<<(good?"true":"false")<<'}';if(target)PostMessageW(target,WM_CLOSE,0,0);Pump(300);if(targetHook)UnhookWindowsHookEx(targetHook);PostMessageW(frame,apply,0,0);Pump(300);UnhookWindowsHookEx(hook);FreeLibrary(lab);CoUninitialize();return good?0:1;
 }
 if(mode==L"nested-save"){
  DWORD pid{};GetWindowThreadProcessId(frame,&pid);HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,pid);PostMessageW(frame,apply,85,0);Pump(100);DWORD_PTR result{};
  SendMessageTimeoutW(frame,patch::mvp::DetachMessage(),0,0,SMTO_ABORTIFHUNG,3000,&result);Pump(500);
  check("nested-destroy-during-session-save",!IsWindow(frame)&&process&&WaitForSingleObject(process,0)==WAIT_TIMEOUT);if(process)CloseHandle(process);
  out<<"],\"success\":"<<(good?"true":"false")<<'}';UnhookWindowsHookEx(hook);FreeLibrary(lab);CoUninitialize();return good?0:1;
 }
 if(mode==L"menu"||mode==L"edit"||mode==L"detach"||mode==L"detach-stale"||mode==L"lifetime") {
  if(mode==L"menu") {
   auto beta=Named(frame,L"Beta");Point(beta.rect);MouseButton(MOUSEEVENTF_RIGHTDOWN);MouseButton(MOUSEEVENTF_RIGHTUP);const bool popup=MenuPresent();auto a=Access(frame);const auto alpha=Named(frame,L"Close Alpha");HRESULT removed=popup?a->accDoDefaultAction(Child(alpha.child)):E_FAIL;Pump(250);const bool picked=PopupClick(L"Duplicate tab");Pump(800);
   check("popup-target-survives-sibling-removal",popup&&picked&&CurrentUrl(frame)==L"file:///C:/NativeLabFolders/Beta"&&TabCount(frame)==3&&Named(frame,L"Gamma").child&&!Named(frame,L"Alpha").child);
   out<<"],\"popup\":"<<(popup?"true":"false")<<",\"removedHr\":"<<removed<<",\"picked\":"<<(picked?"true":"false")<<",\"after\":";State(out,frame);Escape();
  }else if(mode==L"detach"||mode==L"detach-stale") {
   PostMessageW(frame,apply,83,0);Pump(100);Point(Named(frame,L"Beta").rect);MouseButton(MOUSEEVENTF_RIGHTDOWN);MouseButton(MOUSEEVENTF_RIGHTUP);const bool picked=PopupClick(L"Move tab to new window");Pump(300);
   HWND created=static_cast<HWND>(GetPropW(frame,L"HeaderLab.ProductCreatedFrame"));auto before=ExplorerWindows();ShellExecuteW(nullptr,L"open",L"C:\\Windows\\explorer.exe",L"/n,C:\\NativeLabFolders\\Beta",nullptr,SW_SHOWNORMAL);HWND competitor{};for(unsigned i=0;i<20&&!competitor;++i){Pump(100);for(HWND candidate:ExplorerWindows())if(candidate!=created&&candidate!=frame&&std::find(before.begin(),before.end(),candidate)==before.end())competitor=candidate;}
   if(mode==L"detach-stale"){
    auto access=Access(frame);access->accDoDefaultAction(Child(Named(frame,L"Beta").child));Pump(400);Navigate(frame,L"C:\\NativeLabFolders\\Gamma");
    HHOOK targetHook=created?SetWindowsHookExW(WH_GETMESSAGE,reinterpret_cast<HOOKPROC>(GetProcAddress(lab,"HeaderLabHook")),lab,GetWindowThreadProcessId(created,nullptr)):nullptr;
    if(created)PostMessageW(created,apply,64,0);Pump(1400);
    check("changed-source-kept-after-delayed-detach",picked&&created&&TabCount(frame)==3&&CurrentUrl(frame)==L"file:///C:/NativeLabFolders/Gamma"&&CurrentUrl(created)==L"file:///C:/NativeLabFolders/Beta");
    out<<"],\"picked\":"<<(picked?"true":"false")<<",\"created\":"<<reinterpret_cast<ULONG_PTR>(created)<<",\"createdFolder\":"<<Json(CurrentUrl(created))<<",\"source\":";State(out,frame);out<<",\"success\":"<<(good?"true":"false")<<'}';
    PostMessageW(frame,apply,84,0);for(HWND window:{created,competitor})if(window)PostMessageW(window,WM_CLOSE,0,0);Pump(400);if(targetHook)UnhookWindowsHookEx(targetHook);
    PostMessageW(frame,apply,0,0);Pump(300);UnhookWindowsHookEx(hook);FreeLibrary(lab);CoUninitialize();return good?0:1;
   }
   std::vector<HHOOK> extraHooks;auto attach=[&](HWND window){if(!window)return;extraHooks.push_back(SetWindowsHookExW(WH_GETMESSAGE,reinterpret_cast<HOOKPROC>(GetProcAddress(lab,"HeaderLabHook")),lab,GetWindowThreadProcessId(window,nullptr)));PostMessageW(window,apply,64,0);Pump(700);};
   attach(competitor);check("competing-same-folder-frame-not-consumed",created&&competitor&&created!=competitor&&TabCount(frame)==3);
   attach(created);Pump(1200);check("detach-binds-created-window",picked&&TabCount(frame)==2&&!Named(frame,L"Beta").child&&CurrentUrl(created)==L"file:///C:/NativeLabFolders/Beta");
   std::wstring transferredHistory;ULONG_PTR foreground{};bool historyPhysical=false;
   if(created){ShowWindow(created,SW_RESTORE);SetWindowPos(created,HWND_TOPMOST,100,70,1050,650,SWP_SHOWWINDOW);SetForegroundWindow(created);RECT bounds{};GetWindowRect(created,&bounds);PhysicalClick({bounds.left+450,bounds.top+280});foreground=reinterpret_cast<ULONG_PTR>(GetForegroundWindow());historyPhysical=GetForegroundWindow()==created;
    if(historyPhysical){Key(VK_MENU);Key(VK_LEFT);Key(VK_LEFT,true);Key(VK_MENU,true);}else {DWORD_PTR result{};SendMessageTimeoutW(created,WM_APPCOMMAND,reinterpret_cast<WPARAM>(created),MAKELPARAM(0,APPCOMMAND_BROWSER_BACKWARD),SMTO_ABORTIFHUNG,1000,&result);}Pump(900);transferredHistory=CurrentUrl(created);check("created-window-received-original-history",transferredHistory==L"file:///C:/NativeLabFolders/Alpha");}
   PostMessageW(frame,apply,84,0);Pump(100);for(HWND window:{created,competitor})if(window)PostMessageW(window,WM_CLOSE,0,0);Pump(400);for(auto extra:extraHooks)if(extra)UnhookWindowsHookEx(extra);
   out<<"],\"created\":"<<reinterpret_cast<ULONG_PTR>(created)<<",\"competitor\":"<<reinterpret_cast<ULONG_PTR>(competitor)<<",\"historyForeground\":"<<foreground<<",\"historyByPhysicalKeyboard\":"<<(historyPhysical?"true":"false")<<",\"historyFolder\":"<<Json(transferredHistory);
  }else if(mode==L"lifetime") {
   Point(Named(frame,L"Beta").rect);MouseButton(MOUSEEVENTF_RIGHTDOWN);MouseButton(MOUSEEVENTF_RIGHTUP);bool popup=MenuPresent();PostMessageW(frame,WM_CLOSE,0,0);Pump(500);Escape();check("frame-close-during-menu",popup&&!IsWindow(frame)&&!IsWindow(Strip(frame)));out<<']';
  }else {
   PhysicalClick({550,350});const auto folder=CurrentUrl(frame);Key(VK_MENU);Key(VK_SPACE);Key(VK_SPACE,true);Key(VK_MENU,true);Pump(150);Key('T');Key('T',true);Pump(250);GUITHREADINFO focus{sizeof(focus)};GetGUIThreadInfo(GetWindowThreadProcessId(frame,nullptr),&focus);check("native-system-menu-keyboard-entry",focus.hwndFocus==Strip(frame));Escape();
   PhysicalClick({550,350});Chord('L');Chord('A');Type(L"BackspaceProbe");Key(VK_BACK);Key(VK_BACK,true);Pump(100);auto address=FocusText(frame);check("address-backspace-edits",address==L"BackspaceProb"&&CurrentUrl(frame)==folder);Escape();
   Chord('E');Type(L"BackspaceProbe");Key(VK_BACK);Key(VK_BACK,true);Pump(100);auto search=FocusText(frame);check("search-backspace-edits",search==L"BackspaceProb"&&CurrentUrl(frame)==folder);Escape();
   Navigate(frame,L"C:\\NativeLabFolders\\Gamma");PhysicalClick({550,350});Key(VK_HOME);Key(VK_HOME,true);Key(VK_F2);Key(VK_F2,true);Pump(250);Chord('A');Type(L"BackspaceProbe");Key(VK_BACK);Key(VK_BACK,true);Pump(100);const auto rename=FocusText(frame);check("rename-backspace-edits",rename==L"BackspaceProb");Escape();
   Navigate(frame,L"C:\\NativeLabFolders\\Beta");Navigate(frame,L"C:\\NativeLabFolders\\Gamma");PhysicalClick({550,350});Key(VK_BACK);Key(VK_BACK,true);Pump(700);check("folder-view-backspace-travels-history",CurrentUrl(frame)==L"file:///C:/NativeLabFolders/Beta");
   out<<"],\"address\":"<<Json(address)<<",\"search\":"<<Json(search)<<",\"rename\":"<<Json(rename);
  }
  out<<",\"success\":"<<(good?"true":"false")<<'}';PostMessageW(frame,apply,0,0);Pump(300);UnhookWindowsHookEx(hook);FreeLibrary(lab);CoUninitialize();return good?0:1;
 }
 auto tabs=Tabs(frame);if(tabs.size()==3){
  const size_t before=TabCount(frame);SetCursorPos(Center(tabs[1].rect).x,Center(tabs[1].rect).y);MouseButton(MOUSEEVENTF_MIDDLEUP);
  check("orphan-middle-up-does-not-close",TabCount(frame)==before);
  if(TabCount(frame)<before){Key(VK_SHIFT);Chord('T');Key(VK_SHIFT,true);Pump(600);}
  auto a=Access(frame);auto plus=Named(frame,L"New tab");size_t count=TabCount(frame);auto folder=CurrentUrl(frame);
  HRESULT focus=a?a->accSelect(SELFLAG_TAKEFOCUS,Child(plus.child)):E_FAIL;Pump(350);
  check("new-focus-does-not-invoke",SUCCEEDED(focus)&&TabCount(frame)==count&&CurrentUrl(frame)==folder);
  if(TabCount(frame)>count){Chord('W');Pump(500);}
  a=Access(frame);auto close=Named(frame,L"Close Alpha");check("independent-close-exposed",close.child>0);
  if(close.child){folder=CurrentUrl(frame);count=TabCount(frame);a->accSelect(SELFLAG_TAKEFOCUS,Child(close.child));Pump(150);VARIANT focusNow{};a->get_accFocus(&focusNow);check("close-focus-only",focusNow.vt==VT_I4&&focusNow.lVal==close.child&&TabCount(frame)==count&&CurrentUrl(frame)==folder);}
  a=Access(frame);VARIANT invalid{};invalid.vt=VT_BSTR;invalid.bstrVal=SysAllocString(L"wrong");VARIANT role{};check("invalid-accessible-argument",a&&a->get_accRole(invalid,&role)==E_INVALIDARG);VariantClear(&invalid);
  plus=Named(frame,L"New tab");count=TabCount(frame);folder=CurrentUrl(frame);VARIANT oldFocus{};a->get_accFocus(&oldFocus);HRESULT noAction=a->accSelect(SELFLAG_NONE,Child(plus.child));VARIANT afterFocus{};a->get_accFocus(&afterFocus);check("selection-none-has-no-action",noAction==S_OK&&TabCount(frame)==count&&CurrentUrl(frame)==folder&&oldFocus.vt==afterFocus.vt&&oldFocus.lVal==afterFocus.lVal);
  PhysicalClick({550,350});Key(VK_MENU);Key(VK_SPACE);Key(VK_SPACE,true);Key(VK_MENU,true);Pump(150);Key('T');Key('T',true);Pump(250);GUITHREADINFO keyboardFocus{sizeof(keyboardFocus)};GetGUIThreadInfo(GetWindowThreadProcessId(frame,nullptr),&keyboardFocus);check("native-system-menu-keyboard-entry",keyboardFocus.hwndFocus==Strip(frame));Escape();
  if(a){plus=Named(frame,L"New tab");a->accSelect(SELFLAG_TAKEFOCUS,Child(plus.child));SetCursorPos(20,700);Key(VK_APPS);Key(VK_APPS,true);Pump(300);HWND menu=FindWindowW(L"#32768",nullptr);RECT mr{};if(menu)GetWindowRect(menu,&mr);check("keyboard-menu-anchored-to-focus",menu&&abs(mr.left-plus.rect.left)<80&&abs(mr.top-plus.rect.bottom)<80);Escape();}
  count=TabCount(frame);auto target=Named(frame,L"Close Alpha");Point(target.rect);MouseButton(MOUSEEVENTF_LEFTDOWN);Point(Named(frame,L"Beta").rect);MouseButton(MOUSEEVENTF_LEFTUP);check("close-release-elsewhere-cancels",TabCount(frame)==count);
  target=Named(frame,L"Close Alpha");Point(target.rect);MouseButton(MOUSEEVENTF_LEFTDOWN);Escape();MouseButton(MOUSEEVENTF_LEFTUP);check("escape-cancels-press",TabCount(frame)==count);
  Point(target.rect);MouseButton(MOUSEEVENTF_LEFTDOWN);SendMessageW(Strip(frame),WM_CANCELMODE,0,0);MouseButton(MOUSEEVENTF_LEFTUP);check("cancelmode-cancels-press",TabCount(frame)==count);
  Point(target.rect);MouseButton(MOUSEEVENTF_LEFTDOWN);PostMessageW(frame,apply,81,0);Pump(200);MouseButton(MOUSEEVENTF_LEFTUP);PostMessageW(frame,apply,82,0);Pump(100);check("capture-theft-cancels-press",TabCount(frame)==count);
  auto beta=Named(frame,L"Beta");ComPtr<IDispatch> betaDispatch;ComPtr<IAccessible> betaObject;a=Access(frame);a->get_accChild(Child(beta.child),&betaDispatch);if(betaDispatch)betaDispatch.As(&betaObject);
  Point(Named(frame,L"Close Beta").rect);MouseButton(MOUSEEVENTF_LEFTDOWN);a->accDoDefaultAction(Child(Named(frame,L"Close Alpha").child));Pump(250);MouseButton(MOUSEEVENTF_LEFTUP);check("removed-sibling-does-not-retarget-release",TabCount(frame)==count-1&&Named(frame,L"Beta").child&&Named(frame,L"Gamma").child);
  VARIANT self=Child(CHILDID_SELF);HRESULT retained=betaObject?betaObject->accDoDefaultAction(self):E_FAIL;Pump(700);check("retained-accessible-child-keeps-identity",SUCCEEDED(retained)&&CurrentUrl(frame)==L"file:///C:/NativeLabFolders/Beta");
  ReopenPhysical();count=TabCount(frame);auto middle=Named(frame,L"Alpha");Point(middle.rect);MouseButton(MOUSEEVENTF_MIDDLEDOWN);MouseButton(MOUSEEVENTF_MIDDLEUP);check("matching-middle-close-exactly-once",TabCount(frame)==count-1&&!Named(frame,L"Alpha").child&&Named(frame,L"Gamma").child&&Named(frame,L"Beta").child);ReopenPhysical();
  // The modal mutation case runs in its own "menu" fixture with a fresh frame.
  const auto beforeHandoff=TabCount(frame);const auto beforeFolder=CurrentUrl(frame);DWORD_PTR detachResult{};SendMessageTimeoutW(frame,patch::mvp::DetachMessage(),1,0,SMTO_ABORTIFHUNG,2000,&detachResult);Pump(250);bool detached=!Strip(frame)&&!GetPropW(frame,L"Win10NativePatch.ExplorerController.v2");PostMessageW(frame,apply,64,0);Pump(1200);check("live-window-update-continuity",detachResult==1&&detached&&TabCount(frame)==beforeHandoff&&CurrentUrl(frame)==beforeFolder);
  PhysicalClick({550,350});folder=CurrentUrl(frame);Chord('L');Chord('A');Type(L"BackspaceProbe");Key(VK_BACK);Key(VK_BACK,true);Pump(100);check("address-backspace-edits",FocusText(frame)==L"BackspaceProb"&&CurrentUrl(frame)==folder);Escape();
  Chord('E');Type(L"BackspaceProbe");Key(VK_BACK);Key(VK_BACK,true);Pump(100);check("search-backspace-edits",FocusText(frame)==L"BackspaceProb"&&CurrentUrl(frame)==folder);Escape();
  PhysicalClick({550,350});std::vector<std::wstring> route;for(int i=0;i<8;i++){Key(VK_F6);Key(VK_F6,true);Pump(100);GUITHREADINFO info{sizeof(info)};GetGUIThreadInfo(GetWindowThreadProcessId(frame,nullptr),&info);route.push_back(Class(info.hwndFocus));}
  out<<"],\"nativeF6FocusRoute\":[";for(size_t i=0;i<route.size();++i){if(i)out<<',';out<<Json(route[i]);}
 }else out<<"],\"nativeF6FocusRoute\":[";
 // Documented Explorer creation supplies the exact created frame identity.
 PostMessageW(frame,apply,80,0);Pump(1000);SHANDLE_PTR raw=reinterpret_cast<SHANDLE_PTR>(GetPropW(frame,L"HeaderLab.CreatedFrame"));HRESULT creation=static_cast<HRESULT>(reinterpret_cast<LONG_PTR>(GetPropW(frame,L"HeaderLab.CreationResult"))),handle=creation;HWND owned=reinterpret_cast<HWND>(raw);bool exact=SUCCEEDED(creation)&&Class(owned)==L"CabinetWClass"&&owned!=frame;
 out<<"],\"documentedCreation\":{\"hr\":"<<creation<<",\"handleHr\":"<<handle<<",\"hwnd\":"<<raw<<",\"exactFrame\":"<<(exact?"true":"false")<<"},\"success\":"<<(good&&exact?"true":"false")<<'}';
 if(owned)PostMessageW(owned,WM_CLOSE,0,0);PostMessageW(frame,apply,0,0);Pump(350);UnhookWindowsHookEx(hook);FreeLibrary(lab);SetWindowPos(frame,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);CoUninitialize();return good&&exact?0:1;
}
