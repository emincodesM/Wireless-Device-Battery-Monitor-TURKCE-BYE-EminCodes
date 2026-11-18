#include "tray.h"
#include "brands.h"
#include "flyout.h"
#include "i18n.h"
#include "settings.h"
#include "widget.h"

#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cstring>
#include <cwchar>

namespace tray {
namespace {

enum {
    ID_SHOW_FLYOUT   = 999,
    ID_REFRESH       = 1001,
    ID_STARTUP       = 1002,
    ID_SHOW_NUM      = 1003,
    ID_DUMP          = 1004,
    ID_COLOR_PCT     = 1005,
    ID_SHORT_NAMES   = 1006,
    ID_SHOW_OFF      = 1007,
    ID_TRAY_BARS     = 1008,
    ID_NOTIFY_LOW    = 1020,
    ID_NOTIFY_FULL   = 1021,
    ID_NOTIFY_CONN   = 1022,
    ID_EXIT          = 1009,
    ID_POLL_5        = 1105,
    ID_POLL_10       = 1110,
    ID_POLL_15       = 1115,
    ID_POLL_30       = 1130,
    ID_POLL_60       = 1160,
    ID_WARN_OFF      = 1200,
    ID_WARN_10       = 1210,
    ID_WARN_15       = 1215,
    ID_WARN_20       = 1220,
    ID_WARN_25       = 1225,
    ID_WARN_30       = 1230,
    ID_LANG_SYSTEM   = 1400,
    ID_LANG_BASE     = 1401,
    ID_DEV_BASE      = 2000,
    ID_UP_BASE       = 2200,
    ID_DOWN_BASE     = 2400,
    ID_COLOR_BASE    = 3000,
};
constexpr int kColorStride = 32;

HICON g_icon = nullptr;
bool  g_notifIcon = false;

HICON CreateBitmapIcon(HDC screen, int sz, DWORD* bits, HBITMAP bmp) {
    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = bmp;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    (void)screen;
    (void)bits;
    return icon;
}

HICON MakeBarsIcon(const std::vector<DeviceStatus>& devs) {
    int count = std::min((int)devs.size(), 4);
    if (count <= 0) return nullptr;
    int sz = GetSystemMetrics(SM_CXSMICON);
    if (sz <= 0) sz = 16;

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sz;
    bi.bmiHeader.biHeight = -sz;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    DWORD* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    memset(bits, 0, sz * sz * 4);

    auto setPx = [&](int x, int y, DWORD argb) {
        if (x >= 0 && x < sz && y >= 0 && y < sz) bits[y * sz + x] = argb;
    };
    auto fillRect = [&](int x0, int y0, int w, int h, DWORD argb) {
        for (int y = y0; y < y0 + h; y++)
            for (int x = x0; x < x0 + w; x++) setPx(x, y, argb);
    };

    int gap = std::max(1, sz / 16);
    int rowH = (sz - gap * (count - 1)) / count;
    for (int i = 0; i < count; i++) {
        const auto& d = devs[i];
        int y = i * (rowH + gap);
        fillRect(0, y, sz, rowH, 0x55FFFFFF);
        if (d.connected && d.percent >= 0) {
            int fillW = std::max(1, (sz * std::clamp(d.percent, 0, 100)) / 100);
            DWORD col = 0xFF2ECC71;
            if (!d.charging) {
                if (d.percent <= 10)      col = 0xFFE74C3C;
                else if (d.percent <= 25) col = 0xFFF39C12;
            }
            fillRect(0, y, fillW, rowH, col);
        }
    }

    SelectObject(dc, oldBmp);
    HICON icon = CreateBitmapIcon(screen, sz, bits, bmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return icon;
}

HICON MakeTrayIcon(int percent, bool charging, bool anyDevice, const std::vector<DeviceStatus>& barDevs) {
    if (widget::TrayBars() && !barDevs.empty()) {
        if (HICON bars = MakeBarsIcon(barDevs)) return bars;
    }

    int sz = GetSystemMetrics(SM_CXSMICON);
    if (sz <= 0) sz = 16;

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sz;
    bi.bmiHeader.biHeight = -sz;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    DWORD* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);

    COLORREF bg;
    if (!anyDevice || percent < 0) bg = RGB(95, 99, 104);
    else if (charging)             bg = RGB(39, 174, 96);
    else if (percent <= 15)        bg = RGB(217, 48, 37);
    else if (percent <= 30)        bg = RGB(227, 116, 0);
    else                           bg = RGB(30, 142, 62);

    RECT r{ 0, 0, sz, sz };
    HBRUSH br = CreateSolidBrush(bg);
    FillRect(dc, &r, br);
    DeleteObject(br);

    wchar_t txt[8];
    if (!anyDevice || percent < 0) wcscpy_s(txt, L"--");
    else if (percent >= 100)       wcscpy_s(txt, L"99");
    else                           swprintf_s(txt, L"%d", percent);

    int fsize = (wcslen(txt) >= 2) ? (sz * 68 / 100) : (sz * 78 / 100);
    HFONT font = CreateFontW(-fsize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, txt, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
    DeleteObject(font);

    for (int i = 0; i < sz * sz; i++) bits[i] |= 0xFF000000;
    const int k = std::max(1, sz / 8);
    auto clearPx = [&](int x, int y) { bits[y * sz + x] = 0; };
    for (int d = 0; d < k; d++) {
        clearPx(d, 0); clearPx(0, d);
        clearPx(sz - 1 - d, 0); clearPx(sz - 1, d);
        clearPx(d, sz - 1); clearPx(0, sz - 1 - d);
        clearPx(sz - 1 - d, sz - 1); clearPx(sz - 1, sz - 1 - d);
    }

    SelectObject(dc, oldBmp);
    HICON icon = CreateBitmapIcon(screen, sz, bits, bmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return icon;
}

std::wstring BuildTooltip(const std::vector<DeviceStatus>& allConnected) {
    if (allConnected.empty()) {
        return std::wstring(T(L"Wireless Battery")) + L"\r\n" + T(L"No wireless devices detected");
    }
    std::wstring t;
    for (const auto& d : allConnected) {
        if (!t.empty()) t += L"\r\n";
        t += TranslateDeviceName(d.name) + L": ";
        if (!d.connected)    t += T(L"not connected");
        else if (d.percent >= 0) {
            if (!d.levelText.empty()) t += TranslateLevel(d.levelText);
            else t += std::to_wstring(d.percent) + L"%";
            if (d.charging)      t += std::wstring(L" (") + T(L"charging") + L")";
            else if (d.asleep)   t += std::wstring(L" (") + T(L"sleeping") + L")";
            if (!d.detail.empty()) t += L" (" + TranslateDetail(d.detail) + L")";
        } else               t += T(L"no battery info");
        if (!d.via.empty())  t += L" [" + TranslateVia(d.via) + L"]";
    }
    if (t.size() >= 127) t.resize(126);
    return t;
}

void ShowBalloon(HWND wnd, const wchar_t* title, const std::wstring& msg, DWORD infoFlags) {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = wnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO | NIF_ICON;
    nid.hIcon = g_icon;
    nid.dwInfoFlags = infoFlags | NIIF_RESPECT_QUIET_TIME;
    wcsncpy_s(nid.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(nid.szInfo, msg.c_str(), _TRUNCATE);
    g_notifIcon = true;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    SetTimer(wnd, kTimerRestoreIcon, 30000, nullptr); // 30s fallback timer (P1-8)
}

void WriteDiagnosticDump(HWND wnd) {
    wchar_t desk[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desk))) return;
    std::wstring path = std::wstring(desk) + L"\\wireless-battery-dump.txt";
    std::wstring text = DumpHidCollections();
    int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), utf8.data(), len, nullptr, nullptr);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(f, utf8.data(), (DWORD)utf8.size(), &wr, nullptr);
        CloseHandle(f);
        ShellExecuteW(wnd, L"open", L"notepad.exe", path.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

}  // namespace

void ResetNotifState() {
    g_notifIcon = false;
}

void UpdateIcon(HWND wnd, bool add, UINT callbackMsg,
                const std::vector<DeviceStatus>& allConnected,
                const std::vector<DeviceStatus>& taskbarVisible) {
    int lowest = -1;
    bool charging = false;
    bool any = false;
    for (const auto& d : taskbarVisible) {
        if (!d.connected) continue;
        any = true;
        if (d.percent >= 0 && (lowest < 0 || d.percent < lowest)) {
            lowest = d.percent;
            charging = d.charging;
        }
    }

    HICON newIcon = MakeTrayIcon(lowest, charging, any, taskbarVisible);
    std::wstring tip = BuildTooltip(allConnected);

    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = wnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_TIP | (g_notifIcon ? 0 : NIF_ICON);
    nid.uCallbackMessage = callbackMsg;
    nid.hIcon = newIcon;
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);

    if (add) {
        Shell_NotifyIconW(NIM_ADD, &nid);
    } else {
        if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) {
            // Recover automatically if explorer dropped the icon (P3-4).
            nid.uFlags = NIF_MESSAGE | NIF_TIP | NIF_ICON;
            Shell_NotifyIconW(NIM_ADD, &nid);
        }
    }

    if (g_icon) DestroyIcon(g_icon);
    g_icon = newIcon;
}

void RemoveIcon(HWND wnd) {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = wnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (g_icon) {
        DestroyIcon(g_icon);
        g_icon = nullptr;
    }
}

void OnBalloonClosed(HWND wnd, UINT callbackMsg,
                     const std::vector<DeviceStatus>& allConnected,
                     const std::vector<DeviceStatus>& taskbarVisible) {
    KillTimer(wnd, kTimerRestoreIcon);
    if (g_notifIcon) {
        g_notifIcon = false;
        UpdateIcon(wnd, false, callbackMsg, allConnected, taskbarVisible);
    }
}

void ShowLowBatteryNotification(HWND wnd, const DeviceStatus& d) {
    wchar_t body[256] = {};
    std::wstring dispName = TranslateDeviceName(d.name);
    if (IsTurkish()) {
        swprintf_s(body, L"%s pil seviyesi %%%d d\x00FCzeyine d\x00FC\x015Ft\x00FC. L\x00FCtfen \x015Farja tak\x0131n.",
                   dispName.c_str(), d.percent);
    } else {
        swprintf_s(body, L"%s is at %d%%", dispName.c_str(), d.percent);
    }
    ShowBalloon(wnd, T(L"Low battery"), body, NIIF_WARNING);
}

void ShowFullChargeNotification(HWND wnd, const DeviceStatus& d) {
    wchar_t body[256] = {};
    std::wstring dispName = TranslateDeviceName(d.name);
    if (IsTurkish()) {
        swprintf_s(body, L"%s tamamen \x015Farj oldu (%%100). Kabloyu \x00E7\x0131karabilirsiniz.", dispName.c_str());
    } else {
        swprintf_s(body, L"%s is fully charged (100%%).", dispName.c_str());
    }
    ShowBalloon(wnd, T(L"Battery Charged"), body, NIIF_INFO);
}

void ShowConnectionNotification(HWND wnd, const DeviceStatus& d, bool connected) {
    wchar_t body[256] = {};
    std::wstring dispName = TranslateDeviceName(d.name);
    if (connected) {
        if (d.percent >= 0) {
            if (IsTurkish())
                swprintf_s(body, L"%s ba\x011Fland\x0131 (Pil: %%%d).", dispName.c_str(), d.percent);
            else
                swprintf_s(body, L"%s connected (Battery: %d%%).", dispName.c_str(), d.percent);
        } else {
            if (IsTurkish())
                swprintf_s(body, L"%s ba\x011Fland\x0131.", dispName.c_str());
            else
                swprintf_s(body, L"%s connected.", dispName.c_str());
        }
    } else {
        if (IsTurkish())
            swprintf_s(body, L"%s ba\x011Flant\x0131s\x0131 kesildi.", dispName.c_str());
        else
            swprintf_s(body, L"%s disconnected.", dispName.c_str());
    }
    ShowBalloon(wnd, connected ? T(L"Device Connected") : T(L"Device Disconnected"), body, NIIF_INFO);
}

void ShowContextMenu(HWND wnd, const std::wstring& onlyKey,
                     const std::function<void()>& onRedraw,
                     const std::function<void()>& onPollNow) {
    HMENU m = CreatePopupMenu();
    // P0-1: Always use flyout::OrderedEntries() so indices match the Flyout card list.
    const auto& ordered = flyout::OrderedEntries();

    auto addDeviceControls = [&](HMENU dst, size_t i) {
        const auto& d = ordered[i].status;
        bool shown = !settings::g_hiddenDevices.count(d.key);
        AppendMenuW(dst, MF_STRING | (shown ? MF_CHECKED : 0), ID_DEV_BASE + i, T(L"Show on taskbar"));
        if (ordered.size() > 1) {
            AppendMenuW(dst, MF_STRING | (i == 0 ? MF_GRAYED : 0),                  ID_UP_BASE + i,   T(L"Move up"));
            AppendMenuW(dst, MF_STRING | (i + 1 == ordered.size() ? MF_GRAYED : 0), ID_DOWN_BASE + i, T(L"Move down"));
        }

        HMENU cm = CreatePopupMenu();
        int cur = widget::AccentPref(d.key);
        int base = ID_COLOR_BASE + (int)i * kColorStride;
        std::wstring autoLabel = T(L"Automatic");
        Accent det = BrandOf(d);
        if (det != Accent::None) {
            autoLabel += std::wstring(L" (") + StyleOf(det).name + L")";
        }
        AppendMenuW(cm, MF_STRING | (cur < 0  ? MF_CHECKED : 0), base,     autoLabel.c_str());
        AppendMenuW(cm, MF_STRING | (cur == 0 ? MF_CHECKED : 0), base + 1, T(L"None (white / black)"));
        AppendMenuW(cm, MF_SEPARATOR, 0, nullptr);
        int nBrands = 0;
        const BrandStyle* brands = AllBrands(nBrands);
        for (int b = 1; b < nBrands; b++) {
            int id = base + 1 + (int)brands[b].accent;
            AppendMenuW(cm, MF_STRING | (cur == (int)brands[b].accent ? MF_CHECKED : 0), id, brands[b].name);
        }
        AppendMenuW(dst, MF_POPUP, (UINT_PTR)cm, T(L"Color"));
    };

    auto statusText = [](const DeviceStatus& d) {
        std::wstring line = TranslateDeviceName(d.name) + L"  \x2014  ";
        if (!d.connected)       line += T(L"not connected");
        else if (d.percent >= 0) {
            if (!d.levelText.empty()) line += TranslateLevel(d.levelText);
            else line += std::to_wstring(d.percent) + L"%";
            if (d.charging)    line += std::wstring(L" (") + T(L"charging") + L")";
            else if (d.asleep) line += std::wstring(L" (") + T(L"sleeping") + L")";
        } else                  line += T(L"no battery info");
        return line;
    };

    if (!onlyKey.empty()) {
        for (size_t i = 0; i < ordered.size(); i++) {
            if (ordered[i].status.key != onlyKey) continue;
            AppendMenuW(m, MF_STRING | MF_GRAYED, 0, statusText(ordered[i].status).c_str());
            if (!ordered[i].status.detail.empty())
                AppendMenuW(m, MF_STRING | MF_GRAYED, 0, TranslateDetail(ordered[i].status.detail).c_str());
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            addDeviceControls(m, i);
            break;
        }
    } else {
        AppendMenuW(m, MF_STRING, ID_SHOW_FLYOUT, T(L"Show panel (Flyout)"));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

        if (ordered.empty()) {
            AppendMenuW(m, MF_STRING | MF_GRAYED, 0, T(L"No wireless devices detected"));
        } else {
            for (size_t i = 0; i < ordered.size(); i++) {
                const auto& d = ordered[i].status;
                bool shown = !settings::g_hiddenDevices.count(d.key);
                std::wstring line = statusText(d);
                if (!shown) line += std::wstring(L"  [") + T(L"hidden") + L"]";
                HMENU sub = CreatePopupMenu();
                if (!d.via.empty()) {
                    AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, TranslateVia(d.via).c_str());
                    if (!d.detail.empty())
                        AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, TranslateDetail(d.detail).c_str());
                    AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
                }
                addDeviceControls(sub, i);
                AppendMenuW(m, MF_POPUP | (shown ? MF_CHECKED : 0), (UINT_PTR)sub, line.c_str());
            }
        }
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

        // Taskbar Widget Submenu
        HMENU tbMenu = CreatePopupMenu();
        AppendMenuW(tbMenu, MF_STRING | (widget::ShowNumbers() ? MF_CHECKED : 0), ID_SHOW_NUM,    T(L"Show percentage"));
        AppendMenuW(tbMenu, MF_STRING | (widget::ColorPct()    ? MF_CHECKED : 0), ID_COLOR_PCT,   T(L"Colored percentages"));
        AppendMenuW(tbMenu, MF_STRING | (widget::ShortNames()  ? MF_CHECKED : 0), ID_SHORT_NAMES, T(L"Short names"));
        AppendMenuW(tbMenu, MF_STRING | (widget::ShowOff()     ? MF_CHECKED : 0), ID_SHOW_OFF,    T(L"Show disconnected"));
        AppendMenuW(m, MF_POPUP, (UINT_PTR)tbMenu, T(L"Taskbar Widget"));

        // Tray Icon Submenu
        HMENU trayMenu = CreatePopupMenu();
        AppendMenuW(trayMenu, MF_STRING | (widget::TrayBars() ? MF_CHECKED : 0), ID_TRAY_BARS, T(L"Battery bars"));
        AppendMenuW(m, MF_POPUP, (UINT_PTR)trayMenu, T(L"Tray icon"));

        // Poll Interval Submenu
        int curPoll = settings::g_pollIntervalSec.load(std::memory_order_relaxed);
        HMENU pollMenu = CreatePopupMenu();
        AppendMenuW(pollMenu, MF_STRING | (curPoll == 5  ? MF_CHECKED : 0), ID_POLL_5,  IsTurkish() ? L"5 saniye (H\x0131zl\x0131)" : L"5s");
        AppendMenuW(pollMenu, MF_STRING | (curPoll == 10 ? MF_CHECKED : 0), ID_POLL_10, IsTurkish() ? L"10 saniye" : L"10s");
        AppendMenuW(pollMenu, MF_STRING | (curPoll == 15 ? MF_CHECKED : 0), ID_POLL_15, IsTurkish() ? L"15 saniye (Varsay\x0131lan)" : L"15s");
        AppendMenuW(pollMenu, MF_STRING | (curPoll == 30 ? MF_CHECKED : 0), ID_POLL_30, IsTurkish() ? L"30 saniye" : L"30s");
        AppendMenuW(pollMenu, MF_STRING | (curPoll == 60 ? MF_CHECKED : 0), ID_POLL_60, IsTurkish() ? L"60 saniye (D\x00FC\x015F\x00FCk g\x00FC\x00E7)" : L"60s");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)pollMenu, T(L"Update interval"));

        // Low Battery Warning Submenu
        HMENU warnMenu = CreatePopupMenu();
        AppendMenuW(warnMenu, MF_STRING | (settings::g_lowBatteryThreshold == 0  ? MF_CHECKED : 0), ID_WARN_OFF, T(L"Off"));
        AppendMenuW(warnMenu, MF_STRING | (settings::g_lowBatteryThreshold == 10 ? MF_CHECKED : 0), ID_WARN_10,  L"%10");
        AppendMenuW(warnMenu, MF_STRING | (settings::g_lowBatteryThreshold == 15 ? MF_CHECKED : 0), ID_WARN_15,  L"%15");
        AppendMenuW(warnMenu, MF_STRING | (settings::g_lowBatteryThreshold == 20 ? MF_CHECKED : 0), ID_WARN_20,  L"%20");
        AppendMenuW(warnMenu, MF_STRING | (settings::g_lowBatteryThreshold == 25 ? MF_CHECKED : 0), ID_WARN_25,  L"%25");
        AppendMenuW(warnMenu, MF_STRING | (settings::g_lowBatteryThreshold == 30 ? MF_CHECKED : 0), ID_WARN_30,  L"%30");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)warnMenu, T(L"Low battery warning"));

        // Notifications Submenu
        HMENU notifMenu = CreatePopupMenu();
        AppendMenuW(notifMenu, MF_STRING | (settings::g_notifyLowBattery ? MF_CHECKED : 0), ID_NOTIFY_LOW,  T(L"Low battery notification"));
        AppendMenuW(notifMenu, MF_STRING | (settings::g_notifyFullCharge ? MF_CHECKED : 0), ID_NOTIFY_FULL, T(L"Full charge notification"));
        AppendMenuW(notifMenu, MF_STRING | (settings::g_notifyConnect    ? MF_CHECKED : 0), ID_NOTIFY_CONN, T(L"Connection notifications"));
        AppendMenuW(m, MF_POPUP, (UINT_PTR)notifMenu, T(L"Notifications"));

        // Language Submenu
        HMENU langMenu = CreatePopupMenu();
        const auto& langs = AllLanguages();
        const std::wstring& curLang = CurrentLanguage();
        for (size_t i = 0; i < langs.size(); i++) {
            AppendMenuW(langMenu, MF_STRING | (curLang == langs[i].code ? MF_CHECKED : 0), ID_LANG_BASE + i, langs[i].label);
        }
        AppendMenuW(m, MF_POPUP, (UINT_PTR)langMenu, T(L"Language"));

        AppendMenuW(m, MF_STRING | (settings::IsAutoStartEnabled() ? MF_CHECKED : 0), ID_STARTUP, T(L"Run at Windows startup"));
        AppendMenuW(m, MF_STRING, ID_REFRESH, T(L"Refresh now"));
        AppendMenuW(m, MF_STRING, ID_DUMP,    T(L"Device diagnostic report (--dump)"));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, ID_EXIT,    T(L"Exit"));
    }

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(wnd);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, wnd, nullptr);
    DestroyMenu(m);

    if (cmd == ID_SHOW_FLYOUT) {
        flyout::Toggle(wnd);
    } else if (cmd == ID_REFRESH) {
        if (onPollNow) onPollNow();
    } else if (cmd == ID_SHOW_NUM) {
        widget::SetShowNumbers(!widget::ShowNumbers());
    } else if (cmd == ID_COLOR_PCT) {
        widget::SetColorPct(!widget::ColorPct());
    } else if (cmd == ID_SHORT_NAMES) {
        widget::SetShortNames(!widget::ShortNames());
        if (onRedraw) onRedraw();
    } else if (cmd == ID_SHOW_OFF) {
        widget::SetShowOff(!widget::ShowOff());
        if (onRedraw) onRedraw();
    } else if (cmd == ID_TRAY_BARS) {
        widget::SetTrayBars(!widget::TrayBars());
        if (onRedraw) onRedraw();
    } else if (cmd == ID_STARTUP) {
        settings::SetAutoStart(!settings::IsAutoStartEnabled());
    } else if (cmd == ID_POLL_5 || cmd == ID_POLL_10 || cmd == ID_POLL_15 || cmd == ID_POLL_30 || cmd == ID_POLL_60) {
        settings::g_pollIntervalSec.store((int)(cmd - 1100), std::memory_order_relaxed);
        settings::Save();
        if (onPollNow) onPollNow();
    } else if (cmd == ID_WARN_OFF || cmd == ID_WARN_10 || cmd == ID_WARN_15 || cmd == ID_WARN_20 || cmd == ID_WARN_25 || cmd == ID_WARN_30) {
        settings::g_lowBatteryThreshold = (int)(cmd - 1200);
        settings::Save();
    } else if (cmd == ID_NOTIFY_LOW) {
        settings::g_notifyLowBattery = !settings::g_notifyLowBattery;
        settings::Save();
    } else if (cmd == ID_NOTIFY_FULL) {
        settings::g_notifyFullCharge = !settings::g_notifyFullCharge;
        settings::Save();
    } else if (cmd == ID_NOTIFY_CONN) {
        settings::g_notifyConnect = !settings::g_notifyConnect;
        settings::Save();
    } else if (cmd >= ID_LANG_BASE && cmd < ID_LANG_BASE + AllLanguages().size()) {
        SetLanguage(AllLanguages()[cmd - ID_LANG_BASE].code);
        if (onRedraw) onRedraw();
    } else if (cmd == ID_DUMP) {
        WriteDiagnosticDump(wnd);
    } else if (cmd == ID_EXIT) {
        PostQuitMessage(0);
    } else if (cmd >= ID_DEV_BASE && cmd < ID_DEV_BASE + ordered.size()) {
        const auto& key = ordered[cmd - ID_DEV_BASE].status.key;
        if (!settings::g_hiddenDevices.erase(key)) settings::g_hiddenDevices.insert(key);
        settings::Save();
        if (onRedraw) onRedraw();
    } else if (cmd >= ID_UP_BASE && cmd < ID_DOWN_BASE + ordered.size()) {
        bool up = cmd < ID_DOWN_BASE;
        size_t idx = up ? (cmd - ID_UP_BASE) : (cmd - ID_DOWN_BASE);
        size_t swapWith = up ? idx - 1 : idx + 1;
        if (swapWith < ordered.size()) {
            settings::g_deviceOrder.clear();
            for (const auto& e : ordered) settings::g_deviceOrder.push_back(e.status.key);
            std::swap(settings::g_deviceOrder[idx], settings::g_deviceOrder[swapWith]);
            settings::Save();
            if (onRedraw) onRedraw();
        }
    } else if (cmd >= ID_COLOR_BASE) {
        size_t idx = (cmd - ID_COLOR_BASE) / kColorStride;
        int choice = (cmd - ID_COLOR_BASE) % kColorStride;
        if (idx < ordered.size()) {
            int pref = (choice == 0) ? -1 : (choice == 1 ? 0 : choice - 1);
            widget::SetAccentPref(ordered[idx].status.key, pref);
            if (onRedraw) onRedraw();
        }
    }
}

}  // namespace tray
