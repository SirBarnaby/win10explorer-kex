#pragma once
#include <windows.h>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace snap {
inline constexpr std::uint32_t DefaultShare=5000;
RECT Tile(const RECT& work,bool right,int row,std::uint32_t leftShare=DefaultShare);
std::optional<std::uint32_t> SharedSplit(const RECT& left,const RECT& right,const RECT& work);
std::wstring DisplayIdentity(HMONITOR monitor);
class Preferences {
public:
    explicit Preferences(std::wstring key=L"Software\\Win10NativePatch\\SnapLayout\\v1"):key_(std::move(key)){}
    std::uint32_t Load(const std::wstring& display) const;
    bool Save(const std::wstring& display,std::uint32_t leftShare) const;
private:
    std::wstring key_;
};
}
