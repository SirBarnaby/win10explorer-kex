#include "SnapLayout.h"
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;
    std::vector<std::pair<std::string,bool>> checks;
    auto check=[&](const char* name,bool pass){checks.emplace_back(name,pass);};
    auto same=[](const RECT& a,const RECT& b){return EqualRect(&a,&b)!=FALSE;};
    RECT work{-1921,-50,0,1031};
    RECT left=snap::Tile(work,false,0,4000),right=snap::Tile(work,true,0,4000);
    check("forty_percent_uses_current_work_area",same(left,RECT{-1921,-50,-1153,1031}));
    check("complementary_odd_width_has_no_gap",left.right==right.left&&right.right==work.right&&right.top==work.top&&right.bottom==work.bottom);
    check("bottom_corner_keeps_horizontal_preference",same(snap::Tile(work,false,1,4000),RECT{-1921,490,-1153,1031}));
    check("top_right_keeps_complementary_width",same(snap::Tile(work,true,-1,4000),RECT{-1153,-50,0,490}));
    check("default_half_uses_one_boundary",same(snap::Tile(work,false,0),RECT{-1921,-50,-961,1031}));
    check("sixty_percent_complements_forty",same(snap::Tile(RECT{0,0,1000,800},true,0,6000),RECT{600,0,1000,800}));
    check("changed_work_area_recomputes_split",same(snap::Tile(RECT{20,40,1220,940},false,0,4000),RECT{20,40,500,940}));
    check("invalid_share_uses_default",same(snap::Tile(work,false,0,0),snap::Tile(work,false,0))&&same(snap::Tile(work,true,0,10000),snap::Tile(work,true,0)));
    auto observed=snap::SharedSplit(RECT{0,0,400,800},RECT{400,0,1000,800},RECT{0,0,1000,800});
    check("completed_pair_measures_forty_percent",observed&&*observed==4000);
    check("unrelated_geometry_does_not_measure",!snap::SharedSplit(RECT{20,0,400,800},RECT{400,0,1000,800},RECT{0,0,1000,800}));
    check("overlapping_pair_does_not_measure",!snap::SharedSplit(RECT{0,0,500,800},RECT{400,0,1000,800},RECT{0,0,1000,800}));
    check("corner_pair_does_not_measure",!snap::SharedSplit(RECT{0,0,400,400},RECT{400,0,1000,800},RECT{0,0,1000,800}));
    check("zero_width_work_does_not_measure",!snap::SharedSplit(RECT{},RECT{},RECT{}));
    auto odd=snap::SharedSplit(left,right,work);
    check("odd_width_measurement_round_trips_pixels",odd&&same(snap::Tile(work,false,0,*odd),left));
    std::wstring key=L"Software\\Win10NativePatch\\Tests\\Split-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    snap::Preferences preferences(key);
    check("missing_display_defaults_to_half",preferences.Load(L"display-a")==5000);
    bool saved=preferences.Save(L"display-a",4000);
    check("intentional_split_can_be_saved",saved);
    snap::Preferences restarted(key);
    check("new_store_instance_remembers_split",restarted.Load(L"display-a")==4000);
    check("other_display_keeps_its_default",restarted.Load(L"display-b")==5000);
    check("display_preferences_are_independent",preferences.Save(L"display-b",6000)&&restarted.Load(L"display-a")==4000&&restarted.Load(L"display-b")==6000);
    check("invalid_share_cannot_replace_preference",!preferences.Save(L"display-a",0)&&!preferences.Save(L"display-a",10000)&&preferences.Load(L"display-a")==4000);
    check("unknown_display_is_not_persisted",!preferences.Save(L"",4000)&&preferences.Load(L"")==5000);
    HKEY raw{};
    if(RegOpenKeyExW(HKEY_CURRENT_USER,key.c_str(),0,KEY_SET_VALUE,&raw)==ERROR_SUCCESS){
        DWORD corrupt=123;
        RegSetValueExW(raw,L"display-a",0,REG_DWORD,reinterpret_cast<const BYTE*>(&corrupt),sizeof(corrupt));
        check("corrupt_record_uses_default",preferences.Load(L"display-a")==5000);
        struct Record{DWORD size,version,share,reserved;} future{16,2,4000,0};
        RegSetValueExW(raw,L"display-a",0,REG_BINARY,reinterpret_cast<const BYTE*>(&future),sizeof(future));
        check("future_record_is_not_interpreted",preferences.Load(L"display-a")==5000);
        check("future_record_is_preserved",!preferences.Save(L"display-a",6000));
        RegCloseKey(raw);
    }else{
        check("corrupt_record_uses_default",false);check("future_record_is_not_interpreted",false);check("future_record_is_preserved",false);
    }
    RegDeleteTreeW(HKEY_CURRENT_USER,key.c_str());
    bool success=true;for(const auto& c:checks)success=success&&c.second;
    std::ofstream out(argv[1]);
    out<<"{\"success\":"<<(success?"true":"false")<<",\"checks\":[";
    for(size_t i=0;i<checks.size();++i)out<<(i?",":"")<<"{\"name\":\""<<checks[i].first<<"\",\"passed\":"<<(checks[i].second?"true":"false")<<"}";
    out<<"]}\n";return !out?2:success?0:1;
}
