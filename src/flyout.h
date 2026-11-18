#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "battery_model.h"
#include "brands.h"
#include "devices.h"

namespace flyout {

struct Entry {
    DeviceStatus status;
    Accent accent = Accent::None;
    bool hiddenOnTaskbar = false;
    double dischargeRatePerHour = 0.0; // >0 = %/hour change rate
    std::wstring etaText;              // e.g. "~14 sa 30 dk kaldı" or "~45 dk sonra tam dolu"
    std::vector<BatterySample> history;
};

// Action codes sent to `ownerWnd` via `actionMsg` (wParam):
//   0..999 : open per-device context menu for OrderedEntries()[wParam]
//   1000   : refresh now (PollNow)
//   1001   : open general context menu
//   1002   : settings or visibility changed inside Flyout (redraw taskbar & tray)
constexpr WPARAM kActionRefresh         = 1000;
constexpr WPARAM kActionGeneralMenu     = 1001;
constexpr WPARAM kActionSettingsChanged = 1002;

void Create(HINSTANCE hInst, HWND ownerWnd, UINT trayCallbackMsg, UINT actionMsg);
void Destroy();
void Update(const std::vector<Entry>& devices);
void Toggle(HWND trayFallbackWnd);
void Hide();
bool IsVisible();

// Returns the entries in the exact display order used by the Flyout and Context Menu (P0-1).
const std::vector<Entry>& OrderedEntries();
std::wstring KeyAt(int index);

}  // namespace flyout
