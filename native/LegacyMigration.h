#pragma once
#include "Patch.h"
namespace migration {
struct Value { DWORD present{}, value{}; };
struct Legacy { bool found{}, qtOwned{}; std::array<Value,9> baseline{}; };
bool ReadLegacy(const std::wstring& path,Legacy& result,std::wstring& error);
bool PrepareLegacyImport(std::wstring& error);
int SelfTest(const std::wstring& report);
}
