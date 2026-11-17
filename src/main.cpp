// Kablosuz Pil - ASUS ROG Harpe Ace Aim Lab Edition ve ATK N9 Ultra için özel pil izleme uygulaması.
#include <windows.h>
#include <shellapi.h>
#include <dbt.h>
#include <initguid.h>
#include <hidclass.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "devices.h"
#include "flyout.h"
#include "widget.h"

namespace {

// Standard Windows GUID for COM port device interface arrival/removal notifications
const GUID kGuidDevInterfaceComPort = { 0x86E0D1E0, 0x8089, 0x11D0, { 0x9C, 0xE4, 0x08, 0x00, 0x3E, 0x30, 0x1F, 0x73 } };

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_RESULTS = WM_APP + 2;
constexpr UINT WM_WIDGET_CLICK = WM_APP + 3;
constexpr UINT WM_FLYOUT_ACTION = WM_APP + 4;

constexpr UINT_PTR TIMER_DEBOUNCE = 1;
constexpr UINT_PTR TIMER_WIDGET = 2;
constexpr UINT_PTR TIMER_NOTIF_CLEANUP = 3;

constexpr UINT PLACEHOLDER_ID = 1;
constexpr UINT NOTIF_ICON_ID = 2;

enum MenuId : UINT {
    ID_OPEN_FLYOUT = 90,
    ID_REFRESH = 100,
    ID_AUTOSTART,
    ID_HIDE_LAST_KNOWN,
    ID_EXIT,
    ID_MODE_WIDGET,
    ID_MODE_TRAY,
    ID_POLL_30S = 200,
    ID_POLL_60S,
    ID_POLL_300S,
    ID_LOW_10 = 210,
    ID_LOW_15,
    ID_LOW_20,
    ID_DEVICE_FIRST = 1000
};

const wchar_t* kAppName = L"Kablosuz Pil";
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunValue = L"WirelessBattery";
const wchar_t* kSettingsKey = L"Software\\WirelessBattery";

HINSTANCE g_inst;
HWND g_hwnd;
HANDLE g_refreshEvt, g_stopEvt, g_worker;
UINT g_taskbarCreatedMsg;
bool g_lightTaskbar;
bool g_wantWidget = true;      // user's choice: taskbar widget (default) or tray icons
bool g_useWidget = false;      // what is actually shown right now
bool g_hideLastKnown = false;  // hide (last known) / asleep devices from taskbar and tray
int  g_pollIntervalSec = 60;   // configurable: 30, 60, or 300 seconds
int  g_lowBatteryPct = 15;     // configurable: 10%, 15%, or 20%

struct TrayEntry {
    UINT uid = 0;
    DeviceStatus st;
    int lastPct = -1;          // last known percentage, kept while the device sleeps
    int prevPct = -1;          // previous poll's valid percentage
    ULONGLONG lastSeen = 0;
    ULONGLONG sessionStartTick = 0;
    int sessionStartPct = -1;  // for calculating discharge rate (%/hr) and ETA
    bool added = false;
    bool lowWarned = false;
    bool fullWarned = false;
    std::wstring iconSig;      // what the current icon shows, to avoid needless redraws
    HICON icon = nullptr;
};
std::map<std::wstring, TrayEntry> g_entries;
bool g_placeholder = false;
HICON g_placeholderIcon = nullptr;
bool g_notifIconAdded = false;
HICON g_notifIcon = nullptr;

// ---- helpers ----------------------------------------------------------------------------------

UINT StableId(const std::wstring& key) {
    UINT h = 2166136261u;
    for (wchar_t c : key) { h ^= c; h *= 16777619u; }
    return (h & 0x7FFFFFFF) | 0x100;
}

bool ReadLightTaskbar() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return v != 0;
    return false;
}

std::wstring ExePath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

// Devices the user chose to hide (by key), stored as a REG_MULTI_SZ.
std::set<std::wstring> g_hidden;

void LoadHidden() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"HiddenDevices", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || !size)
        return;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"HiddenDevices", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS)
        return;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) g_hidden.insert(p);
}

void SaveHidden() {
    std::wstring multi;
    for (const auto& k : g_hidden) { multi += k; multi.push_back(L'\0'); }
    multi.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"HiddenDevices", REG_MULTI_SZ, multi.data(), (DWORD)(multi.size() * sizeof(wchar_t)));
}

bool IsHidden(const std::wstring& key) { return g_hidden.count(key) != 0; }

bool IsEntryHidden(const TrayEntry& e) {
    return IsHidden(e.st.key) || (g_hideLastKnown && (e.st.asleep || (e.st.percent < 0 && e.lastPct >= 0)));
}

DWORD LoadDwordSetting(const wchar_t* name, DWORD defaultVal) {
    DWORD v = defaultVal, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v;
}

void SaveDwordSetting(const wchar_t* name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &v, sizeof(v));
}

bool LoadBoolSetting(const wchar_t* name, bool defaultVal) {
    return LoadDwordSetting(name, defaultVal ? 1 : 0) != 0;
}

void SaveBoolSetting(const wchar_t* name, bool on) {
    SaveDwordSetting(name, on ? 1 : 0);
}

bool LoadWidgetMode() { return LoadBoolSetting(L"TaskbarWidget", true); }
void SaveWidgetMode(bool on) { SaveBoolSetting(L"TaskbarWidget", on); }
bool LoadHideLastKnown() { return LoadBoolSetting(L"HideLastKnownDevices", false); }
void SaveHideLastKnown(bool on) { SaveBoolSetting(L"HideLastKnownDevices", on); }

void LoadUserPreferences() {
    int poll = (int)LoadDwordSetting(L"PollIntervalSec", 60);
    if (poll != 30 && poll != 60 && poll != 300) poll = 60;
    g_pollIntervalSec = poll;

    int low = (int)LoadDwordSetting(L"LowBatteryPct", 15);
    if (low != 10 && low != 15 && low != 20) low = 15;
    g_lowBatteryPct = low;
}

bool AutostartEnabled() {
    wchar_t buf[MAX_PATH * 2];
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return false;
    return _wcsicmp(buf, (L"\"" + ExePath() + L"\"").c_str()) == 0;
}

void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = L"\"" + ExePath() + L"\"";
        RegSetValueExW(k, kRunValue, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunValue);
    }
    RegCloseKey(k);
}

// ---- icon rendering ---------------------------------------------------------------------------

struct Rgb { BYTE r, g, b; };

HICON RenderIcon(const std::wstring& text, int barPct, Rgb textColor, Rgb barColor, bool dim) {
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    UINT dpi = taskbar ? GetDpiForWindow(taskbar) : 0;
    const int S = GetSystemMetricsForDpi(SM_CXSMICON, dpi ? dpi : GetDpiForSystem());
    const int barH = std::max(2, S / 8);
    const int textH = S - barH - 1;

    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    const int W = S * 3, H = S * 2;
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    void* sbits = nullptr;
    HBITMAP scratch = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &sbits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, scratch);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    auto* sp = static_cast<DWORD*>(sbits);

    int bx0 = 0, by0 = 0, bx1 = -1, by1 = -1;
    for (int fh = textH * 2; fh >= 6; fh--) {
        HFONT f = CreateFontW(-fh, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldF = SelectObject(dc, f);
        SetTextCharacterExtra(dc, text.size() >= 3 ? -1 : 0);
        memset(sbits, 0, (size_t)W * H * 4);
        TextOutW(dc, 2, 0, text.c_str(), (int)text.size());
        GdiFlush();
        SelectObject(dc, oldF);
        DeleteObject(f);
        bx0 = W; by0 = H; bx1 = -1; by1 = -1;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (sp[y * W + x] & 0xFF) { bx0 = std::min(bx0, x); bx1 = std::max(bx1, x); by0 = std::min(by0, y); by1 = std::max(by1, y); }
        if (bx1 < 0) break;
        if (bx1 - bx0 + 1 <= S && by1 - by0 + 1 <= textH) break;
    }

    bi.bmiHeader.biWidth = S;
    bi.bmiHeader.biHeight = -S;
    void* ibits = nullptr;
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &ibits, nullptr, 0);
    auto* ip = static_cast<DWORD*>(ibits);
    memset(ip, 0, (size_t)S * S * 4);
    const int alphaScale = dim ? 140 : 255;
    auto put = [&](int x, int y, Rgb c, int a) {
        if (x < 0 || y < 0 || x >= S || y >= S || a <= 0) return;
        a = a * alphaScale / 255;
        ip[y * S + x] = ((DWORD)a << 24) | ((DWORD)c.r << 16) | ((DWORD)c.g << 8) | c.b;
    };
    if (bx1 >= 0) {
        int gw = bx1 - bx0 + 1, gh = by1 - by0 + 1;
        int ox = (S - gw) / 2, oy = (textH - gh) / 2;
        for (int y = 0; y < gh; y++)
            for (int x = 0; x < gw; x++)
                put(ox + x, oy + y, textColor, sp[(by0 + y) * W + (bx0 + x)] & 0xFF);
    }
    int fill = barPct < 0 ? 0 : std::max(barPct > 0 ? 1 : 0, (barPct * S + 50) / 100);
    for (int y = S - barH; y < S; y++)
        for (int x = 0; x < S; x++) {
            if (x < fill) put(x, y, barColor, 255);
            else put(x, y, g_lightTaskbar ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }, 70);
        }

    SelectObject(dc, oldBmp);
    DeleteObject(scratch);
    DeleteDC(dc);

    HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr);
    {
        HDC mdc = CreateCompatibleDC(nullptr);
        HGDIOBJ o = SelectObject(mdc, mask);
        PatBlt(mdc, 0, 0, S, S, BLACKNESS);
        SelectObject(mdc, o);
        DeleteDC(mdc);
    }
    ICONINFO ii{ TRUE, 0, 0, mask, color };
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

Rgb Foreground() { return g_lightTaskbar ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }; }

HICON IconFor(int pct, bool charging, bool dim) {
    Rgb fg = Foreground();
    Rgb bar = charging               ? Rgb{ 0, 170, 255 }
            : pct <= g_lowBatteryPct ? Rgb{ 232, 32, 48 }
            : pct <= 30              ? Rgb{ 255, 150, 0 }
                                     : Rgb{ 30, 190, 90 };
    Rgb text = (!charging && pct >= 0 && pct <= g_lowBatteryPct) ? Rgb{ 240, 50, 60 } : fg;
    std::wstring t = pct < 0 ? L"?" : std::to_wstring(pct);
    return RenderIcon(t, pct, text, bar, dim);
}

// ---- tray & notification management -----------------------------------------------------------

void CopyTip(wchar_t* dst, size_t cap, const std::wstring& s) {
    wcsncpy(dst, s.c_str(), cap - 1);
    dst[cap - 1] = 0;
}

std::wstring StatusText(const TrayEntry& e) {
    const auto& s = e.st;
    if (!s.connected) return L"kapalı / bağlı değil";
    std::wstring t;
    if (!s.levelText.empty() && s.percent >= 0) t = s.levelText;
    else if (s.percent >= 0) t = L"%" + std::to_wstring(s.percent);
    else if (e.lastPct >= 0) t = L"%" + std::to_wstring(e.lastPct) + L" (son bilinen)";
    else t = L"bilinmiyor";
    if (s.charging) t += L", şarj oluyor";
    if (s.asleep) t += L", uykuda";
    return t;
}

std::wstring EtaText(const TrayEntry& e) {
    const auto& s = e.st;
    if (!s.connected) return L"Alıcı takılı, cihaz kapalı";
    if (s.charging) {
        if (s.percent >= 100) return L"Şarj tamamlandı (%100)";
        return L"Şarj oluyor ⚡";
    }
    int shown = s.percent >= 0 ? s.percent : e.lastPct;
    if (s.asleep || s.percent < 0) {
        if (shown >= 0) return L"Uykuda / Önbellek (son bilinen değer)";
        return L"Uykuda veya ATK HUB portu kullanıyor";
    }
    if (e.sessionStartPct > s.percent && e.sessionStartTick > 0) {
        ULONGLONG elapsedMs = GetTickCount64() - e.sessionStartTick;
        if (elapsedMs >= 60000) {
            double dropped = (double)(e.sessionStartPct - s.percent);
            double hours = (double)elapsedMs / 3600000.0;
            double ratePerHr = dropped / hours;
            if (ratePerHr >= 0.1 && ratePerHr <= 50.0) {
                double remHours = (double)s.percent / ratePerHr;
                int rh = (int)remHours;
                int rm = (int)((remHours - rh) * 60.0);
                wchar_t buf[128];
                if (rh > 0)
                    swprintf(buf, 128, L"Tüketim: ~%%%.1f/sa  •  Tahmini ~%d sa %d dk", ratePerHr, rh, rm);
                else
                    swprintf(buf, 128, L"Tüketim: ~%%%.1f/sa  •  Tahmini ~%d dk", ratePerHr, std::max(1, rm));
                return buf;
            }
        }
    }
    if (s.millivolts > 0) {
        return L"Aktif  •  " + std::to_wstring(s.millivolts) + L" mV";
    }
    return L"Aktif  •  Tüketim hızı hesaplanıyor";
}

void TrayCall(DWORD msg, UINT uid, HICON icon, const std::wstring& tip, const wchar_t* infoTitle = nullptr,
              const std::wstring& info = L"", DWORD infoFlags = NIIF_WARNING) {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd;
    nid.uID = uid;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = icon;
    CopyTip(nid.szTip, ARRAYSIZE(nid.szTip), tip);
    if (infoTitle) {
        nid.uFlags |= NIF_INFO;
        CopyTip(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), infoTitle);
        CopyTip(nid.szInfo, ARRAYSIZE(nid.szInfo), info);
        nid.dwInfoFlags = infoFlags;
    }
    Shell_NotifyIconW(msg, &nid);
    if (msg == NIM_ADD) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
}

void TrayDelete(UINT uid) {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd;
    nid.uID = uid;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

// Sends a Windows toast/balloon notification even when running in Taskbar Widget mode!
void SendDesktopNotification(const wchar_t* title, const std::wstring& body, DWORD flags) {
    if (!g_notifIcon) g_notifIcon = RenderIcon(L"!", 15, Foreground(), Rgb{ 0, 170, 255 }, false);
    TrayCall(g_notifIconAdded ? NIM_MODIFY : NIM_ADD, NOTIF_ICON_ID, g_notifIcon, kAppName, title, body, flags);
    g_notifIconAdded = true;
    SetTimer(g_hwnd, TIMER_NOTIF_CLEANUP, 6000, nullptr);
}

void CheckNotifications(TrayEntry& e) {
    if (IsHidden(e.st.key) || !e.st.connected) return;
    const auto& s = e.st;
    int shown = s.percent >= 0 ? s.percent : e.lastPct;

    if (s.charging || shown > g_lowBatteryPct + 10) e.lowWarned = false;
    if (!s.charging || (s.percent >= 0 && s.percent < 95)) e.fullWarned = false;

    if (!s.charging && s.percent >= 0 && s.percent <= g_lowBatteryPct && !e.lowWarned) {
        e.lowWarned = true;
        std::wstring msg = s.name + L" pili %" + std::to_wstring(s.percent) + L" seviyesine düştü.";
        SendDesktopNotification(L"Düşük Pil Uyarısı", msg, NIIF_WARNING);
    } else if (s.charging && s.percent == 100 && !e.fullWarned && e.prevPct > 0 && e.prevPct < 100) {
        e.fullWarned = true;
        std::wstring msg = s.name + L" tamamen şarj oldu (%100).";
        SendDesktopNotification(L"Şarj Tamamlandı ⚡", msg, NIIF_INFO);
    }
}

void UpdatePlaceholder() {
    if (g_useWidget) {
        if (g_placeholder) { TrayDelete(PLACEHOLDER_ID); g_placeholder = false; }
        return;
    }
    bool any = false;
    for (auto& [k, e] : g_entries) any |= e.added;
    if (!any && !g_placeholder) {
        if (!g_placeholderIcon) g_placeholderIcon = RenderIcon(L"-", -1, Foreground(), Foreground(), true);
        TrayCall(NIM_ADD, PLACEHOLDER_ID, g_placeholderIcon, L"Kablosuz Pil\nPil bilgisi olan cihaz bulunamadı");
        g_placeholder = true;
    } else if (any && g_placeholder) {
        TrayDelete(PLACEHOLDER_ID);
        g_placeholder = false;
    }
}

void ApplyEntry(TrayEntry& e) {
    CheckNotifications(e);

    const auto& s = e.st;
    bool visible = s.connected && !g_useWidget && !IsEntryHidden(e);
    if (!visible) {
        if (e.added) { TrayDelete(e.uid); e.added = false; }
        return;
    }
    int shown = s.percent >= 0 ? s.percent : e.lastPct;
    bool dim = s.percent < 0;
    std::wstring sig = std::to_wstring(shown) + (s.charging ? L"c" : L"") + (dim ? L"d" : L"") +
                       (g_lightTaskbar ? L"l" : L"k") + std::to_wstring(g_lowBatteryPct);
    if (sig != e.iconSig || !e.icon) {
        if (e.icon) DestroyIcon(e.icon);
        e.icon = IconFor(shown, s.charging, dim);
        e.iconSig = sig;
    }
    std::wstring tip = s.name + L"\n" + StatusText(e);
    TrayCall(e.added ? NIM_MODIFY : NIM_ADD, e.uid, e.icon, tip);
    e.added = true;
}

int KindOrder(DeviceKind k) {  // headset first, then mouse, ...
    switch (k) {
        case DeviceKind::Headset:  return 0;
        case DeviceKind::Mouse:    return 1;
        case DeviceKind::Keyboard: return 2;
        case DeviceKind::Gamepad:  return 3;
        default:                   return 4;
    }
}

void UpdateFlyoutData() {
    std::vector<const TrayEntry*> list;
    for (auto& [k, e] : g_entries) list.push_back(&e);
    std::sort(list.begin(), list.end(), [](const TrayEntry* a, const TrayEntry* b) {
        int ka = KindOrder(a->st.kind), kb = KindOrder(b->st.kind);
        return ka != kb ? ka < kb : a->st.name < b->st.name;
    });
    std::vector<FlyoutCard> cards;
    for (auto* e : list) {
        FlyoutCard c;
        c.key = e->st.key;
        c.name = e->st.name;
        c.via = e->st.via;
        c.statusText = StatusText(*e);
        c.etaText = EtaText(*e);
        c.kind = e->st.kind;
        c.percent = e->st.percent >= 0 ? e->st.percent : e->lastPct;
        c.charging = e->st.charging;
        c.asleep = e->st.asleep || e->st.percent < 0;
        c.connected = e->st.connected;
        c.low = !c.charging && c.percent >= 0 && c.percent <= g_lowBatteryPct;
        c.hidden = IsHidden(e->st.key);
        cards.push_back(c);
    }
    flyout::SetData(cards, g_pollIntervalSec, g_lowBatteryPct, g_lightTaskbar);
}

void UpdateWidget() {
    UpdateFlyoutData();
    if (!g_useWidget) return;
    std::vector<const TrayEntry*> list;
    for (auto& [k, e] : g_entries)
        if (e.st.connected && !IsEntryHidden(e)) list.push_back(&e);
    std::sort(list.begin(), list.end(), [](const TrayEntry* a, const TrayEntry* b) {
        int ka = KindOrder(a->st.kind), kb = KindOrder(b->st.kind);
        return ka != kb ? ka < kb : a->st.name < b->st.name;
    });
    std::vector<WidgetItem> items;
    std::wstring tip;
    for (auto* e : list) {
        WidgetItem it;
        it.kind = e->st.kind;
        it.percent = e->st.percent >= 0 ? e->st.percent : e->lastPct;
        if (e->st.percent >= 0) it.text = e->st.levelText;
        it.charging = e->st.charging;
        it.dim = e->st.percent < 0;
        it.low = !it.charging && it.percent >= 0 && it.percent <= g_lowBatteryPct;
        it.bluetooth = e->st.via == L"Bluetooth";
        items.push_back(it);
        if (!tip.empty()) tip += L"\n";
        tip += e->st.name + L": " + StatusText(*e);
    }
    if (tip.empty()) tip = L"Kablosuz Pil: pil bilgisi olan cihaz bulunamadı (Sol tık: Panel, Sağ tık: Menü)";
    widget::SetItems(items, tip);
}

void OnResults(std::vector<DeviceStatus>& results) {
    ULONGLONG now = GetTickCount64();
    std::map<std::wstring, bool> seen;
    for (auto& s : results) {
        auto& e = g_entries[s.key];
        if (!e.uid) e.uid = StableId(s.key);
        e.prevPct = e.lastPct;
        e.st = s;
        if (s.percent >= 0) {
            if (s.charging || e.sessionStartPct < 0 || s.percent > e.lastPct) {
                e.sessionStartPct = s.percent;
                e.sessionStartTick = now;
            }
            e.lastPct = s.percent;
        }
        e.lastSeen = now;
        seen[s.key] = true;
        ApplyEntry(e);
    }
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        auto& e = it->second;
        bool inGrace = !seen.count(it->first) && e.st.graceSec > 0 && e.st.connected &&
                       now - e.lastSeen < (ULONGLONG)e.st.graceSec * 1000;
        if (inGrace) {
            e.st.percent = -1;
            e.st.asleep = true;
            ApplyEntry(e);
        }
        if (!seen.count(it->first) && !inGrace) {
            if (it->second.added) TrayDelete(it->second.uid);
            if (it->second.icon) DestroyIcon(it->second.icon);
            it = g_entries.erase(it);
        } else {
            ++it;
        }
    }
    UpdatePlaceholder();
    UpdateWidget();
}

void Reconcile() {
    bool alive = g_wantWidget && widget::Alive();
    if (g_wantWidget && !alive) alive = widget::Create(g_inst, g_hwnd, WM_WIDGET_CLICK, g_lightTaskbar);
    if (!g_wantWidget) widget::Destroy();
    bool changed = alive != g_useWidget;
    g_useWidget = alive;
    if (changed) {
        for (auto& [k, e] : g_entries) ApplyEntry(e);
        UpdatePlaceholder();
        UpdateWidget();
    } else if (g_useWidget) {
        widget::Reposition();
    }
}

void SetMode(bool useWidget) {
    g_wantWidget = useWidget;
    SaveWidgetMode(useWidget);
    Reconcile();
}

void SetHideLastKnown(bool on) {
    g_hideLastKnown = on;
    SaveHideLastKnown(on);
    for (auto& [k, e] : g_entries) ApplyEntry(e);
    UpdatePlaceholder();
    UpdateWidget();
}

void SetPollInterval(int sec) {
    g_pollIntervalSec = sec;
    SaveDwordSetting(L"PollIntervalSec", (DWORD)sec);
    SetEvent(g_refreshEvt);
    UpdateFlyoutData();
}

void SetLowBatteryThreshold(int pct) {
    g_lowBatteryPct = pct;
    SaveDwordSetting(L"LowBatteryPct", (DWORD)pct);
    for (auto& [k, e] : g_entries) ApplyEntry(e);
    UpdateWidget();
}

void ToggleFlyoutPanel() {
    UpdateFlyoutData();
    RECT anchor{};
    if (!g_useWidget || !widget::GetScreenRect(anchor)) {
        POINT pt;
        GetCursorPos(&pt);
        anchor = { pt.x - 20, pt.y - 10, pt.x + 20, pt.y + 10 };
    }
    flyout::Toggle(anchor, g_lightTaskbar);
}

void ReAddAll() {
    g_placeholder = false;
    for (auto& [k, e] : g_entries) { e.added = false; ApplyEntry(e); }
    UpdatePlaceholder();
}

void RedrawAll() {
    if (g_placeholderIcon) {
        DestroyIcon(g_placeholderIcon);
        g_placeholderIcon = nullptr;
        if (g_placeholder) { TrayDelete(PLACEHOLDER_ID); g_placeholder = false; }
    }
    for (auto& [k, e] : g_entries) { e.iconSig.clear(); ApplyEntry(e); }
    UpdatePlaceholder();
}

void ShowMenu() {
    HMENU m = CreatePopupMenu();
    std::vector<std::wstring> keys;  // menu id ID_DEVICE_FIRST + i -> device key
    AppendMenuW(m, MF_STRING, ID_OPEN_FLYOUT, L"Kontrol panelini aç (Modern Arayüz)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (g_entries.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Pil bilgisi olan cihaz bulunamadı");
    else AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Göstermek veya gizlemek için bir cihaza tıklayın");
    for (auto& [k, e] : g_entries) {
        std::wstring line = e.st.name + L":  " + StatusText(e) + L"   [" + e.st.via + L"]";
        AppendMenuW(m, MF_STRING | (IsHidden(k) ? 0 : MF_CHECKED), ID_DEVICE_FIRST + (UINT)keys.size(), line.c_str());
        keys.push_back(k);
    }
    AppendMenuW(m, MF_STRING | (g_hideLastKnown ? MF_CHECKED : 0), ID_HIDE_LAST_KNOWN, L"Son bilinen / uykudaki cihazları gizle");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_wantWidget ? MF_CHECKED : 0), ID_MODE_WIDGET, L"Görev çubuğunda göster (simge + yüzde)");
    AppendMenuW(m, MF_STRING | (!g_wantWidget ? MF_CHECKED : 0), ID_MODE_TRAY, L"Tepsi simgeleri olarak göster");
    CheckMenuRadioItem(m, ID_MODE_WIDGET, ID_MODE_TRAY, g_wantWidget ? ID_MODE_WIDGET : ID_MODE_TRAY, MF_BYCOMMAND);

    // Yenileme sıklığı alt menüsü
    HMENU pollMenu = CreatePopupMenu();
    AppendMenuW(pollMenu, MF_STRING | (g_pollIntervalSec == 30 ? MF_CHECKED : 0), ID_POLL_30S, L"30 saniye");
    AppendMenuW(pollMenu, MF_STRING | (g_pollIntervalSec == 60 ? MF_CHECKED : 0), ID_POLL_60S, L"1 dakika (Varsayılan)");
    AppendMenuW(pollMenu, MF_STRING | (g_pollIntervalSec == 300 ? MF_CHECKED : 0), ID_POLL_300S, L"5 dakika");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)pollMenu, L"Yenileme sıklığı");

    // Düşük pil uyarısı alt menüsü
    HMENU lowMenu = CreatePopupMenu();
    AppendMenuW(lowMenu, MF_STRING | (g_lowBatteryPct == 10 ? MF_CHECKED : 0), ID_LOW_10, L"%10");
    AppendMenuW(lowMenu, MF_STRING | (g_lowBatteryPct == 15 ? MF_CHECKED : 0), ID_LOW_15, L"%15 (Varsayılan)");
    AppendMenuW(lowMenu, MF_STRING | (g_lowBatteryPct == 20 ? MF_CHECKED : 0), ID_LOW_20, L"%20");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)lowMenu, L"Düşük pil uyarısı eşiği");

    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_REFRESH, L"Şimdi yenile");
    AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Windows ile başlat");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Çıkış");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (cmd) {
        case ID_OPEN_FLYOUT: ToggleFlyoutPanel(); break;
        case ID_REFRESH: SetEvent(g_refreshEvt); break;
        case ID_AUTOSTART: SetAutostart(!AutostartEnabled()); break;
        case ID_HIDE_LAST_KNOWN: SetHideLastKnown(!g_hideLastKnown); break;
        case ID_MODE_WIDGET: SetMode(true); break;
        case ID_MODE_TRAY: SetMode(false); break;
        case ID_POLL_30S: SetPollInterval(30); break;
        case ID_POLL_60S: SetPollInterval(60); break;
        case ID_POLL_300S: SetPollInterval(300); break;
        case ID_LOW_10: SetLowBatteryThreshold(10); break;
        case ID_LOW_15: SetLowBatteryThreshold(15); break;
        case ID_LOW_20: SetLowBatteryThreshold(20); break;
        case ID_EXIT: DestroyWindow(g_hwnd); break;
        default:
            if (cmd >= ID_DEVICE_FIRST && cmd < ID_DEVICE_FIRST + keys.size()) {
                const std::wstring& key = keys[cmd - ID_DEVICE_FIRST];
                if (!g_hidden.erase(key)) g_hidden.insert(key);
                SaveHidden();
                auto it = g_entries.find(key);
                if (it != g_entries.end()) ApplyEntry(it->second);
                UpdatePlaceholder();
                UpdateWidget();
            }
    }
}

// ---- worker thread ----------------------------------------------------------------------------

DWORD WINAPI Worker(LPVOID) {
    for (;;) {
        auto* v = new std::vector<DeviceStatus>(PollAllDevices());
        if (!PostMessageW(g_hwnd, WM_RESULTS, 0, (LPARAM)v)) delete v;
        HANDLE hs[2] = { g_stopEvt, g_refreshEvt };
        DWORD waitMs = (DWORD)g_pollIntervalSec * 1000;
        if (WaitForMultipleObjects(2, hs, FALSE, waitMs) == WAIT_OBJECT_0) return 0;
    }
}

void RefreshSoon(UINT ms) { SetTimer(g_hwnd, TIMER_DEBOUNCE, ms, nullptr); }

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreatedMsg && msg != 0) {  // Explorer restarted
        widget::Destroy();
        g_useWidget = false;
        ReAddAll();
        Reconcile();
        return 0;
    }
    switch (msg) {
        case WM_RESULTS: {
            auto* v = reinterpret_cast<std::vector<DeviceStatus>*>(lp);
            OnResults(*v);
            delete v;
            return 0;
        }
        case WM_TRAY:
            switch (LOWORD(lp)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                case WM_LBUTTONUP:
                    ToggleFlyoutPanel();
                    break;
                case WM_CONTEXTMENU:
                case WM_RBUTTONUP:
                    flyout::Hide();
                    ShowMenu();
                    break;
            }
            return 0;
        case WM_WIDGET_CLICK:
            if (wp == 1) {
                flyout::Hide();
                ShowMenu();
            } else {
                ToggleFlyoutPanel();
            }
            return 0;
        case WM_FLYOUT_ACTION:
            if (wp == flyout::ACT_REFRESH) {
                SetEvent(g_refreshEvt);
            } else if (wp == flyout::ACT_SETTINGS) {
                flyout::Hide();
                ShowMenu();
            } else if (wp >= flyout::ACT_TOGGLE_DEVICE_BASE) {
                size_t idx = (size_t)(wp - flyout::ACT_TOGGLE_DEVICE_BASE);
                std::vector<const TrayEntry*> list;
                for (auto& [k, e] : g_entries) list.push_back(&e);
                std::sort(list.begin(), list.end(), [](const TrayEntry* a, const TrayEntry* b) {
                    int ka = KindOrder(a->st.kind), kb = KindOrder(b->st.kind);
                    return ka != kb ? ka < kb : a->st.name < b->st.name;
                });
                if (idx < list.size()) {
                    const std::wstring& key = list[idx]->st.key;
                    if (!g_hidden.erase(key)) g_hidden.insert(key);
                    SaveHidden();
                    auto it = g_entries.find(key);
                    if (it != g_entries.end()) ApplyEntry(it->second);
                    UpdatePlaceholder();
                    UpdateWidget();
                }
            }
            return 0;
        case WM_ENDSESSION:  // sign-out / shutdown / installer restart manager
            if (wp) DestroyWindow(h);
            return 0;
        case WM_DEVICECHANGE:
            if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) RefreshSoon(2500);
            return TRUE;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC) RefreshSoon(5000);
            return TRUE;
        case WM_TIMER:
            if (wp == TIMER_DEBOUNCE) { KillTimer(h, TIMER_DEBOUNCE); SetEvent(g_refreshEvt); }
            if (wp == TIMER_WIDGET) Reconcile();
            if (wp == TIMER_NOTIF_CLEANUP) {
                KillTimer(h, TIMER_NOTIF_CLEANUP);
                if (g_notifIconAdded) { TrayDelete(NOTIF_ICON_ID); g_notifIconAdded = false; }
            }
            return 0;
        case WM_SETTINGCHANGE:
            if (lp && !lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet")) {
                bool light = ReadLightTaskbar();
                if (light != g_lightTaskbar) {
                    g_lightTaskbar = light;
                    RedrawAll();
                    widget::SetLightTheme(light);
                    UpdateFlyoutData();
                }
            }
            return 0;
        case WM_DESTROY:
            KillTimer(h, TIMER_WIDGET);
            KillTimer(h, TIMER_NOTIF_CLEANUP);
            flyout::Destroy();
            widget::Destroy();
            SetEvent(g_stopEvt);
            if (g_worker) WaitForSingleObject(g_worker, 5000);
            for (auto& [k, e] : g_entries) {
                if (e.added) TrayDelete(e.uid);
                if (e.icon) DestroyIcon(e.icon);
            }
            if (g_placeholder) TrayDelete(PLACEHOLDER_ID);
            if (g_notifIconAdded) TrayDelete(NOTIF_ICON_ID);
            if (g_notifIcon) DestroyIcon(g_notifIcon);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// `WirelessBattery.exe --list` prints what it finds, `--dump` every HID collection.
int ListToConsole(bool dump) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    auto write = [&](const std::wstring& s) {
        int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
        std::string u(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), u.data(), n, nullptr, nullptr);
        DWORD w;
        WriteFile(out, u.data(), (DWORD)u.size(), &w, nullptr);
    };
    if (dump) {
        write(DumpHidCollections());
        return 0;
    }
    auto list = PollAllDevices();
    if (list.empty()) write(L"Pil bilgisi olan cihaz bulunamadı.\n");
    for (auto& s : list) {
        std::wstring line = s.name + L"  [" + s.via + L"]  ";
        if (!s.connected) line += L"kapalı / bağlı değil";
        else if (s.asleep) line += L"uykuda";
        else if (!s.levelText.empty()) line += s.levelText;
        else if (s.percent >= 0) line += L"%" + std::to_wstring(s.percent);
        else line += L"bilinmiyor";
        if (s.charging) line += L", şarj oluyor";
        if (s.millivolts) line += L", " + std::to_wstring(s.millivolts) + L" mV";
        write(line + L"   (" + s.key + L")\n");
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    if (cmdLine && wcsstr(cmdLine, L"--list")) return ListToConsole(false);
    if (cmdLine && wcsstr(cmdLine, L"--dump")) return ListToConsole(true);
    if (cmdLine && wcsstr(cmdLine, L"--quit")) {  // used by the installer before upgrading/uninstalling
        if (HWND w = FindWindowW(L"WirelessBatteryWnd", nullptr)) {
            PostMessageW(w, WM_CLOSE, 0, 0);
            for (int i = 0; i < 50 && FindWindowW(L"WirelessBatteryWnd", nullptr); i++) Sleep(100);
        }
        return 0;
    }
    g_inst = inst;

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\WirelessBatteryTray");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g_lightTaskbar = ReadLightTaskbar();
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"WirelessBatteryWnd";
    RegisterClassExW(&wc);
    // A hidden top-level window (not message-only) so it receives the TaskbarCreated broadcast.
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;

    DEV_BROADCAST_DEVICEINTERFACE_W hidFilter{ sizeof(hidFilter) };
    hidFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    hidFilter.dbcc_classguid = GUID_DEVINTERFACE_HID;
    HDEVNOTIFY notifyHid = RegisterDeviceNotificationW(g_hwnd, &hidFilter, DEVICE_NOTIFY_WINDOW_HANDLE);

    DEV_BROADCAST_DEVICEINTERFACE_W comFilter{ sizeof(comFilter) };
    comFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    comFilter.dbcc_classguid = kGuidDevInterfaceComPort;
    HDEVNOTIFY notifyCom = RegisterDeviceNotificationW(g_hwnd, &comFilter, DEVICE_NOTIFY_WINDOW_HANDLE);

    g_refreshEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_stopEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wantWidget = LoadWidgetMode();
    g_hideLastKnown = LoadHideLastKnown();
    LoadUserPreferences();
    LoadHidden();
    if (!LoadBoolSetting(L"FirstRunAutoStartSet", false)) {
        SetAutostart(true);
        SaveBoolSetting(L"FirstRunAutoStartSet", true);
    }
    flyout::Init(inst, g_hwnd, WM_FLYOUT_ACTION);
    Reconcile();
    UpdatePlaceholder();  // something to click on until the first poll finishes
    SetTimer(g_hwnd, TIMER_WIDGET, 1000, nullptr);
    g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    // drain results posted after shutdown started
    while (PeekMessageW(&m, nullptr, WM_RESULTS, WM_RESULTS, PM_REMOVE)) delete reinterpret_cast<std::vector<DeviceStatus>*>(m.lParam);
    if (notifyCom) UnregisterDeviceNotification(notifyCom);
    if (notifyHid) UnregisterDeviceNotification(notifyHid);
    if (g_worker) CloseHandle(g_worker);
    ReleaseMutex(mutex);
    return 0;
}
