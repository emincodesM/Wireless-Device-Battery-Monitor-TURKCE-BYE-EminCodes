#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <vector>
#include "devices.h"

namespace tray {

constexpr UINT_PTR kTimerRestoreIcon = 2;

void UpdateIcon(HWND wnd, bool add, UINT callbackMsg,
                const std::vector<DeviceStatus>& allConnected,
                const std::vector<DeviceStatus>& taskbarVisible);
void RemoveIcon(HWND wnd);
void ResetNotifState();
void OnBalloonClosed(HWND wnd, UINT callbackMsg,
                     const std::vector<DeviceStatus>& allConnected,
                     const std::vector<DeviceStatus>& taskbarVisible);

void ShowLowBatteryNotification(HWND wnd, const DeviceStatus& d);
void ShowFullChargeNotification(HWND wnd, const DeviceStatus& d);
void ShowConnectionNotification(HWND wnd, const DeviceStatus& d, bool connected);

void ShowContextMenu(HWND wnd, const std::wstring& onlyKey,
                     const std::function<void()>& onRedraw,
                     const std::function<void()>& onPollNow);

}  // namespace tray
