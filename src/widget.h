#pragma once
#include <windows.h>
#include <vector>
#include "brands.h"
#include "devices.h"

namespace widget {

struct Entry {
    DeviceStatus status;
    Accent accent = Accent::None;
    std::wstring shortLabel;
};

void Create(HINSTANCE hInst, HWND ownerWnd, UINT clickMsg);
void Destroy();
void Update(const std::vector<Entry>& devices);

// Returns the screen rectangle of the primary taskbar widget (used to anchor the Flyout).
bool GetScreenRect(RECT& out);

// Taskbar & tray appearance options (saved under HKCU\Software\WirelessBattery).
bool ShowNumbers();
void SetShowNumbers(bool on);
bool ColorPct();
void SetColorPct(bool on);
bool ShortNames();
void SetShortNames(bool on);
bool ShowOff();
void SetShowOff(bool on);
bool TrayBars();
void SetTrayBars(bool on);

// Per-device colour override: -1 = automatic (from brand), 0 = none, >0 = Accent enum value.
int  AccentPref(const std::wstring& key);
void SetAccentPref(const std::wstring& key, int pref);

}  // namespace widget
