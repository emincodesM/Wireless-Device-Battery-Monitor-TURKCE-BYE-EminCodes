// Modern Windows 11 Flyout Panel for Wireless Battery Monitor.
// Supports dark/light theme, DPI scaling, multi-monitor workareas, top/bottom taskbars,
// mouse-wheel scrolling, keyboard navigation, per-card taskbar hide toggle, battery sparkline,
// and an interactive in-Flyout Settings view.
#include "flyout.h"
#include "i18n.h"
#include "labels.h"
#include "platform_compat.h"
#include "settings.h"
#include "widget.h"
#include "../res/version.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>
#ifdef DeleteBrush
#undef DeleteBrush
#endif
#ifdef DeletePen
#undef DeletePen
#endif
#include <algorithm>
#include <cmath>
#include <cwchar>

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

namespace flyout {
namespace {

constexpr const wchar_t* kClass = L"WirelessBatteryFlyoutWnd";

HINSTANCE g_inst = nullptr;
HWND g_owner = nullptr;
HWND g_wnd = nullptr;
UINT g_actionMsg = 0;
std::vector<Entry> g_entries;
int g_dpi = 96;
bool g_light = false;
int g_hoverCard = -1;
int g_hoverEyeCard = -1;
int g_hoverBtn = 0; // 1 = Refresh, 2 = Settings, 3 = Close
int g_focusIndex = -1; // keyboard navigation focus (-1 = none, 0..N-1 = cards, N..N+2 = header buttons)
int g_scrollY = 0;
int g_contentHeight = 0;
bool g_showSettings = false;
int g_hoverSettingRow = -1;
ULONGLONG g_lastPollTick = 0;
ULONGLONG g_deactivatedAt = 0; // P0-2: prevents immediate reopen when clicking tray/widget to close

struct SettingHit {
    RECT rc{};
    int action = 0;
    int param = 0;
};
std::vector<SettingHit> g_settingHits;

int Px(int v) { return MulDiv(v, g_dpi, 96); }

UINT QueryMonitorDpi(const RECT& anchor) {
    HMONITOR mon = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static GetDpiForMonitorFn getMonDpi = []() -> GetDpiForMonitorFn {
        HMODULE shcore = LoadSystemLibrary(L"shcore.dll");
        return shcore ? reinterpret_cast<GetDpiForMonitorFn>(reinterpret_cast<void*>(GetProcAddress(shcore, "GetDpiForMonitor"))) : nullptr;
    }();
    if (getMonDpi && mon) {
        UINT dx = 96, dy = 96;
        if (SUCCEEDED(getMonDpi(mon, 0 /*MDT_EFFECTIVE_DPI*/, &dx, &dy)) && dx > 0) return dx;
    }
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static GetDpiForWindowFn fn = reinterpret_cast<GetDpiForWindowFn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    if (fn && g_wnd) return fn(g_wnd);
    HDC dc = GetDC(nullptr);
    int d = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return d ? d : 96;
}

bool IsLightTheme() {
    DWORD val = 0, sz = sizeof(val);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &val, &sz) == ERROR_SUCCESS)
        return val != 0;
    return false;
}

const wchar_t* IconFontName() {
    static const wchar_t* chosen = nullptr;
    if (chosen) return chosen;
    HDC dc = GetDC(nullptr);
    chosen = L"Segoe MDL2 Assets";
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, L"Segoe Fluent Icons");
    EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp) -> int {
        *reinterpret_cast<const wchar_t**>(lp) = L"Segoe Fluent Icons";
        return 0;
    }, (LPARAM)&chosen, 0);
    ReleaseDC(nullptr, dc);
    return chosen;
}

wchar_t Glyph(DeviceKind k) {
    switch (k) {
        case DeviceKind::Mouse:    return 0xE962;
        case DeviceKind::Keyboard: return 0xE765;
        case DeviceKind::Headset:  return 0xE7F6;
        case DeviceKind::Gamepad:  return 0xE7FC;
        case DeviceKind::Phone:    return 0xE8EA;
        default:                   return 0xE839;
    }
}

HFONT MakeFont(int heightPx, int weight, const wchar_t* face) {
    return CreateFontW(-heightPx, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

// Cached GDI fonts recreated only when DPI changes (P3-4).
struct FontCache {
    int dpi = 0;
    HFONT title = nullptr;
    HFONT sub   = nullptr;
    HFONT name  = nullptr;
    HFONT meta  = nullptr;
    HFONT badge = nullptr;
    HFONT pct   = nullptr;
    HFONT icon  = nullptr;
    HFONT btn   = nullptr;

    void Clear() {
        for (HFONT* f : { &title, &sub, &name, &meta, &badge, &pct, &icon, &btn }) {
            if (*f) { DeleteObject(*f); *f = nullptr; }
        }
        dpi = 0;
    }

    void Ensure(int curDpi) {
        if (dpi == curDpi && title) return;
        Clear();
        dpi   = curDpi;
        title = MakeFont(Px(15), FW_BOLD,     L"Segoe UI");
        sub   = MakeFont(Px(11), FW_NORMAL,   L"Segoe UI");
        name  = MakeFont(Px(13), FW_SEMIBOLD, L"Segoe UI");
        meta  = MakeFont(Px(11), FW_NORMAL,   L"Segoe UI");
        badge = MakeFont(Px(10), FW_SEMIBOLD, L"Segoe UI");
        pct   = MakeFont(Px(18), FW_BOLD,     L"Segoe UI");
        icon  = MakeFont(Px(20), FW_NORMAL,   IconFontName());
        btn   = MakeFont(Px(13), FW_NORMAL,   IconFontName());
    }
} g_fonts;

// ---- GDI+ helpers ------------------------------------------------------------------------------

struct GpGraphics;
struct GpBrush;
struct GpPen;
struct GpPath;
using GpStatus = int;
using ARGB = DWORD;
enum GpSmoothingMode { SmoothingModeAntiAlias = 4 };
enum GpUnit { UnitWorld = 0 };

struct GdiplusStartupInput {
    UINT32 GdiplusVersion = 1;
    void* DebugEventCallback = nullptr;
    BOOL SuppressBackgroundThread = FALSE;
    BOOL SuppressExternalCodecs = FALSE;
};

struct Gdip {
    HMODULE mod = nullptr;
    ULONG_PTR token = 0;
    GpStatus (WINAPI* Startup)(ULONG_PTR*, const GdiplusStartupInput*, void*) = nullptr;
    GpStatus (WINAPI* CreateFromHDC)(HDC, GpGraphics**) = nullptr;
    GpStatus (WINAPI* DeleteGraphics)(GpGraphics*) = nullptr;
    GpStatus (WINAPI* SetSmoothing)(GpGraphics*, GpSmoothingMode) = nullptr;
    GpStatus (WINAPI* CreateSolidFill)(ARGB, GpBrush**) = nullptr;
    GpStatus (WINAPI* DeleteBrush)(GpBrush*) = nullptr;
    GpStatus (WINAPI* CreatePen1)(ARGB, float, GpUnit, GpPen**) = nullptr;
    GpStatus (WINAPI* DeletePen)(GpPen*) = nullptr;
    GpStatus (WINAPI* CreatePath)(int, GpPath**) = nullptr;
    GpStatus (WINAPI* DeletePath)(GpPath*) = nullptr;
    GpStatus (WINAPI* AddPathArc)(GpPath*, float, float, float, float, float, float) = nullptr;
    GpStatus (WINAPI* AddPathLine)(GpPath*, float, float, float, float) = nullptr;
    GpStatus (WINAPI* ClosePath)(GpPath*) = nullptr;
    GpStatus (WINAPI* FillPath)(GpGraphics*, GpBrush*, GpPath*) = nullptr;
    GpStatus (WINAPI* DrawPath)(GpGraphics*, GpPen*, GpPath*) = nullptr;
    GpStatus (WINAPI* DrawLine)(GpGraphics*, GpPen*, float, float, float, float) = nullptr;

    bool Init() {
        if (mod) return true;
        mod = LoadSystemLibrary(L"gdiplus.dll");
        if (!mod) return false;
        auto load = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::decay_t<decltype(fn)>>(reinterpret_cast<void*>(GetProcAddress(mod, name)));
        };
        load(Startup,         "GdiplusStartup");
        load(CreateFromHDC,   "GdipCreateFromHDC");
        load(DeleteGraphics,  "GdipDeleteGraphics");
        load(SetSmoothing,    "GdipSetSmoothingMode");
        load(CreateSolidFill, "GdipCreateSolidFill");
        load(DeleteBrush,     "GdipDeleteBrush");
        load(CreatePen1,      "GdipCreatePen1");
        load(DeletePen,       "GdipDeletePen");
        load(CreatePath,      "GdipCreatePath");
        load(DeletePath,      "GdipDeletePath");
        load(AddPathArc,      "GdipAddPathArc");
        load(AddPathLine,     "GdipAddPathLine");
        load(ClosePath,       "GdipClosePathFigure");
        load(FillPath,        "GdipFillPath");
        load(DrawPath,        "GdipDrawPath");
        load(DrawLine,        "GdipDrawLine");
        if (!Startup || !CreateFromHDC) return false;
        GdiplusStartupInput in;
        return Startup(&token, &in, nullptr) == 0;
    }
} g_gdip;

ARGB MakeArgb(BYTE a, BYTE r, BYTE g, BYTE b) {
    return ((ARGB)a << 24) | ((ARGB)r << 16) | ((ARGB)g << 8) | (ARGB)b;
}

GpPath* RoundPath(float x, float y, float w, float h, float r) {
    GpPath* p = nullptr;
    g_gdip.CreatePath(0, &p);
    if (!p) return nullptr;
    float d = r * 2.0f;
    if (d > w) d = w;
    if (d > h) d = h;
    if (d <= 0.1f) {
        g_gdip.AddPathLine(p, x, y, x + w, y);
        g_gdip.AddPathLine(p, x + w, y + h, x, y + h);
    } else {
        g_gdip.AddPathArc(p, x,         y,         d, d, 180.0f, 90.0f);
        g_gdip.AddPathArc(p, x + w - d, y,         d, d, 270.0f, 90.0f);
        g_gdip.AddPathArc(p, x + w - d, y + h - d, d, d,   0.0f, 90.0f);
        g_gdip.AddPathArc(p, x,         y + h - d, d, d,  90.0f, 90.0f);
    }
    g_gdip.ClosePath(p);
    return p;
}

void FillRound(GpGraphics* g, float x, float y, float w, float h, float r, ARGB col) {
    if (!g || w <= 0 || h <= 0) return;
    GpPath* p = RoundPath(x, y, w, h, r);
    GpBrush* b = nullptr;
    g_gdip.CreateSolidFill(col, &b);
    if (p && b) g_gdip.FillPath(g, b, p);
    if (b) g_gdip.DeleteBrush(b);
    if (p) g_gdip.DeletePath(p);
}

void StrokeRound(GpGraphics* g, float x, float y, float w, float h, float r, float sw, ARGB col) {
    if (!g || w <= 0 || h <= 0) return;
    GpPath* p = RoundPath(x, y, w, h, r);
    GpPen* pen = nullptr;
    g_gdip.CreatePen1(col, sw, UnitWorld, &pen);
    if (p && pen) g_gdip.DrawPath(g, pen, p);
    if (pen) g_gdip.DeletePen(pen);
    if (p) g_gdip.DeletePath(p);
}

// ---- Layout metrics ----------------------------------------------------------------------------

int PanelWidth()   { return Px(370); }
int HeaderHeight() { return Px(52); }
int CardHeight()   { return Px(94); }
int CardGap()      { return Px(8); }
int PadX()         { return Px(14); }
int FooterHeight() { return Px(34); }

int DesiredContentHeight() {
    if (g_showSettings) return Px(330);
    int n = (int)g_entries.size();
    if (n == 0) return Px(110);
    return n * CardHeight() + (n - 1) * CardGap() + Px(4);
}

int DesiredPanelHeight(int maxWorkAreaH) {
    g_contentHeight = DesiredContentHeight();
    int total = HeaderHeight() + g_contentHeight + FooterHeight();
    int limit = maxWorkAreaH > 0 ? (maxWorkAreaH * 80 / 100) : total;
    return std::min(total, std::max(Px(220), limit));
}

RECT CardRect(int idx, int W) {
    int y = HeaderHeight() + idx * (CardHeight() + CardGap()) - g_scrollY;
    return RECT{ PadX(), y, W - PadX(), y + CardHeight() };
}

RECT CardEyeRect(const RECT& c) {
    return RECT{ c.right - Px(30), c.top + Px(6), c.right - Px(8), c.top + Px(26) };
}

RECT RefreshBtnRect(int W)  { return RECT{ W - Px(98), Px(12), W - Px(70), Px(40) }; }
RECT SettingsBtnRect(int W) { return RECT{ W - Px(68), Px(12), W - Px(40), Px(40) }; }
RECT CloseBtnRect(int W)    { return RECT{ W - Px(38), Px(12), W - Px(12), Px(40) }; }

std::wstring StatusBadgeText(const DeviceStatus& d) {
    if (!d.connected) return T(L"Disconnected");
    if (d.charging)   return T(L"Charging");
    if (d.asleep)     return T(L"Sleeping / Cached");
    if (d.percent < 0) return T(L"No Battery Info");
    if (d.percent <= 10) return T(L"Critical");
    if (d.percent <= 25) return T(L"Low");
    if (d.percent >= 80) return T(L"Good");
    return T(L"Discharging");
}

void DrawSparkline(GpGraphics* gg, float x, float y, float w, float h, const std::vector<BatterySample>& hist, ARGB col) {
    if (!gg || !g_gdip.DrawLine || hist.size() < 2) return;
    GpPen* pen = nullptr;
    g_gdip.CreatePen1(col, (float)std::max(1, Px(1)), UnitWorld, &pen);
    if (!pen) return;
    size_t n = hist.size();
    for (size_t i = 1; i < n; i++) {
        float x0 = x + (w * (float)(i - 1)) / (float)(n - 1);
        float x1 = x + (w * (float)i) / (float)(n - 1);
        float y0 = y + h - (h * std::clamp(hist[i - 1].percent, 0, 100)) / 100.0f;
        float y1 = y + h - (h * std::clamp(hist[i].percent, 0, 100)) / 100.0f;
        g_gdip.DrawLine(gg, pen, x0, y0, x1, y1);
    }
    g_gdip.DeletePen(pen);
}

void PaintSettingsView(HDC dc, GpGraphics* gg, int W, int H, COLORREF txtMain, COLORREF txtSub) {
    g_settingHits.clear();
    int y = HeaderHeight() + Px(4) - g_scrollY;
    int left = PadX();
    int right = W - PadX();

    auto drawPill = [&](int px, int py, int pw, int ph, const wchar_t* label, bool active, int action, int param) {
        RECT r{ px, py, px + pw, py + ph };
        bool hov = g_hoverSettingRow >= 0 && g_hoverSettingRow == (int)g_settingHits.size();
        ARGB bg = active ? MakeArgb(230, 0, 120, 212)
                         : (g_light ? MakeArgb(hov ? 255 : 220, 232, 235, 242) : MakeArgb(hov ? 255 : 210, 44, 46, 54));
        FillRound(gg, (float)r.left, (float)r.top, (float)pw, (float)ph, (float)Px(6), bg);
        SelectObject(dc, g_fonts.badge);
        SetTextColor(dc, active ? RGB(255, 255, 255) : txtMain);
        DrawTextW(dc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        g_settingHits.push_back({ r, action, param });
    };

    // 1. Poll interval
    SelectObject(dc, g_fonts.name);
    SetTextColor(dc, txtMain);
    RECT rLbl1{ left, y, right, y + Px(20) };
    DrawTextW(dc, T(L"Update interval"), -1, &rLbl1, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    y += Px(22);
    int curPoll = settings::g_pollIntervalSec.load(std::memory_order_relaxed);
    int px = left;
    int pw = (right - left - Px(4) * 4) / 5;
    for (int s : { 5, 10, 15, 30, 60 }) {
        wchar_t b[16];
        swprintf_s(b, IsTurkish() ? L"%d sn" : L"%ds", s);
        drawPill(px, y, pw, Px(24), b, curPoll == s, 1, s);
        px += pw + Px(4);
    }
    y += Px(32);

    // 2. Low battery warning threshold
    SelectObject(dc, g_fonts.name);
    SetTextColor(dc, txtMain);
    RECT rLbl2{ left, y, right, y + Px(20) };
    DrawTextW(dc, T(L"Low battery warning"), -1, &rLbl2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    y += Px(22);
    px = left;
    pw = (right - left - Px(4) * 5) / 6;
    for (int p : { 0, 10, 15, 20, 25, 30 }) {
        wchar_t b[16];
        if (p == 0) wcscpy_s(b, T(L"Off"));
        else swprintf_s(b, L"%%%d", p);
        drawPill(px, y, pw, Px(24), b, settings::g_lowBatteryThreshold == p, 2, p);
        px += pw + Px(4);
    }
    y += Px(34);

    // 3. Toggle rows (Notifications, Taskbar, Startup, Language)
    auto drawToggleRow = [&](const wchar_t* label, bool on, int action, int param = 0) {
        RECT rRow{ left, y, right, y + Px(28) };
        bool hov = g_hoverSettingRow >= 0 && g_hoverSettingRow == (int)g_settingHits.size();
        ARGB bg = g_light ? MakeArgb(hov ? 255 : 210, 240, 242, 247) : MakeArgb(hov ? 255 : 200, 36, 38, 46);
        FillRound(gg, (float)rRow.left, (float)rRow.top, (float)(right - left), (float)Px(28), (float)Px(6), bg);
        SelectObject(dc, g_fonts.meta);
        SetTextColor(dc, txtMain);
        RECT rTxt{ left + Px(10), y, right - Px(64), y + Px(28) };
        DrawTextW(dc, label, -1, &rTxt, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        // Toggle switch pill on right
        int swW = Px(36), swH = Px(18);
        int swX = right - Px(10) - swW, swY = y + (Px(28) - swH) / 2;
        FillRound(gg, (float)swX, (float)swY, (float)swW, (float)swH, (float)(swH / 2),
                  on ? MakeArgb(240, 46, 204, 113) : MakeArgb(160, 120, 125, 135));
        float knobD = (float)(swH - Px(4));
        float knobX = on ? (float)(swX + swW - Px(2) - knobD) : (float)(swX + Px(2));
        FillRound(gg, knobX, (float)(swY + Px(2)), knobD, knobD, knobD * 0.5f, MakeArgb(255, 255, 255, 255));

        g_settingHits.push_back({ rRow, action, param });
        y += Px(32);
    };

    drawToggleRow(T(L"Low battery notification"),  settings::g_notifyLowBattery, 3, 1);
    drawToggleRow(T(L"Full charge notification"),  settings::g_notifyFullCharge, 3, 2);
    drawToggleRow(T(L"Connection notifications"),  settings::g_notifyConnect,    3, 3);
    drawToggleRow(T(L"Run at Windows startup"),    settings::IsAutoStartEnabled(), 4, 0);

    // Language switch row (Türkçe / English) + Reset hidden devices
    int halfW = (right - left - Px(6)) / 2;
    drawPill(left, y, halfW / 2 - Px(2), Px(26), L"T\x00FCrk\x00E7" L"e", IsTurkish(), 5, 0);
    drawPill(left + halfW / 2 + Px(2), y, halfW / 2 - Px(2), Px(26), L"English", !IsTurkish(), 5, 1);

    wchar_t resetBuf[64];
    swprintf_s(resetBuf, L"%s (%zu)", T(L"Reset hidden devices"), settings::g_hiddenDevices.size());
    drawPill(left + halfW + Px(6), y, halfW, Px(26), resetBuf, false, 6, 0);
    (void)H;
    (void)txtSub;
}

void PaintPanel(HWND wnd, HDC targetDc) {
    RECT rc;
    GetClientRect(wnd, &rc);
    int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) return;

    g_fonts.Ensure(g_dpi);

    HDC dc = CreateCompatibleDC(targetDc);
    HBITMAP bmp = CreateCompatibleBitmap(targetDc, W, H);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);

    COLORREF bgMain = g_light ? RGB(246, 247, 250) : RGB(24, 25, 30);
    COLORREF txtMain = g_light ? RGB(22, 24, 28)   : RGB(245, 246, 250);
    COLORREF txtSub  = g_light ? RGB(105, 110, 120): RGB(155, 160, 172);

    HBRUSH brBg = CreateSolidBrush(bgMain);
    FillRect(dc, &rc, brBg);
    DeleteObject(brBg);

    GpGraphics* gg = nullptr;
    if (g_gdip.Init()) {
        g_gdip.CreateFromHDC(dc, &gg);
        if (gg) g_gdip.SetSmoothing(gg, SmoothingModeAntiAlias);
    }

    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, g_fonts.title);

    // ---- Header (fixed at top) ----
    SetTextColor(dc, txtMain);
    RECT rTitle{ PadX(), Px(10), W - Px(104), Px(30) };
    const wchar_t* headerTitle = g_showSettings ? T(L"Settings") : T(L"Wireless Battery");
    DrawTextW(dc, headerTitle, -1, &rTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(dc, g_fonts.sub);
    SetTextColor(dc, txtSub);
    RECT rSub{ PadX(), Px(30), W - Px(104), Px(46) };
    std::wstring subText;
    if (g_lastPollTick > 0) {
        ULONGLONG sec = (GetTickCount64() - g_lastPollTick) / 1000;
        if (IsTurkish()) {
            subText = sec < 3 ? L"Az \x00F6nce g\x00FCncellendi" : (std::to_wstring(sec) + L" sn \x00F6nce g\x00FCncellendi");
        } else {
            subText = sec < 3 ? L"Updated just now" : (L"Updated " + std::to_wstring(sec) + L"s ago");
        }
    } else {
        subText = T(L"Devices");
    }
    DrawTextW(dc, subText.c_str(), (int)subText.size(), &rSub, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // Header action buttons using Fluent Icons (0xE72C Refresh, 0xE713 Settings, 0xE711 Close)
    auto drawHeaderBtn = [&](RECT r, wchar_t glyph, int btnId, bool activeState) {
        bool hov = (g_hoverBtn == btnId);
        bool focused = (g_focusIndex == (int)g_entries.size() + btnId - 1);
        if ((hov || activeState) && gg) {
            ARGB c = (btnId == 3 && hov) ? MakeArgb(200, 220, 53, 69)
                   : activeState         ? MakeArgb(200, 0, 120, 212)
                   : (g_light ? MakeArgb(25, 0, 0, 0) : MakeArgb(35, 255, 255, 255));
            FillRound(gg, (float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top), (float)Px(6), c);
        }
        if (focused && gg) {
            StrokeRound(gg, (float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top),
                        (float)Px(6), 1.5f, MakeArgb(220, 0, 120, 212));
        }
        SelectObject(dc, g_fonts.btn);
        SetTextColor(dc, ((btnId == 3 && hov) || activeState) ? RGB(255, 255, 255) : txtMain);
        wchar_t s[2] = { glyph, 0 };
        DrawTextW(dc, s, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    };
    drawHeaderBtn(RefreshBtnRect(W),  0xE72C, 1, false);
    drawHeaderBtn(SettingsBtnRect(W), 0xE713, 2, g_showSettings);
    drawHeaderBtn(CloseBtnRect(W),    0xE711, 3, false);

    // Clip content region between Header and Footer for smooth scrolling
    int clipTop = HeaderHeight();
    int clipBot = H - FooterHeight();
    int clipId = SaveDC(dc);
    IntersectClipRect(dc, 0, clipTop, W, clipBot);

    if (g_showSettings) {
        PaintSettingsView(dc, gg, W, H, txtMain, txtSub);
    } else if (g_entries.empty()) {
        RECT rEmpty{ PadX(), clipTop + Px(18), W - PadX(), clipBot - Px(10) };
        SelectObject(dc, g_fonts.name);
        SetTextColor(dc, txtSub);
        std::wstring emptyMsg = std::wstring(T(L"No wireless devices detected")) + L"\n" +
                                T(L"Connect your wireless mouse, keyboard, headset or gamepad");
        DrawTextW(dc, emptyMsg.c_str(), (int)emptyMsg.size(), &rEmpty, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    } else {
        for (int i = 0; i < (int)g_entries.size(); i++) {
            const auto& e = g_entries[i];
            const auto& d = e.status;
            RECT c = CardRect(i, W);
            if (c.bottom < clipTop || c.top > clipBot) continue;

            bool hov = (g_hoverCard == i);
            bool focused = (g_focusIndex == i);

            ARGB cardBg = g_light
                ? MakeArgb(255, hov ? 255 : 252, hov ? 255 : 252, 255)
                : MakeArgb(255, hov ? 42 : 34,   hov ? 44 : 36,   hov ? 52 : 43);
            ARGB cardBorder = g_light ? MakeArgb(30, 0, 0, 0) : MakeArgb(35, 255, 255, 255);

            FillRound(gg, (float)c.left, (float)c.top, (float)(c.right - c.left), (float)(c.bottom - c.top), (float)Px(10), cardBg);
            StrokeRound(gg, (float)c.left, (float)c.top, (float)(c.right - c.left), (float)(c.bottom - c.top),
                        (float)Px(10), focused ? 2.0f : 1.0f,
                        focused ? MakeArgb(230, 0, 120, 212) : cardBorder);

            BrandColor bc = AccentColor(e.accent != Accent::None ? e.accent : BrandOf(d), g_light);
            ARGB accentArgb = (e.accent != Accent::None || BrandOf(d) != Accent::None)
                ? MakeArgb(235, bc.r, bc.g, bc.b)
                : MakeArgb(200, 88, 166, 255);
            FillRound(gg, (float)(c.left + Px(4)), (float)(c.top + Px(14)), (float)Px(4), (float)(CardHeight() - Px(28)), (float)Px(2), accentArgb);

            // Device Icon Circle
            int icX = c.left + Px(16), icY = c.top + Px(14), icS = Px(38);
            ARGB icBg = g_light ? MakeArgb(22, bc.r, bc.g, bc.b) : MakeArgb(38, bc.r, bc.g, bc.b);
            FillRound(gg, (float)icX, (float)icY, (float)icS, (float)icS, (float)Px(10), icBg);

            SelectObject(dc, g_fonts.icon);
            SetTextColor(dc, RGB(bc.r, bc.g, bc.b));
            wchar_t gl[2] = { Glyph(d.kind), 0 };
            RECT rIc{ icX, icY, icX + icS, icY + icS };
            DrawTextW(dc, gl, 1, &rIc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            // Eye toggle button (top-right of card, P4-5)
            RECT rEye = CardEyeRect(c);
            bool eyeHov = (g_hoverEyeCard == i);
            if (eyeHov && gg) {
                FillRound(gg, (float)rEye.left, (float)rEye.top, (float)(rEye.right - rEye.left), (float)(rEye.bottom - rEye.top),
                          (float)Px(4), g_light ? MakeArgb(25, 0, 0, 0) : MakeArgb(35, 255, 255, 255));
            }
            SelectObject(dc, g_fonts.btn);
            SetTextColor(dc, e.hiddenOnTaskbar ? RGB(235, 87, 87) : txtSub);
            wchar_t eyeGlyph[2] = { (wchar_t)(e.hiddenOnTaskbar ? 0xED1A : 0xE7B3), 0 };
            DrawTextW(dc, eyeGlyph, 1, &rEye, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            // Device Name
            SelectObject(dc, g_fonts.name);
            SetTextColor(dc, txtMain);
            RECT rName{ icX + icS + Px(12), c.top + Px(10), c.right - Px(95), c.top + Px(30) };
            std::wstring dispName = TranslateDeviceName(d.name);
            DrawTextW(dc, dispName.c_str(), (int)dispName.size(), &rName, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

            // Connection type + mV / detail + ETA
            SelectObject(dc, g_fonts.meta);
            SetTextColor(dc, txtSub);
            std::wstring meta = d.via.empty() ? T(L"Wireless device") : TranslateVia(d.via);
            if (!d.detail.empty()) meta += L" \x2022 " + TranslateDetail(d.detail);
            else if (d.millivolts > 0) meta += L" \x2022 " + std::to_wstring(d.millivolts) + L" mV";
            if (!e.etaText.empty() && d.connected && !d.asleep) meta += L" \x2022 " + e.etaText;
            if (e.hiddenOnTaskbar) meta += std::wstring(L" (") + T(L"hidden") + L")";
            RECT rMeta{ icX + icS + Px(12), c.top + Px(30), c.right - Px(95), c.top + Px(48) };
            DrawTextW(dc, meta.c_str(), (int)meta.size(), &rMeta, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

            // Status Badge Pill
            std::wstring badge = StatusBadgeText(d);
            SelectObject(dc, g_fonts.badge);
            SIZE bsz{};
            GetTextExtentPoint32W(dc, badge.c_str(), (int)badge.size(), &bsz);
            int bw = bsz.cx + Px(14), bh = Px(18);
            int bx = icX + icS + Px(12), by = c.top + Px(50);

            ARGB badgeBg;
            COLORREF badgeFg;
            if (!d.connected) {
                badgeBg = MakeArgb(40, 140, 140, 150);
                badgeFg = txtSub;
            } else if (d.charging) {
                badgeBg = MakeArgb(45, 46, 204, 113);
                badgeFg = g_light ? RGB(20, 130, 70) : RGB(80, 230, 140);
            } else if (d.asleep) {
                badgeBg = MakeArgb(45, 241, 196, 15);
                badgeFg = g_light ? RGB(160, 110, 0) : RGB(250, 210, 75);
            } else if (d.percent >= 0 && d.percent <= 20) {
                badgeBg = MakeArgb(50, 235, 65, 55);
                badgeFg = g_light ? RGB(190, 30, 25) : RGB(255, 110, 100);
            } else {
                badgeBg = MakeArgb(40, 46, 204, 113);
                badgeFg = g_light ? RGB(20, 130, 70) : RGB(80, 230, 140);
            }
            FillRound(gg, (float)bx, (float)by, (float)bw, (float)bh, (float)Px(9), badgeBg);
            SetTextColor(dc, badgeFg);
            RECT rBadge{ bx, by, bx + bw, by + bh };
            DrawTextW(dc, badge.c_str(), (int)badge.size(), &rBadge, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            // Mini sparkline next to badge when history is available (P4-2)
            if (e.history.size() >= 2 && bx + bw + Px(8) < c.right - Px(95)) {
                DrawSparkline(gg, (float)(bx + bw + Px(8)), (float)(by + Px(3)),
                              (float)Px(42), (float)(bh - Px(6)), e.history, accentArgb);
            }

            // Right Side: Large Battery Percentage
            SelectObject(dc, g_fonts.pct);
            std::wstring pctStr;
            if (!d.connected || d.percent < 0) pctStr = L"--";
            else if (!d.levelText.empty()) pctStr = TranslateLevel(d.levelText);
            else pctStr = std::to_wstring(d.percent) + L"%";

            COLORREF pctCol = txtMain;
            if (!d.connected || d.percent < 0) pctCol = txtSub;
            else if (d.charging) pctCol = g_light ? RGB(20, 140, 75) : RGB(72, 225, 135);
            else if (d.percent <= 10) pctCol = g_light ? RGB(200, 30, 25) : RGB(255, 95, 86);
            else if (d.percent <= 25) pctCol = g_light ? RGB(185, 110, 0) : RGB(255, 185, 50);

            SetTextColor(dc, pctCol);
            RECT rPct{ c.right - Px(92), c.top + Px(24), c.right - Px(14), c.top + Px(56) };
            DrawTextW(dc, pctStr.c_str(), (int)pctStr.size(), &rPct, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            // Bottom Progress Bar inside Card
            int barX = c.left + Px(16), barY = c.bottom - Px(14);
            int barW = (c.right - c.left) - Px(32), barH = Px(6);
            ARGB trackCol = g_light ? MakeArgb(30, 0, 0, 0) : MakeArgb(40, 255, 255, 255);
            FillRound(gg, (float)barX, (float)barY, (float)barW, (float)barH, (float)Px(3), trackCol);

            if (d.connected && d.percent >= 0) {
                int fillW = std::max(Px(6), (barW * std::clamp(d.percent, 0, 100)) / 100);
                ARGB fillCol;
                BYTE alpha = d.asleep ? 150 : 240;
                if (d.charging)          fillCol = MakeArgb(alpha, 46, 204, 113);
                else if (d.percent <= 10) fillCol = MakeArgb(alpha, 235, 65, 55);
                else if (d.percent <= 25) fillCol = MakeArgb(alpha, 245, 166, 35);
                else                      fillCol = MakeArgb(alpha, 46, 204, 113);
                FillRound(gg, (float)barX, (float)barY, (float)fillW, (float)barH, (float)Px(3), fillCol);
            }
        }
    }

    RestoreDC(dc, clipId);

    // ---- Footer ----
    SelectObject(dc, g_fonts.sub);
    SetTextColor(dc, txtSub);
    RECT rFoot{ PadX(), H - FooterHeight(), W - PadX(), H - Px(4) };

    int connectedCount = 0;
    std::wstring customHint;
    for (const auto& e : g_entries) {
        if (e.status.connected && e.status.percent >= 0) connectedCount++;
        if (customHint.empty() && !e.status.hint.empty()) customHint = e.status.hint;
    }
    wchar_t footBuf[160] = {};
    if (!customHint.empty()) {
        swprintf_s(footBuf, L"%d/%zu %s \x2022 %s", connectedCount, g_entries.size(), T(L"Active"), customHint.c_str());
    } else if (IsTurkish()) {
        swprintf_s(footBuf, L"%d/%zu cihaz aktif \x2022 v%s \x2022 Sa\x011F t\x0131k: Cihaz men\x00FCs\x00FC",
                   connectedCount, g_entries.size(), APP_VERSION_WSTR);
    } else {
        swprintf_s(footBuf, L"%d/%zu active \x2022 v%s \x2022 Right-click: Device menu",
                   connectedCount, g_entries.size(), APP_VERSION_WSTR);
    }
    DrawTextW(dc, footBuf, -1, &rFoot, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    // Subtle outer border
    StrokeRound(gg, 0.5f, 0.5f, (float)W - 1.0f, (float)H - 1.0f, (float)Px(10), 1.0f,
                g_light ? MakeArgb(45, 0, 0, 0) : MakeArgb(55, 255, 255, 255));

    SelectObject(dc, oldFont);
    if (gg) g_gdip.DeleteGraphics(gg);

    BitBlt(targetDc, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

void ApplyWin11Styling(HWND wnd) {
    DWORD corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(wnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    BOOL dark = g_light ? FALSE : TRUE;
    DwmSetWindowAttribute(wnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

int CardAtPoint(POINT pt, int W) {
    if (g_showSettings || pt.y < HeaderHeight()) return -1;
    for (int i = 0; i < (int)g_entries.size(); i++) {
        RECT r = CardRect(i, W);
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

int BtnAtPoint(POINT pt, int W) {
    RECT r1 = RefreshBtnRect(W);
    if (PtInRect(&r1, pt)) return 1;
    RECT r2 = SettingsBtnRect(W);
    if (PtInRect(&r2, pt)) return 2;
    RECT r3 = CloseBtnRect(W);
    if (PtInRect(&r3, pt)) return 3;
    return 0;
}

void HandleSettingClick(const SettingHit& hit) {
    switch (hit.action) {
        case 1: // Poll interval
            settings::g_pollIntervalSec.store(hit.param, std::memory_order_relaxed);
            settings::Save();
            if (g_owner && g_actionMsg) PostMessageW(g_owner, g_actionMsg, kActionRefresh, 0);
            break;
        case 2: // Low battery threshold
            settings::g_lowBatteryThreshold = hit.param;
            settings::Save();
            break;
        case 3: // Notification toggles
            if (hit.param == 1) settings::g_notifyLowBattery = !settings::g_notifyLowBattery;
            if (hit.param == 2) settings::g_notifyFullCharge = !settings::g_notifyFullCharge;
            if (hit.param == 3) settings::g_notifyConnect    = !settings::g_notifyConnect;
            settings::Save();
            break;
        case 4: // Run at startup
            settings::SetAutoStart(!settings::IsAutoStartEnabled());
            break;
        case 5: // Language (0 = tr, 1 = en)
            SetLanguage(hit.param == 0 ? L"tr" : L"en");
            SetWindowTextW(g_wnd, T(L"Wireless Battery"));
            if (g_owner && g_actionMsg) PostMessageW(g_owner, g_actionMsg, kActionSettingsChanged, 0);
            break;
        case 6: // Reset hidden devices
            settings::g_hiddenDevices.clear();
            settings::Save();
            if (g_owner && g_actionMsg) PostMessageW(g_owner, g_actionMsg, kActionSettingsChanged, 0);
            break;
    }
    InvalidateRect(g_wnd, nullptr, FALSE);
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            PaintPanel(h, dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            RECT rc;
            GetClientRect(h, &rc);
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int b = BtnAtPoint(pt, rc.right);
            int c = CardAtPoint(pt, rc.right);
            int eye = -1;
            if (c >= 0) {
                RECT rEye = CardEyeRect(CardRect(c, rc.right));
                if (PtInRect(&rEye, pt)) eye = c;
            }
            int sRow = -1;
            if (g_showSettings) {
                for (int i = 0; i < (int)g_settingHits.size(); i++) {
                    if (PtInRect(&g_settingHits[i].rc, pt)) { sRow = i; break; }
                }
            }
            if (b != g_hoverBtn || c != g_hoverCard || eye != g_hoverEyeCard || sRow != g_hoverSettingRow) {
                g_hoverBtn = b;
                g_hoverCard = c;
                g_hoverEyeCard = eye;
                g_hoverSettingRow = sRow;
                InvalidateRect(h, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (g_hoverBtn != 0 || g_hoverCard != -1 || g_hoverEyeCard != -1 || g_hoverSettingRow != -1) {
                g_hoverBtn = 0;
                g_hoverCard = -1;
                g_hoverEyeCard = -1;
                g_hoverSettingRow = -1;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSEWHEEL: {
            RECT rc;
            GetClientRect(h, &rc);
            int viewH = rc.bottom - HeaderHeight() - FooterHeight();
            int maxScroll = std::max(0, g_contentHeight - viewH);
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            int nextScroll = std::clamp(g_scrollY - (delta / WHEEL_DELTA) * Px(36), 0, maxScroll);
            if (nextScroll != g_scrollY) {
                g_scrollY = nextScroll;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            RECT rc;
            GetClientRect(h, &rc);
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int b = BtnAtPoint(pt, rc.right);
            if (b == 1) { // Refresh
                if (g_owner && g_actionMsg) PostMessageW(g_owner, g_actionMsg, kActionRefresh, 0);
                return 0;
            }
            if (b == 2) { // Toggle in-Flyout Settings panel (P1-6)
                g_showSettings = !g_showSettings;
                g_scrollY = 0;
                Hide();
                Toggle(g_owner);
                return 0;
            }
            if (b == 3) { // Close
                Hide();
                return 0;
            }
            if (g_showSettings) {
                for (const auto& hit : g_settingHits) {
                    if (PtInRect(&hit.rc, pt)) {
                        HandleSettingClick(hit);
                        return 0;
                    }
                }
                return 0;
            }
            int c = CardAtPoint(pt, rc.right);
            if (c >= 0 && c < (int)g_entries.size()) {
                RECT rEye = CardEyeRect(CardRect(c, rc.right));
                if (PtInRect(&rEye, pt)) {
                    // Toggle taskbar visibility directly from card eye button (P4-5)
                    const std::wstring& k = g_entries[c].status.key;
                    if (!settings::g_hiddenDevices.erase(k)) settings::g_hiddenDevices.insert(k);
                    settings::Save();
                    if (g_owner && g_actionMsg) PostMessageW(g_owner, g_actionMsg, kActionSettingsChanged, 0);
                    return 0;
                }
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            RECT rc;
            GetClientRect(h, &rc);
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int c = CardAtPoint(pt, rc.right);
            if (c >= 0 && c < (int)g_entries.size() && g_owner && g_actionMsg) {
                PostMessageW(g_owner, g_actionMsg, (WPARAM)c, 0);
            } else if (g_owner && g_actionMsg) {
                PostMessageW(g_owner, g_actionMsg, kActionGeneralMenu, 0);
            }
            return 0;
        }
        case WM_KEYDOWN: {
            int totalItems = (int)g_entries.size() + 3; // cards + 3 header buttons
            if (wp == VK_ESCAPE) {
                Hide();
                return 0;
            }
            if (wp == VK_F5) {
                if (g_owner && g_actionMsg) PostMessageW(g_owner, g_actionMsg, kActionRefresh, 0);
                return 0;
            }
            if (wp == VK_TAB || wp == VK_DOWN) {
                g_focusIndex = (g_focusIndex + 1) % std::max(1, totalItems);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (wp == VK_UP) {
                g_focusIndex = (g_focusIndex <= 0) ? (totalItems - 1) : (g_focusIndex - 1);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (wp == VK_RETURN || wp == VK_SPACE) {
                int nCards = (int)g_entries.size();
                if (g_focusIndex >= 0 && g_focusIndex < nCards) {
                    if (g_owner && g_actionMsg) PostMessageW(g_owner, g_actionMsg, (WPARAM)g_focusIndex, 0);
                } else if (g_focusIndex == nCards) {
                    if (g_owner && g_actionMsg) PostMessageW(g_owner, g_actionMsg, kActionRefresh, 0);
                } else if (g_focusIndex == nCards + 1) {
                    g_showSettings = !g_showSettings;
                    g_scrollY = 0;
                    Hide();
                    Toggle(g_owner);
                } else if (g_focusIndex == nCards + 2) {
                    Hide();
                }
                return 0;
            }
            break;
        }
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                g_deactivatedAt = GetTickCount64();
                Hide();
            }
            return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

}  // namespace

void Create(HINSTANCE hInst, HWND ownerWnd, UINT /*trayCallbackMsg*/, UINT actionMsg) {
    g_inst = hInst;
    g_owner = ownerWnd;
    g_actionMsg = actionMsg;

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
}

void Destroy() {
    if (g_wnd) {
        DestroyWindow(g_wnd);
        g_wnd = nullptr;
    }
    g_fonts.Clear();
}

void Update(const std::vector<Entry>& devices) {
    g_entries = devices;
    g_lastPollTick = GetTickCount64();
    if (g_wnd && IsWindowVisible(g_wnd)) {
        InvalidateRect(g_wnd, nullptr, FALSE);
    }
}

void Hide() {
    if (g_wnd && IsWindowVisible(g_wnd)) {
        ShowWindow(g_wnd, SW_HIDE);
    }
}

bool IsVisible() {
    return g_wnd && IsWindowVisible(g_wnd);
}

const std::vector<Entry>& OrderedEntries() {
    return g_entries;
}

std::wstring KeyAt(int index) {
    if (index < 0 || index >= (int)g_entries.size()) return {};
    return g_entries[index].status.key;
}

void Toggle(HWND trayFallbackWnd) {
    if (IsVisible()) {
        Hide();
        return;
    }
    // P0-2: If the Flyout just closed because the user clicked the tray icon or taskbar widget,
    // do not immediately reopen it on the same click.
    if (g_deactivatedAt != 0 && GetTickCount64() - g_deactivatedAt < 250) {
        g_deactivatedAt = 0;
        return;
    }

    RECT anchor{};
    bool gotAnchor = widget::GetScreenRect(anchor);
    if (!gotAnchor && trayFallbackWnd) {
        NOTIFYICONIDENTIFIER nii{ sizeof(nii) };
        nii.hWnd = trayFallbackWnd;
        nii.uID = 1;
        if (SUCCEEDED(Shell_NotifyIconGetRect(&nii, &anchor))) gotAnchor = true;
    }
    if (!gotAnchor) {
        POINT pt;
        GetCursorPos(&pt);
        anchor = { pt.x - 16, pt.y - 16, pt.x + 16, pt.y + 16 };
    }

    g_dpi = QueryMonitorDpi(anchor);
    g_light = IsLightTheme();

    // Multi-monitor work area (P1-5)
    HMONITOR mon = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    RECT wa{};
    if (mon && GetMonitorInfoW(mon, &mi)) {
        wa = mi.rcWork;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    }

    int W = PanelWidth();
    int H = DesiredPanelHeight(wa.bottom - wa.top);

    int cx = (anchor.left + anchor.right) / 2;
    int x = cx - W / 2;
    // Place below the taskbar if the taskbar / anchor is at the top of the monitor, otherwise above.
    bool topTaskbar = (anchor.bottom <= wa.top + Px(80));
    int y = topTaskbar ? std::max((int)wa.top + Px(8), (int)anchor.bottom + Px(8))
                       : std::min((int)wa.bottom - H - Px(8), (int)anchor.top - H - Px(8));

    if (x + W > wa.right - Px(8)) x = wa.right - W - Px(8);
    if (x < wa.left + Px(8))      x = wa.left + Px(8);
    if (y + H > wa.bottom - Px(8)) y = wa.bottom - H - Px(8);
    if (y < wa.top + Px(8))        y = wa.top + Px(8);

    if (!g_wnd) {
        g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClass, T(L"Wireless Battery"),
                                WS_POPUP, x, y, W, H, g_owner, nullptr, g_inst, nullptr);
    } else {
        SetWindowTextW(g_wnd, T(L"Wireless Battery"));
    }
    if (!g_wnd) return;

    g_scrollY = 0;
    g_focusIndex = -1;
    ApplyWin11Styling(g_wnd);
    SetWindowPos(g_wnd, HWND_TOPMOST, x, y, W, H, SWP_SHOWWINDOW);
    SetForegroundWindow(g_wnd);
    InvalidateRect(g_wnd, nullptr, FALSE);
}

}  // namespace flyout
