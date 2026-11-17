#pragma once
// Windows 11 style modern flyout panel showing detailed battery cards, progress bars,
// discharge rate / ETA telemetry, and quick action buttons.
#include <windows.h>
#include <string>
#include <vector>
#include "devices.h"

struct FlyoutCard {
    std::wstring key;
    std::wstring name;
    std::wstring via;
    std::wstring statusText;
    std::wstring etaText;
    DeviceKind kind = DeviceKind::Other;
    int  percent   = -1;
    bool charging  = false;
    bool asleep    = false;
    bool connected = true;
    bool low       = false;
    bool hidden    = false;
};

namespace flyout {

enum Action : UINT {
    ACT_NONE = 0,
    ACT_REFRESH = 1,
    ACT_SETTINGS = 2,
    ACT_TOGGLE_DEVICE_BASE = 1000
};

void Init(HINSTANCE inst, HWND notifyWnd, UINT actionMsg);
void Destroy();
bool IsVisible();
void Toggle(const RECT& anchorRect, bool lightTheme);
void Hide();
void SetData(const std::vector<FlyoutCard>& cards, int pollSec, int lowBattPct, bool lightTheme);
void NotifyRefreshing();

}  // namespace flyout
