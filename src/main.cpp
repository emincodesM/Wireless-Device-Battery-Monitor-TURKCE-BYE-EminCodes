#include "battery_model.h"
#include "brands.h"
#include "devices.h"
#include "flyout.h"
#include "i18n.h"
#include "labels.h"
#include "settings.h"
#include "tray.h"
#include "widget.h"
#include "../res/version.h"

#include <windows.h>
#include <dbt.h>
#include <hidsdi.h>
#include <shellapi.h>
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr UINT WM_TRAY          = WM_APP + 1;
constexpr UINT WM_POLL_DONE     = WM_APP + 2;
constexpr UINT WM_WIDGET_CLICK  = WM_APP + 3;
constexpr UINT WM_FLYOUT_ACTION = WM_APP + 4;
constexpr UINT WM_SHOW_FLYOUT   = WM_APP + 5;

constexpr UINT_PTR TIMER_DEBOUNCE = 1;
constexpr const wchar_t* kHostClass = L"WirelessBatteryHostWnd";

HWND   g_wnd = nullptr;
HANDLE g_wakeEvent = nullptr;
HANDLE g_stopEvent = nullptr;
HANDLE g_pollThread = nullptr;
HDEVNOTIFY g_devNotifyHid = nullptr;
HDEVNOTIFY g_devNotifyCom = nullptr;

std::vector<DeviceStatus> g_devices;
std::map<std::wstring, int> g_lowWarnedAt;
std::map<std::wstring, bool> g_fullChargedWarned;
std::map<std::wstring, bool> g_prevConnected;
bool g_firstPollDone = false;

struct CachedReading {
    DeviceStatus status;
    ULONGLONG seenAt = 0;
};
std::map<std::wstring, CachedReading> g_cache;
std::map<std::wstring, BatteryTrackState> g_batteryTrack;

void UpdateAllBatteryTracks() {
    ULONGLONG now = GetTickCount64();
    bool trLang = IsTurkish();
    for (const auto& d : g_devices) {
        if (!d.connected || d.asleep || d.percent < 0) continue;
        UpdateBatteryTrack(g_batteryTrack[d.key], d.percent, d.charging, now, trLang);
    }
}

Accent ResolveAccent(const DeviceStatus& d) {
    int pref = widget::AccentPref(d.key);
    if (pref == 0) return Accent::None;
    if (pref > 0)  return (Accent)pref;
    return BrandOf(d);
}

// Single canonical device ordering used by both the Taskbar Widget, the Flyout, and the Tray Menu (P0-1).
std::vector<DeviceStatus> OrderedDevices() {
    auto rank = [](const std::wstring& key) -> size_t {
        auto it = std::find(settings::g_deviceOrder.begin(), settings::g_deviceOrder.end(), key);
        return it == settings::g_deviceOrder.end() ? SIZE_MAX : (size_t)(it - settings::g_deviceOrder.begin());
    };
    std::vector<DeviceStatus> sorted = g_devices;
    std::stable_sort(sorted.begin(), sorted.end(), [&](const DeviceStatus& a, const DeviceStatus& b) {
        return rank(a.key) < rank(b.key);
    });
    return sorted;
}

std::vector<widget::Entry> VisibleWidgetEntries(const std::vector<DeviceStatus>& ordered) {
    std::vector<widget::Entry> v;
    for (const auto& d : ordered) {
        if (!d.connected && !widget::ShowOff()) continue;
        if (!settings::g_hiddenDevices.count(d.key)) v.push_back({ d, ResolveAccent(d), {} });
    }
    if (widget::ShortNames()) {
        std::vector<LabelInput> in;
        in.reserve(v.size());
        for (const auto& e : v) in.push_back({ e.status.kind, e.status.name });
        auto labels = ShortLabels(in);
        for (size_t i = 0; i < v.size() && i < labels.size(); i++) v[i].shortLabel = std::move(labels[i]);
    }
    return v;
}

std::vector<DeviceStatus> VisibleTaskbarStatuses(const std::vector<DeviceStatus>& ordered) {
    std::vector<DeviceStatus> out;
    for (const auto& d : ordered) {
        if (!d.connected && !widget::ShowOff()) continue;
        if (!settings::g_hiddenDevices.count(d.key)) out.push_back(d);
    }
    return out;
}

std::vector<flyout::Entry> BuildFlyoutEntries(const std::vector<DeviceStatus>& ordered) {
    std::vector<flyout::Entry> v;
    v.reserve(ordered.size());
    bool trLang = IsTurkish();
    for (const auto& d : ordered) {
        flyout::Entry e;
        e.status = d;
        e.accent = ResolveAccent(d);
        e.hiddenOnTaskbar = settings::g_hiddenDevices.count(d.key) != 0;
        auto it = g_batteryTrack.find(d.key);
        if (it != g_batteryTrack.end()) {
            e.dischargeRatePerHour = it->second.ratePerHour;
            e.etaText = FormatBatteryEta(d.percent, d.charging, it->second.ratePerHour, trLang);
            e.history = it->second.history;
        }
        v.push_back(std::move(e));
    }
    return v;
}

void RedrawAll(bool addTrayIcon = false) {
    tray::ResetNotifState(); // P1-8
    auto ordered = OrderedDevices();
    auto widgetEntries = VisibleWidgetEntries(ordered);
    auto taskbarStatuses = VisibleTaskbarStatuses(ordered);
    auto flyoutEntries = BuildFlyoutEntries(ordered);

    tray::UpdateIcon(g_wnd, addTrayIcon, WM_TRAY, ordered, taskbarStatuses);
    widget::Update(widgetEntries);
    flyout::Update(flyoutEntries);
}

void CheckBatteryNotifications() {
    for (const auto& d : g_devices) {
        if (settings::g_hiddenDevices.count(d.key)) continue;

        bool prevConn = g_prevConnected.count(d.key) ? g_prevConnected[d.key] : d.connected;
        if (g_firstPollDone && settings::g_notifyConnect && prevConn != d.connected) {
            tray::ShowConnectionNotification(g_wnd, d, d.connected);
        }
        g_prevConnected[d.key] = d.connected;

        if (!d.connected || d.percent < 0) continue;

        // Low Battery Warning
        if (d.charging || d.percent > settings::g_lowBatteryThreshold + 5) {
            g_lowWarnedAt.erase(d.key);
        } else if (settings::g_notifyLowBattery && settings::g_lowBatteryThreshold > 0 && !d.asleep &&
                   d.percent <= settings::g_lowBatteryThreshold) {
            int prev = g_lowWarnedAt.count(d.key) ? g_lowWarnedAt[d.key] : 101;
            int bucket = (d.percent <= 5) ? 5 : (d.percent <= 10 ? 10 : settings::g_lowBatteryThreshold);
            if (bucket < prev) {
                g_lowWarnedAt[d.key] = bucket;
                tray::ShowLowBatteryNotification(g_wnd, d);
            }
        }

        // Full Charge (100%) Notification
        if (d.percent < 95) {
            g_fullChargedWarned[d.key] = false;
        } else if (settings::g_notifyFullCharge && d.charging && d.percent >= 100 && !g_fullChargedWarned[d.key]) {
            g_fullChargedWarned[d.key] = true;
            tray::ShowFullChargeNotification(g_wnd, d);
        }
    }
    g_firstPollDone = true;
}

void PollNow() {
    if (g_wakeEvent) SetEvent(g_wakeEvent);
}

DWORD WINAPI PollThreadProc(LPVOID) {
    for (;;) {
        settings::LogDebug(L"Starting PollAllDevices()...");
        auto* res = new std::vector<DeviceStatus>(PollAllDevices());
        settings::LogDebug(L"PollAllDevices() returned %zu devices.", res->size());
        if (!PostMessageW(g_wnd, WM_POLL_DONE, 0, (LPARAM)res)) delete res;

        int waitSec = settings::g_pollIntervalSec.load(std::memory_order_relaxed);
        DWORD waitMs = (DWORD)(std::clamp(waitSec, 5, 300) * 1000);
        HANDLE waits[2] = { g_stopEvent, g_wakeEvent };
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, waitMs);
        if (w == WAIT_OBJECT_0) break;
    }
    return 0;
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    static UINT s_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (m == s_taskbarCreated && s_taskbarCreated != 0) {
        RedrawAll(true);
        return 0;
    }
    switch (m) {
        case WM_CREATE: {
            g_wnd = h;
            RedrawAll(true);
            DEV_BROADCAST_DEVICEINTERFACE_W dbi{};
            dbi.dbcc_size = sizeof(dbi);
            dbi.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
            HidD_GetHidGuid(&dbi.dbcc_classguid);
            g_devNotifyHid = RegisterDeviceNotificationW(h, &dbi, DEVICE_NOTIFY_WINDOW_HANDLE);

            static const GUID kGuidComPort = { 0x86E0D1E0L, 0x8089, 0x11D0, { 0x9C, 0xE4, 0x08, 0x00, 0x3E, 0x30, 0x1F, 0x73 } };
            dbi.dbcc_classguid = kGuidComPort;
            g_devNotifyCom = RegisterDeviceNotificationW(h, &dbi, DEVICE_NOTIFY_WINDOW_HANDLE);
            return 0;
        }
        case WM_POLL_DONE: {
            auto* fresh = reinterpret_cast<std::vector<DeviceStatus>*>(lp);
            ULONGLONG now = GetTickCount64();

            for (auto& d : *fresh) {
                auto it = g_cache.find(d.key);
                if (d.connected && d.percent >= 0 && !d.asleep) {
                    g_cache[d.key] = { d, now };
                } else if (it != g_cache.end() && (now - it->second.seenAt) < (ULONGLONG)d.graceSec * 1000ULL) {
                    std::wstring curHint = d.hint;
                    d = it->second.status;
                    d.asleep = true;
                    if (!curHint.empty()) d.hint = curHint;
                } else if (d.asleep && d.percent < 0) {
                    d.connected = false;
                    d.asleep = false;
                }
            }
            for (const auto& [key, c] : g_cache) {
                if ((now - c.seenAt) >= (ULONGLONG)c.status.graceSec * 1000ULL) continue;
                bool present = false;
                for (const auto& d : *fresh)
                    if (d.key == key) { present = true; break; }
                if (!present) {
                    DeviceStatus d = c.status;
                    d.asleep = true;
                    fresh->push_back(d);
                }
            }

            g_devices = std::move(*fresh);
            delete fresh;

            UpdateAllBatteryTracks();
            RedrawAll(false);
            CheckBatteryNotifications();
            return 0;
        }
        case WM_DEVICECHANGE:
            if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE || wp == DBT_DEVNODES_CHANGED)
                SetTimer(h, TIMER_DEBOUNCE, 1500, nullptr);
            return 0;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
                SetTimer(h, TIMER_DEBOUNCE, 2000, nullptr);
            }
            return TRUE;
        case WM_TIMER:
            if (wp == TIMER_DEBOUNCE) {
                KillTimer(h, TIMER_DEBOUNCE);
                PollNow();
            } else if (wp == tray::kTimerRestoreIcon) {
                auto ordered = OrderedDevices();
                tray::OnBalloonClosed(h, WM_TRAY, ordered, VisibleTaskbarStatuses(ordered));
            }
            return 0;
        case WM_TRAY:
            if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) {
                flyout::Toggle(h);
            } else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
                flyout::Hide();
                tray::ShowContextMenu(h, {}, [] { RedrawAll(false); }, [] { PollNow(); });
            } else if (LOWORD(lp) == NIN_BALLOONTIMEOUT || LOWORD(lp) == NIN_BALLOONUSERCLICK) {
                auto ordered = OrderedDevices();
                tray::OnBalloonClosed(h, WM_TRAY, ordered, VisibleTaskbarStatuses(ordered));
            }
            return 0;
        case WM_WIDGET_CLICK:
            if (wp == 0) {
                flyout::Toggle(h);
            } else {
                flyout::Hide();
                tray::ShowContextMenu(h, {}, [] { RedrawAll(false); }, [] { PollNow(); });
            }
            return 0;
        case WM_SHOW_FLYOUT:
            if (!flyout::IsVisible()) flyout::Toggle(h);
            return 0;
        case WM_FLYOUT_ACTION:
            if (wp == flyout::kActionRefresh) {
                PollNow();
            } else if (wp == flyout::kActionGeneralMenu) {
                tray::ShowContextMenu(h, {}, [] { RedrawAll(false); }, [] { PollNow(); });
            } else if (wp == flyout::kActionSettingsChanged) {
                UpdateAllBatteryTracks();
                RedrawAll(false);
            } else {
                // P0-1: Resolve device key directly from flyout::KeyAt((int)wp)
                std::wstring key = flyout::KeyAt((int)wp);
                if (!key.empty()) {
                    tray::ShowContextMenu(h, key, [] { RedrawAll(false); }, [] { PollNow(); });
                }
            }
            return 0;
        case WM_DESTROY:
            if (g_devNotifyHid) { UnregisterDeviceNotification(g_devNotifyHid); g_devNotifyHid = nullptr; }
            if (g_devNotifyCom) { UnregisterDeviceNotification(g_devNotifyCom); g_devNotifyCom = nullptr; }
            widget::Destroy();
            flyout::Destroy();
            tray::RemoveIcon(h);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

void WriteConsoleOrFile(const std::wstring& text, const wchar_t* fallbackFile) {
    int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), utf8.data(), len, nullptr, nullptr);

    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD wr = 0;
        WriteFile(out, utf8.data(), (DWORD)utf8.size(), &wr, nullptr);
        return;
    }
    if (!fallbackFile) return;
    HANDLE f = CreateFileW(fallbackFile, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(f, utf8.data(), (DWORD)utf8.size(), &wr, nullptr);
        CloseHandle(f);
        MessageBoxW(nullptr, fallbackFile, L"Wireless Battery", MB_OK | MB_ICONINFORMATION);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool flagDump = false, flagList = false, flagHelp = false, flagVersion = false, flagDebug = false;
    if (argv) {
        for (int i = 1; i < argc; i++) {
            std::wstring a = argv[i];
            if (a == L"--dump") flagDump = true;
            else if (a == L"--list") flagList = true;
            else if (a == L"--help" || a == L"-h" || a == L"/?") flagHelp = true;
            else if (a == L"--version" || a == L"-v") flagVersion = true;
            else if (a == L"--debug") flagDebug = true;
        }
        LocalFree(argv);
    }

    if (flagVersion) {
        WriteConsoleOrFile(std::wstring(L"WirelessBattery ") + APP_VERSION_WSTR + L"\n", nullptr);
        return 0;
    }
    if (flagHelp) {
        std::wstring help =
            L"WirelessBattery " APP_VERSION_WSTR L"\n"
            L"Usage: WirelessBattery.exe [options]\n\n"
            L"  --list      Print detected wireless devices and battery percentages\n"
            L"  --dump      Save full HID/COM/Bluetooth diagnostic dump\n"
            L"  --debug     Write diagnostic poll logs to %LOCALAPPDATA%\\WirelessBattery\\debug.log\n"
            L"  --version   Print version\n"
            L"  --help      Show this help message\n";
        WriteConsoleOrFile(help, nullptr);
        return 0;
    }
    if (flagDump) {
        WriteConsoleOrFile(DumpHidCollections(), L"wireless-battery-dump.txt");
        return 0;
    }
    if (flagList) {
        std::wstring s;
        for (const auto& d : PollAllDevices()) {
            wchar_t line[512];
            swprintf_s(line, L"[%s] %s | %s | %d%%%s%s%s\n",
                       d.key.c_str(), d.name.c_str(), d.via.c_str(), d.percent,
                       d.charging ? L" charging" : L"",
                       d.asleep ? L" sleeping" : L"",
                       d.connected ? L"" : L" disconnected");
            s += line;
        }
        if (s.empty()) s = L"(no devices detected)\n";
        WriteConsoleOrFile(s, nullptr);
        return 0;
    }

    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\WirelessBatteryTraySingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kHostClass, nullptr)) {
            PostMessageW(existing, WM_SHOW_FLYOUT, 0, 0);
        }
        if (single) CloseHandle(single);
        return 0;
    }

    using SetDpiCtxFn = BOOL(WINAPI*)(HANDLE);
    if (auto fn = reinterpret_cast<SetDpiCtxFn>(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")))) {
        fn(reinterpret_cast<HANDLE>((LONG_PTR)-4)); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    }

    settings::EnableDebugLog(flagDebug);
    LoadLanguage();
    settings::Load();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kHostClass;
    RegisterClassExW(&wc);

    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"Wireless Battery", 0, 0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    widget::Create(hInst, wnd, WM_WIDGET_CLICK);
    flyout::Create(hInst, wnd, WM_TRAY, WM_FLYOUT_ACTION);

    g_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_stopEvent = CreateEventW(nullptr, TRUE,  FALSE, nullptr);
    g_pollThread = CreateThread(nullptr, 0, PollThreadProc, nullptr, 0, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    SetEvent(g_stopEvent);
    if (g_pollThread) {
        WaitForSingleObject(g_pollThread, 3000);
        CloseHandle(g_pollThread);
    }
    CloseHandle(g_wakeEvent);
    CloseHandle(g_stopEvent);
    if (single) CloseHandle(single);
    return 0;
}
