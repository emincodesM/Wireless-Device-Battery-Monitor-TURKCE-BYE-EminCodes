#pragma once
#include <windows.h>
#include <atomic>
#include <set>
#include <string>
#include <vector>

namespace settings {

extern std::atomic<int> g_pollIntervalSec;
extern int  g_lowBatteryThreshold;
extern bool g_notifyLowBattery;
extern bool g_notifyFullCharge;
extern bool g_notifyConnect;
extern bool g_debugLog;

extern std::set<std::wstring>    g_hiddenDevices;
extern std::vector<std::wstring> g_deviceOrder;

void Load();
void Save();

std::wstring ExePath();
bool IsAutoStartEnabled();
void SetAutoStart(bool enable);

void EnableDebugLog(bool enable);
void LogDebug(const wchar_t* fmt, ...);

}  // namespace settings
