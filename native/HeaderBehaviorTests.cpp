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
// ---- Lab timing (U1 baseline and later comparisons) ----
HANDLE TimingFile(){
 static HANDLE file=[]{wchar_t path[MAX_PATH]{};DWORD bytes=sizeof(path);
  if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Win10NativePatch\\v2",L"TimingLog",RRF_RT_REG_SZ,nullptr,path,&bytes)!=ERROR_SUCCESS||!*path)return INVALID_HANDLE_VALUE;
  return CreateFileW(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);}();
 return file;
}
void Mark(const char* event,ULONGLONG a=0){
 HANDLE file=TimingFile();if(file==INVALID_HANDLE_VALUE)return;LARGE_INTEGER now{},rate{};QueryPerformanceCounter(&now);QueryPerformanceFrequency(&rate);
 const auto t=static_cast<ULONGLONG>(now.QuadPart),f=static_cast<ULONGLONG>(rate.QuadPart);char line[160]{};
 const int n=wsprintfA(line,"%I64u %s %I64u 0\r\n",t/f*1000000+t%f*1000000/f,event,a);DWORD written{};if(n>0)WriteFile(file,line,static_cast<DWORD>(n),&written,nullptr);
}
// Navigation keys go out as the dedicated (extended) keys; a numpad PgUp with
// NumLock on would be "un-shifted" by Windows and lose Shift.
void NavKey(WORD key,bool up=false){INPUT i{};i.type=INPUT_KEYBOARD;i.ki.wVk=key;i.ki.dwFlags=KEYEVENTF_EXTENDEDKEY|(up?KEYEVENTF_KEYUP:0);SendInput(1,&i,sizeof(i));}
void CtrlKey(WORD key,bool shift=false){const bool nav=key==VK_PRIOR||key==VK_NEXT||key==VK_END||key==VK_HOME;if(shift)Key(VK_SHIFT);Key(VK_CONTROL);if(nav){NavKey(key);NavKey(key,true);}else{Key(key);Key(key,true);}Key(VK_CONTROL,true);if(shift)Key(VK_SHIFT,true);}
int SelectedIndex(HWND frame){auto tabs=Tabs(frame);for(size_t i=0;i<tabs.size();++i)if(tabs[i].state&STATE_SYSTEM_SELECTED)return static_cast<int>(i);return -1;}
void NavigateMarked(HWND frame,const std::wstring& folder,const char* label){
 ComPtr<IShellWindows> windows;if(FAILED(CoCreateInstance(CLSID_ShellWindows,nullptr,CLSCTX_ALL,IID_PPV_ARGS(&windows))))return;long count{};windows->get_Count(&count);
 for(long i=0;i<count;++i){VARIANT index{};index.vt=VT_I4;index.lVal=i;ComPtr<IDispatch> item;ComPtr<IWebBrowser2> browser;
  if(FAILED(windows->Item(index,&item))||FAILED(item.As(&browser)))continue;SHANDLE_PTR handle{};browser->get_HWND(&handle);if(reinterpret_cast<HWND>(handle)!=frame)continue;
  VARIANT destination{};destination.vt=VT_BSTR;destination.bstrVal=SysAllocString(folder.c_str());VARIANT empty{};
  Mark(label);browser->Navigate2(&destination,&empty,&empty,&empty,&empty);VariantClear(&destination);Pump(2000);return;}
}
void SwitchRounds(HWND frame,WORD first,WORD second,unsigned rounds,const char* label){
 for(unsigned i=0;i<rounds;++i){Mark(label,first);CtrlKey(first);Pump(1800);Mark(label,second);CtrlKey(second);Pump(2200);}
}
template<class Check> bool RapidSwitchAndMeasure(HWND frame,Check& check,std::ostream& out){
 Navigate(frame,L"C:\\NativeLabFolders\\Big");Pump(1500);
 Mark("phase",3);SwitchRounds(frame,'1','3',8,"input-switch");
 Mark("phase",4);CtrlKey('1');Pump(1500);
 for(unsigned i=0;i<8;++i){NavigateMarked(frame,L"C:\\NativeLabFolders\\Big","input-plain");NavigateMarked(frame,L"C:\\NativeLabFolders\\Alpha","input-plain");}
 Mark("phase",5);CtrlKey('T');Pump(1500);CtrlKey('T');Pump(1500);CtrlKey('1');Pump(1500);
 const int before=SelectedIndex(frame);
 for(int i=0;i<3;++i){Mark("input-rapid",i);CtrlKey(VK_TAB);Pump(80);}
 Pump(3000);const int after=SelectedIndex(frame);
 check("timing-bootstrap",TabCount(frame)==5);
 out<<"],\"rapid\":{\"tabs\":"<<TabCount(frame)<<",\"before\":"<<before<<",\"after\":"<<after<<'}';
 Mark("phase",6);RECT r{};GetWindowRect(frame,&r);Mark("drag-begin");
 for(ULONGLONG end=GetTickCount64()+5000;GetTickCount64()<end;){static int step=0;++step;SetWindowPos(frame,nullptr,r.left+(step%40)*4,r.top+(step%40)*2,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);Pump(16);}
 Mark("drag-end");SetWindowPos(frame,nullptr,r.left,r.top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);Pump(500);
 Mark("phase",7);CtrlKey('1');Pump(1500);
 for(size_t target:{size_t{10},size_t{120}}){
  size_t stuck=0;while(TabCount(frame)<target&&stuck<3){const size_t n=TabCount(frame);Mark("input-new",n);CtrlKey('T');Pump(500);stuck=TabCount(frame)==n?stuck+1:0;}
  out<<",\"tabs"<<target<<"\":"<<TabCount(frame);
  Mark("phase",static_cast<ULONGLONG>(TabCount(frame)));SwitchRounds(frame,'1','9',5,"input-switch-many");
 }
 return after==(before+3)%5;
}
// ---- U2 switching fixture helpers ----
ComPtr<IWebBrowser2> Browser(HWND frame){ComPtr<IShellWindows> windows;if(FAILED(CoCreateInstance(CLSID_ShellWindows,nullptr,CLSCTX_ALL,IID_PPV_ARGS(&windows))))return {};long count{};windows->get_Count(&count);
 for(long i=0;i<count;++i){ComPtr<IDispatch> item;ComPtr<IWebBrowser2> browser;if(FAILED(windows->Item(Child(i),&item))||FAILED(item.As(&browser)))continue;SHANDLE_PTR handle{};browser->get_HWND(&handle);if(reinterpret_cast<HWND>(handle)==frame)return browser;}return {};}
ComPtr<IShellFolderViewDual> ViewOf(HWND frame){auto browser=Browser(frame);ComPtr<IDispatch> document;ComPtr<IShellFolderViewDual> view;if(browser&&SUCCEEDED(browser->get_Document(&document))&&document)document.As(&view);return view;}
std::wstring ViewSelection(HWND frame){auto view=ViewOf(frame);ComPtr<FolderItems> items;ComPtr<FolderItem> item;long count{};BSTR name{};
 if(!view||FAILED(view->SelectedItems(&items))||!items||FAILED(items->get_Count(&count))||count!=1||FAILED(items->Item(Child(0),&item))||!item||FAILED(item->get_Name(&name)))return L"";std::wstring result=name?name:L"";SysFreeString(name);return result;}
bool SelectInView(HWND frame,const std::wstring& name){// shldisp's Folder is renamed by the include macro above.
 auto view=ViewOf(frame);ComPtr<struct HeaderFolderPath> folder;ComPtr<FolderItem> item;BSTR text=SysAllocString(name.c_str());
 const bool found=view&&SUCCEEDED(view->get_Folder(&folder))&&folder&&SUCCEEDED(folder->ParseName(text,&item))&&item;SysFreeString(text);if(!found)return false;
 VARIANT target{};target.vt=VT_DISPATCH;target.pdispVal=item.Get();return SUCCEEDED(view->SelectItem(&target,SVSI_SELECT|SVSI_DESELECTOTHERS|SVSI_FOCUSED|SVSI_ENSUREVISIBLE));}
std::vector<std::wstring> TabNames(HWND frame){std::vector<std::wstring> names;for(auto& tab:Tabs(frame))names.push_back(tab.name);return names;}std::wstring FocusText(HWND frame){GUITHREADINFO info{sizeof(info)};GetGUIThreadInfo(GetWindowThreadProcessId(frame,nullptr),&info);wchar_t text[4096]{};SendMessageTimeoutW(info.hwndFocus,WM_GETTEXT,4096,reinterpret_cast<LPARAM>(text),SMTO_ABORTIFHUNG,1000,nullptr);if(*text)return text;ComPtr<IUIAutomation> automation;ComPtr<IUIAutomationElement> element;ComPtr<IUIAutomationValuePattern> pattern;if(SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&automation)))&&SUCCEEDED(automation->GetFocusedElement(&element))&&SUCCEEDED(element->GetCurrentPatternAs(UIA_ValuePatternId,IID_PPV_ARGS(&pattern)))){BSTR value{};pattern->get_CurrentValue(&value);std::wstring result=value?value:L"";SysFreeString(value);return result;}return L"";}
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
 if(mode==L"timing"){
  // Timing marks share the product's QPC clock and log file (TimingLog).
  const bool rapid=RapidSwitchAndMeasure(frame,check,out);
  out<<",\"rapidSwitchDelivered\":"<<(rapid?"true":"false")<<",\"success\":"<<(good?"true":"false")<<'}';
  PostMessageW(frame,apply,0,0);Pump(400);UnhookWindowsHookEx(hook);FreeLibrary(lab);PostMessageW(frame,WM_CLOSE,0,0);CoUninitialize();return good?0:1;
 }
 if(mode==L"overflow"){
  // AE3/AE4: many tabs shrink to the 76 DIP minimum, scroll by whole tabs and
  // stay reachable; the wrapper records the LastSession size afterwards.
  const int dpi=static_cast<int>(GetDpiForWindow(frame)),minimum=MulDiv(76,dpi,96),unit=MulDiv(30,dpi,96);
  size_t stuck=0;while(TabCount(frame)<120&&stuck<3){const size_t n=TabCount(frame);CtrlKey('T');Pump(350);stuck=TabCount(frame)==n?stuck+1:0;}
  Pump(1500);
  auto tabs=Tabs(frame);
  auto firstShown=[&]{tabs=Tabs(frame);for(size_t i=0;i<tabs.size();++i)if(!(tabs[i].state&STATE_SYSTEM_OFFSCREEN))return static_cast<int>(i);return -1;};
  auto width=[](const Target& t){return static_cast<int>(t.rect.right-t.rect.left);};
  int offscreen=0;for(auto& t:tabs)if(t.state&STATE_SYSTEM_OFFSCREEN)++offscreen;
  const int selected=SelectedIndex(frame),shown=firstShown();
  // The neighbour of the revealed last tab is whole and, being narrow, has no close button.
  const int neighbour=tabs.size()>=2?width(tabs[tabs.size()-2]):0;
  check("overflow-120-tabs",tabs.size()==120);
  check("overflow-minimum-width",neighbour>=minimum-1&&neighbour<=minimum+1);
  check("overflow-hides-offscreen-tabs",offscreen>0&&shown>0);
  check("overflow-selected-revealed",selected==119&&!(tabs[119].state&STATE_SYSTEM_OFFSCREEN));
  bool closeHidden=false;for(auto& t:Targets(frame))if(tabs.size()>=2&&t.child==tabs[tabs.size()-2].child+1)closeHidden=(t.state&STATE_SYSTEM_OFFSCREEN)!=0;
  check("overflow-narrow-tab-close-hidden",closeHidden);
  const RECT neighbourRect=tabs.size()>=2?tabs[tabs.size()-2].rect:RECT{};
  SetCursorPos((neighbourRect.left+neighbourRect.right)/2,(neighbourRect.top+neighbourRect.bottom)/2);Pump(200);
  for(int i=0;i<3;++i){INPUT wheel{};wheel.type=INPUT_MOUSE;wheel.mi.dwFlags=MOUSEEVENTF_WHEEL;wheel.mi.mouseData=WHEEL_DELTA;SendInput(1,&wheel,sizeof(wheel));Pump(150);}
  Pump(400);const int afterWheel=firstShown();
  check("overflow-wheel-scrolls-by-tabs",shown>=3&&afterWheel==shown-3);
  check("overflow-wheel-keeps-selection",SelectedIndex(frame)==119);
  if(auto a=Access(frame);a&&!tabs.empty())a->accDoDefaultAction(Child(tabs[0].child));
  Pump(1500);const int afterReveal=firstShown();
  check("overflow-offscreen-tab-reachable",SelectedIndex(frame)==0&&afterReveal==0);
  RECT strip{};GetWindowRect(Strip(frame),&strip);
  PhysicalClick({strip.right-unit*2-unit/2,(strip.top+strip.bottom)/2});Pump(300);
  check("overflow-right-arrow-scrolls-one-tab",firstShown()==1);
  out<<"],\"dpi\":"<<dpi<<",\"tabs\":"<<tabs.size()<<",\"tabWidth\":"<<neighbour<<",\"minimum\":"<<minimum<<",\"offscreen\":"<<offscreen
     <<",\"firstShown\":"<<shown<<",\"afterWheel\":"<<afterWheel<<",\"afterReveal\":"<<afterReveal<<",\"success\":"<<(good?"true":"false")<<'}';
  PostMessageW(frame,apply,0,0);Pump(400);UnhookWindowsHookEx(hook);FreeLibrary(lab);PostMessageW(frame,WM_CLOSE,0,0);CoUninitialize();return good?0:1;
 }
 if(mode==L"switching"){
  // AE1/AE2 and the Firefox chords. Tabs: Alpha, Beta, Gamma (Gamma shown).
  CreateDirectoryW(L"C:\\NativeLabFolders\\Gamma",nullptr);{HANDLE z=CreateFileW(L"C:\\NativeLabFolders\\Gamma\\Zeta.txt",GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);if(z!=INVALID_HANDLE_VALUE)CloseHandle(z);}
  Pump(800);const bool picked=SelectInView(frame,L"RenameProbe.txt");Pump(300);const auto beforeLeave=ViewSelection(frame);
  CtrlKey(VK_NEXT);Pump(900);check("ctrl-pagedown-wraps-to-first",SelectedIndex(frame)==0&&CurrentUrl(frame)==L"file:///C:/NativeLabFolders/Alpha");
  CtrlKey(VK_PRIOR);Pump(900);check("ctrl-pageup-wraps-to-last",SelectedIndex(frame)==2&&CurrentUrl(frame)==L"file:///C:/NativeLabFolders/Gamma");
  CtrlKey('1');Pump(1200);CtrlKey('3');Pump(900);const auto returned=ViewSelection(frame);Pump(1600);const auto returnedLate=ViewSelection(frame);
  // Informational: a selection set through shell automation does not come back
  // after a tab round-trip in the baseline either (reported, not gated).
  CtrlKey(VK_PRIOR,true);Pump(500);auto moved=TabNames(frame);
  check("ctrl-shift-pageup-moves-tab-left",moved.size()==3&&moved[0]==L"Alpha"&&moved[1]==L"Gamma"&&moved[2]==L"Beta"&&SelectedIndex(frame)==1);
  CtrlKey(VK_NEXT,true);Pump(500);CtrlKey(VK_NEXT,true);Pump(500);moved=TabNames(frame);
  check("ctrl-shift-pagedown-stops-at-end",moved.size()==3&&moved[2]==L"Gamma"&&SelectedIndex(frame)==2&&CurrentUrl(frame)==L"file:///C:/NativeLabFolders/Gamma");
  // AE2: a selection the user makes after arrival is not overwritten.
  CtrlKey('1');Pump(1200);CtrlKey('3');Pump(400);NavKey(VK_END);NavKey(VK_END,true);Pump(1800);
  const auto chosen=ViewSelection(frame);check("user-selection-survives-restore",chosen==L"Zeta");
  // AE1: three quick requests while a slow folder loads end on the last one.
  CtrlKey('2');Pump(900);Navigate(frame,L"C:\\NativeLabFolders\\Big");Pump(600);CtrlKey('3');Pump(1200);
  const ULONGLONG start=GetTickCount64();CtrlKey('2');Pump(30);CtrlKey('1');Pump(30);CtrlKey('3');Pump(30);CtrlKey('2');
  const int immediate=SelectedIndex(frame);Pump(3000);
  check("rapid-requests-settle-on-last",SelectedIndex(frame)==1&&CurrentUrl(frame)==L"file:///C:/NativeLabFolders/Big");
  out<<"],\"picked\":"<<(picked?"true":"false")<<",\"beforeLeave\":"<<Json(beforeLeave)<<",\"returned\":"<<Json(returned)<<",\"returnedLate\":"<<Json(returnedLate)<<",\"immediate\":"<<immediate<<",\"selection\":"<<Json(chosen)<<",\"rapidMs\":"<<(GetTickCount64()-start)<<",\"tabs\":";State(out,frame);
  out<<",\"success\":"<<(good?"true":"false")<<'}';PostMessageW(frame,apply,0,0);Pump(300);UnhookWindowsHookEx(hook);FreeLibrary(lab);PostMessageW(frame,WM_CLOSE,0,0);CoUninitialize();return good?0:1;
 } if(mode==L"drag"){
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
