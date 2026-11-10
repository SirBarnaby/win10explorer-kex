#pragma once
#include <windows.h>
#include "HeaderLayout.h"
namespace header {
inline constexpr wchar_t RowProperty[]=L"Win10NativePatch.HeaderRowPixels.v1";
bool Attach(HWND frame);
// transient: called from inside a size/position change; keeps the current
// caption/row mode and defers any mode switch to the queued settled pass.
bool Allocate(HWND frame,HWND ownedSurface,int minimumWidth,Allocation& allocation,bool transient=false);
// Invalidates input/paint/accessibility immediately, before the coalesced reflow.
void Invalidate(HWND frame);
bool IsCurrent(HWND frame,const Allocation& allocation);
void Detach(HWND frame,bool restore=true);
UINT ChangedMessage();
}
