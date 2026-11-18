// Taskbar battery indicator.
// Sits natively inside the Windows taskbar right next to the system tray (ReBarWindow32 / TrayNotifyWnd),
// plus secondary taskbars (Shell_SecondaryTrayWnd) on multi-monitor setups.
// Left-clicking posts `g_clickMsg` with `wParam = 0` (toggle Flyout); right-clicking posts `wParam = 1` (Context Menu).
#include "widget.h"
#include "i18n.h"
#include "platform_compat.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <objbase.h>
#include <uiautomation.h>
#include <uxtheme.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace widget {
namespace {

constexpr const wchar_t* kClass = L"WirelessBatteryTaskbarWidget";
constexpr UINT_PTR kPositionTimer = 1;
constexpr COLORREF kChromaKey = RGB(1, 0, 1);
constexpr const wchar_t* kRegKey = L"Software\\WirelessBattery";

struct Bar {
    HWND tray = nullptr;
    HWND wnd  = nullptr;
    HWND tip  = nullptr;
    bool child = false;
    RECT lastRc{};
    RECT chevronRc{};
    DWORD chevronTick = 0;
};

HINSTANCE g_inst = nullptr;
HWND g_owner = nullptr;
UINT g_clickMsg = 0;
std::vector<Bar> g_bars;
std::vector<Entry> g_devices;
std::wstring g_tipText;
int g_dpi = 96;
bool g_lightTheme = false;
bool g_vertical = false;
bool g_showNumbers = true;
bool g_colorPct = false;
bool g_shortNames = false;
bool g_showOff = false;
bool g_trayBars = false;
std::map<std::wstring, int> g_accentPrefs;

bool LoadOption(const wchar_t* name, bool def) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return def;
    DWORD val = 0, sz = sizeof(val), type = 0;
    bool ok = RegQueryValueExW(k, name, nullptr, &type, (BYTE*)&val, &sz) == ERROR_SUCCESS && type == REG_DWORD;
    RegCloseKey(k);
    return ok ? (val != 0) : def;
}

void SaveOption(const wchar_t* name, bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    DWORD val = on ? 1 : 0;
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
    RegCloseKey(k);
}

void LoadAccentPrefs() {
    g_accentPrefs.clear();
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    DWORD sz = 0, type = 0;
    if (RegQueryValueExW(k, L"DeviceColors", nullptr, &type, nullptr, &sz) == ERROR_SUCCESS && type == REG_SZ && sz >= sizeof(wchar_t)) {
        std::wstring s(sz / sizeof(wchar_t), L'\0');
        RegQueryValueExW(k, L"DeviceColors", nullptr, nullptr, (BYTE*)s.data(), &sz);
        while (!s.empty() && s.back() == L'\0') s.pop_back();
        for (size_t i = 0; i < s.size();) {
            size_t j = s.find(L';', i);
            if (j == std::wstring::npos) j = s.size();
            size_t eq = s.find(L'=', i);
            if (eq != std::wstring::npos && eq < j)
                g_accentPrefs[s.substr(i, eq - i)] = _wtoi(s.substr(eq + 1, j - eq - 1).c_str());
            i = j + 1;
        }
    }
    RegCloseKey(k);
}

void SaveAccentPrefs() {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    std::wstring s;
    for (const auto& [key, val] : g_accentPrefs) {
        if (!s.empty()) s += L';';
        s += key + L'=' + std::to_wstring(val);
    }
    RegSetValueExW(k, L"DeviceColors", 0, REG_SZ, (const BYTE*)s.c_str(), (DWORD)((s.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}

int Px(int v) { return MulDiv(v, g_dpi, 96); }

UINT QueryDpi(HWND w) {
    using Fn = UINT(WINAPI*)(HWND);
    static Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    if (fn && w) return fn(w);
    HDC dc = GetDC(nullptr);
    int d = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return d ? d : 96;
}

bool IsTaskbarLight() {
    DWORD val = 0, sz = sizeof(val);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &val, &sz) == ERROR_SUCCESS)
        return val != 0;
    return false;
}

std::wstring BuildTooltip() {
    std::wstring t;
    for (const auto& e : g_devices) {
        const auto& d = e.status;
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
        if (!d.via.empty())  t += L"  [" + TranslateVia(d.via) + L"]";
    }
    return t;
}

void SyncTooltip(Bar& b) {
    if (!b.tip || !b.wnd) return;
    TOOLINFOW ti{ sizeof(ti) };
    ti.hwnd = b.wnd;
    ti.uId = 1;
    ti.lpszText = const_cast<wchar_t*>(g_tipText.c_str());
    SendMessageW(b.tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    SendMessageW(b.tip, TTM_SETMAXTIPWIDTH, 0, Px(600));
    GetClientRect(b.wnd, &ti.rect);
    SendMessageW(b.tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
}

// ---- GDI+ helpers (loaded dynamically so no extra import archive is needed) --------------------

struct GpBitmap;
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
    GpStatus (WINAPI* FillRect)(GpGraphics*, GpBrush*, float, float, float, float) = nullptr;

    bool Init() {
        if (mod) return true;
        mod = LoadSystemLibrary(L"gdiplus.dll");
        if (!mod) return false;
        auto load = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::decay_t<decltype(fn)>>(reinterpret_cast<void*>(GetProcAddress(mod, name)));
        };
        load(Startup,          "GdiplusStartup");
        load(CreateFromHDC,    "GdipCreateFromHDC");
        load(DeleteGraphics,   "GdipDeleteGraphics");
        load(SetSmoothing,     "GdipSetSmoothingMode");
        load(CreateSolidFill,  "GdipCreateSolidFill");
        load(DeleteBrush,      "GdipDeleteBrush");
        load(CreatePen1,       "GdipCreatePen1");
        load(DeletePen,        "GdipDeletePen");
        load(CreatePath,       "GdipCreatePath");
        load(DeletePath,       "GdipDeletePath");
        load(AddPathArc,       "GdipAddPathArc");
        load(AddPathLine,      "GdipAddPathLine");
        load(ClosePath,        "GdipClosePathFigure");
        load(FillPath,         "GdipFillPath");
        load(DrawPath,         "GdipDrawPath");
        load(FillRect,         "GdipFillRectangle");
        if (!Startup || !CreateFromHDC) return false;
        GdiplusStartupInput in;
        return Startup(&token, &in, nullptr) == 0;
    }
} g_gdip;

ARGB MakeArgb(BYTE a, BYTE r, BYTE g, BYTE b) {
    return ((ARGB)a << 24) | ((ARGB)r << 16) | ((ARGB)g << 8) | (ARGB)b;
}

GpPath* RoundRectPath(float x, float y, float w, float h, float r) {
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

void FillRoundRect(GpGraphics* g, float x, float y, float w, float h, float r, ARGB col) {
    if (w <= 0.0f || h <= 0.0f) return;
    GpPath* path = RoundRectPath(x, y, w, h, r);
    GpBrush* br = nullptr;
    g_gdip.CreateSolidFill(col, &br);
    if (path && br) g_gdip.FillPath(g, br, path);
    if (br) g_gdip.DeleteBrush(br);
    if (path) g_gdip.DeletePath(path);
}

void StrokeRoundRect(GpGraphics* g, float x, float y, float w, float h, float r, float stroke, ARGB col) {
    GpPath* path = RoundRectPath(x, y, w, h, r);
    GpPen* pen = nullptr;
    g_gdip.CreatePen1(col, stroke, UnitWorld, &pen);
    if (path && pen) g_gdip.DrawPath(g, pen, path);
    if (pen) g_gdip.DeletePen(pen);
    if (path) g_gdip.DeletePath(path);
}

void DrawBolt(GpGraphics* g, float cx, float cy, float h, ARGB col) {
    GpPath* p = nullptr;
    g_gdip.CreatePath(0, &p);
    if (!p) return;
    float w = h * 0.62f;
    float x0 = cx - w * 0.5f, y0 = cy - h * 0.5f;
    auto pt = [&](float u, float v, float u2, float v2) {
        g_gdip.AddPathLine(p, x0 + u * w, y0 + v * h, x0 + u2 * w, y0 + v2 * h);
    };
    pt(0.58f, 0.00f, 0.05f, 0.55f);
    pt(0.05f, 0.55f, 0.46f, 0.55f);
    pt(0.46f, 0.55f, 0.34f, 1.00f);
    pt(0.34f, 1.00f, 0.95f, 0.43f);
    pt(0.95f, 0.43f, 0.54f, 0.43f);
    g_gdip.ClosePath(p);
    GpBrush* br = nullptr;
    g_gdip.CreateSolidFill(col, &br);
    if (br) { g_gdip.FillPath(g, br, p); g_gdip.DeleteBrush(br); }
    g_gdip.DeletePath(p);
}

// ---- Fluent / MDL2 icon font + device drawing --------------------------------------------------

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

void DrawBatteryBar(GpGraphics* g, float x, float y, float w, float h, const DeviceStatus& d, bool light) {
    const float stroke = std::max(1.0f, Px(10) / 8.0f);
    const float nubW   = std::max(1.5f, (float)Px(2));
    const float nubH   = std::max(2.0f, std::round(h * 0.45f));
    const float bodyW  = w - nubW - stroke;
    const float r      = std::min(bodyW, h) * 0.30f;

    const BYTE fg = light ? 20 : 245;
    const BYTE frameA = (d.connected && !d.asleep && d.percent >= 0) ? 160 : 85;
    ARGB frameCol = MakeArgb(frameA, fg, fg, fg);

    StrokeRoundRect(g, x + stroke * 0.5f, y + stroke * 0.5f, bodyW - stroke, h - stroke, r, stroke, frameCol);
    FillRoundRect(g, x + bodyW, y + (h - nubH) * 0.5f, nubW, nubH, nubW * 0.4f, frameCol);

    if (!d.connected || d.percent < 0) return;

    const float pad = stroke + std::max(1.0f, Px(10) / 10.0f);
    const float ix = x + pad;
    const float iy = y + pad;
    const float iw = bodyW - pad * 2.0f;
    const float ih = h - pad * 2.0f;
    const float fillW = std::max(ih * 0.45f, iw * (std::clamp(d.percent, 0, 100) / 100.0f));

    ARGB fillCol;
    const BYTE a = d.asleep ? 120 : 240;
    if (d.charging)          fillCol = MakeArgb(a,  46, 204, 113);
    else if (d.percent <= 10) fillCol = MakeArgb(a, 235,  65,  55);
    else if (d.percent <= 25) fillCol = MakeArgb(a, 245, 166,  35);
    else                      fillCol = MakeArgb(a,  46, 204, 113);

    FillRoundRect(g, ix, iy, std::min(iw, fillW), ih, std::max(1.0f, r - pad * 0.6f), fillCol);

    if (d.charging) {
        ARGB boltCol = light ? MakeArgb(235, 20, 20, 20) : MakeArgb(245, 255, 255, 255);
        DrawBolt(g, x + bodyW * 0.5f, y + h * 0.5f, h * 1.35f, boltCol);
    }
}

std::wstring PctLabel(const DeviceStatus& d) {
    if (!d.connected || d.percent < 0) return L"--";
    if (!d.levelText.empty()) return TranslateLevel(d.levelText);
    return std::to_wstring(d.percent) + L"%";
}

COLORREF TextColorFor(const Entry& e, bool light) {
    const auto& d = e.status;
    if (!d.connected || d.percent < 0 || d.asleep)
        return light ? RGB(120, 120, 120) : RGB(155, 155, 155);
    if (g_colorPct) {
        if (d.percent <= 10 && !d.charging) return light ? RGB(196, 30, 24) : RGB(255, 95, 86);
        if (d.percent <= 25 && !d.charging) return light ? RGB(180, 105, 0) : RGB(255, 185, 50);
        return light ? RGB(18, 128, 65) : RGB(65, 215, 125);
    }
    if (e.accent != Accent::None) {
        BrandColor c = AccentColor(e.accent, light);
        return RGB(c.r, c.g, c.b);
    }
    if (d.percent <= 10 && !d.charging) return light ? RGB(196, 30, 24) : RGB(255, 95, 86);
    if (d.percent <= 25 && !d.charging) return light ? RGB(180, 105, 0) : RGB(255, 185, 50);
    return light ? RGB(20, 20, 20) : RGB(245, 245, 245);
}

HFONT MakeFont(int heightPx, int weight, const wchar_t* face) {
    return CreateFontW(-heightPx, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

int MeasureSlotWidth(HDC dc, const Entry& e) {
    const auto& d = e.status;
    int iconW = Px(15);
    int barW  = Px(20);
    if (!g_showNumbers) return std::max(iconW, barW) + Px(8);
    HFONT f = MakeFont(Px(11), FW_SEMIBOLD, L"Segoe UI");
    HGDIOBJ old = SelectObject(dc, f);
    std::wstring s = PctLabel(d);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    int topW = iconW;
    if (g_shortNames && !e.shortLabel.empty()) {
        SIZE lsz{};
        GetTextExtentPoint32W(dc, e.shortLabel.c_str(), (int)e.shortLabel.size(), &lsz);
        topW += Px(3) + lsz.cx;
    }
    SelectObject(dc, old);
    DeleteObject(f);
    return std::max({ topW, barW, (int)sz.cx }) + Px(8);
}

int TotalWidth() {
    if (g_devices.empty()) return 0;
    HDC dc = GetDC(nullptr);
    int w = Px(4);
    for (const auto& e : g_devices) w += MeasureSlotWidth(dc, e);
    ReleaseDC(nullptr, dc);
    return w;
}

int SlotHeight() { return Px(g_showNumbers ? 36 : 26); }

int TotalHeight() {
    if (g_devices.empty()) return 0;
    return Px(4) + (int)g_devices.size() * SlotHeight();
}

void PaintSlot(HDC dc, GpGraphics* gg, int x, int slotY, int sw, int sh, const Entry& e,
               HFONT iconFont, HFONT numFont, bool light) {
    const auto& d = e.status;
    COLORREF txtCol = TextColorFor(e, light);
    wchar_t glyph[2] = { Glyph(d.kind), 0 };

    if (!g_showNumbers) {
        const float barW = (float)Px(20);
        const float barH = (float)Px(7);
        const int totalH = Px(14) + Px(3) + (int)barH;
        const int topY   = slotY + (sh - totalH) / 2;

        SelectObject(dc, iconFont);
        SetTextColor(dc, txtCol);
        RECT rIcon{ x, topY, x + sw, topY + Px(15) };
        DrawTextW(dc, glyph, 1, &rIcon, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        DrawBatteryBar(gg, x + (sw - barW) * 0.5f, (float)(topY + Px(14) + Px(3)), barW, barH, d, light);
        return;
    }

    const int topH = Px(14);
    const int botH = Px(13);
    const int gap  = Px(1);
    const int topY = slotY + (sh - (topH + gap + botH)) / 2;
    const int botY = topY + topH + gap;

    SetTextColor(dc, txtCol);
    if (g_shortNames && !e.shortLabel.empty()) {
        int iconW = Px(14);
        SelectObject(dc, numFont);
        SIZE lsz{};
        GetTextExtentPoint32W(dc, e.shortLabel.c_str(), (int)e.shortLabel.size(), &lsz);
        int gapX = Px(2);
        int groupW = iconW + gapX + lsz.cx;
        int gx = x + (sw - groupW) / 2;

        SelectObject(dc, iconFont);
        RECT rIcon{ gx, topY, gx + iconW, topY + topH };
        DrawTextW(dc, glyph, 1, &rIcon, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        SelectObject(dc, numFont);
        RECT rLbl{ gx + iconW + gapX, topY, gx + groupW, topY + topH };
        DrawTextW(dc, e.shortLabel.c_str(), (int)e.shortLabel.size(), &rLbl, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {
        SelectObject(dc, iconFont);
        RECT rIcon{ x, topY, x + sw, topY + topH };
        DrawTextW(dc, glyph, 1, &rIcon, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    SelectObject(dc, numFont);
    std::wstring pct = PctLabel(d);
    SIZE tsz{};
    GetTextExtentPoint32W(dc, pct.c_str(), (int)pct.size(), &tsz);

    const float boltW = d.charging ? (float)Px(7) : 0.0f;
    const int totalW  = tsz.cx + (int)boltW;
    const int tx      = x + (sw - totalW) / 2 + (int)boltW;

    RECT rNum{ tx, botY, tx + tsz.cx + Px(2), botY + botH };
    DrawTextW(dc, pct.c_str(), (int)pct.size(), &rNum, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    if (d.charging && gg) {
        ARGB boltCol = MakeArgb(245, 46, 204, 113);
        DrawBolt(gg, (float)(tx - boltW * 0.55f), (float)(botY + botH * 0.5f), (float)Px(10), boltCol);
    }
}

void Paint(HWND wnd, HDC targetDc, bool child) {
    RECT rc;
    GetClientRect(wnd, &rc);
    int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) return;

    HDC dc = CreateCompatibleDC(targetDc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    DWORD* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(targetDc, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);

    bool parentCopied = false;
    if (child) {
        HWND parent = GetParent(wnd);
        if (parent) {
            POINT pt{ 0, 0 };
            MapWindowPoints(wnd, parent, &pt, 1);
            OffsetWindowOrgEx(dc, pt.x, pt.y, nullptr);
            LRESULT lr = SendMessageW(parent, WM_PRINTCLIENT, (WPARAM)dc, PRF_CLIENT | PRF_ERASEBKGND);
            SetWindowOrgEx(dc, 0, 0, nullptr);
            if (lr != 0) parentCopied = true;
            else {
                HDC pdc = GetDCEx(parent, nullptr, DCX_CACHE);
                if (pdc) {
                    parentCopied = BitBlt(dc, 0, 0, W, H, pdc, pt.x, pt.y, SRCCOPY) != 0;
                    ReleaseDC(parent, pdc);
                }
            }
        }
    }
    if (!parentCopied) {
        HBRUSH bg = CreateSolidBrush(child ? (g_lightTheme ? RGB(238, 238, 238) : RGB(24, 24, 24)) : kChromaKey);
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
    }

    GpGraphics* gg = nullptr;
    if (g_gdip.Init()) {
        g_gdip.CreateFromHDC(dc, &gg);
        if (gg) g_gdip.SetSmoothing(gg, SmoothingModeAntiAlias);
    }

    SetBkMode(dc, TRANSPARENT);
    HFONT iconFont = MakeFont(Px(g_showNumbers ? 12 : 13), FW_NORMAL, IconFontName());
    HFONT numFont  = MakeFont(Px(11), FW_SEMIBOLD, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, iconFont);

    if (g_vertical) {
        int sh = SlotHeight();
        int y = (H - (int)g_devices.size() * sh) / 2;
        if (y < Px(2)) y = Px(2);
        for (const auto& e : g_devices) {
            PaintSlot(dc, gg, 0, y, W, sh, e, iconFont, numFont, g_lightTheme);
            y += sh;
        }
    } else {
        int x = Px(2);
        for (const auto& e : g_devices) {
            int sw = MeasureSlotWidth(dc, e);
            PaintSlot(dc, gg, x, 0, sw, H, e, iconFont, numFont, g_lightTheme);
            x += sw;
        }
    }

    SelectObject(dc, oldFont);
    DeleteObject(iconFont);
    DeleteObject(numFont);
    if (gg) g_gdip.DeleteGraphics(gg);

    if (bits) {
        const DWORD chromaBgr = ((DWORD)GetBValue(kChromaKey) << 16) |
                                ((DWORD)GetGValue(kChromaKey) << 8)  |
                                 (DWORD)GetRValue(kChromaKey);
        const int n = W * H;
        for (int i = 0; i < n; i++) {
            DWORD rgb = bits[i] & 0x00FFFFFF;
            if (!child && !parentCopied && rgb == chromaBgr) bits[i] = chromaBgr;
            else                                             bits[i] = rgb | 0xFF000000;
        }
    }

    BitBlt(targetDc, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

void DisableWindowTransitions(HWND w) {
    BOOL off = TRUE;
    DwmSetWindowAttribute(w, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
    enum { WCA_CLOAK = 17 };
    struct ACCENT_POLICY { int state, flags; DWORD gradient; int animId; };
    struct WINCOMPATTRDATA { DWORD attr; PVOID data; SIZE_T size; };
    using SetWcaFn = BOOL(WINAPI*)(HWND, WINCOMPATTRDATA*);
    static SetWcaFn setWca = reinterpret_cast<SetWcaFn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute")));
    if (setWca) {
        ACCENT_POLICY pol{ 0, 0, 0, 0 };
        WINCOMPATTRDATA d{ 19 /*WCA_ACCENT_POLICY*/, &pol, sizeof(pol) };
        setWca(w, &d);
    }
}

void EnsureBarWindow(Bar& b, bool wantChild, int x, int y, int w, int h) {
    if (b.wnd && b.child != wantChild) {
        if (b.tip) { DestroyWindow(b.tip); b.tip = nullptr; }
        DestroyWindow(b.wnd);
        b.wnd = nullptr;
        b.lastRc = {};
    }
    if (!b.wnd) {
        b.child = wantChild;
        if (wantChild) {
            b.wnd = CreateWindowExW(WS_EX_NOPARENTNOTIFY, kClass, L"",
                                    WS_CHILD | WS_CLIPSIBLINGS,
                                    x, y, w, h, b.tray, nullptr, g_inst, nullptr);
        } else {
            b.wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                                    kClass, L"", WS_POPUP,
                                    x, y, w, h, nullptr, nullptr, g_inst, nullptr);
            SetLayeredWindowAttributes(b.wnd, kChromaKey, 0, LWA_COLORKEY);
            DisableWindowTransitions(b.wnd);
        }
        if (!b.wnd) return;

        b.tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                b.wnd, nullptr, g_inst, nullptr);
        if (b.tip) {
            SetWindowPos(b.tip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            TOOLINFOW ti{ sizeof(ti) };
            ti.uFlags = TTF_SUBCLASS;
            ti.hwnd = b.wnd;
            ti.uId = 1;
            ti.lpszText = const_cast<wchar_t*>(L"");
            GetClientRect(b.wnd, &ti.rect);
            SendMessageW(b.tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
            SendMessageW(b.tip, TTM_SETMAXTIPWIDTH, 0, Px(600));
        }
    }
}

bool FindXamlTrayOverflowLeft(HWND tray, RECT& outRc) {
    static IUIAutomation* uia = nullptr;
    static bool comInit = false;
    if (!comInit) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        comInit = true;
    }
    if (!uia) {
        if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IUIAutomation, (void**)&uia)))
            return false;
    }
    IUIAutomationElement* root = nullptr;
    if (FAILED(uia->ElementFromHandle(tray, &root)) || !root) return false;

    VARIANT v;
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(L"SystemTrayIcon");
    IUIAutomationCondition* cond = nullptr;
    uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, v, &cond);
    VariantClear(&v);

    bool found = false;
    if (cond) {
        IUIAutomationElementArray* arr = nullptr;
        if (SUCCEEDED(root->FindAll(TreeScope_Descendants, cond, &arr)) && arr) {
            int n = 0;
            arr->get_Length(&n);
            RECT best{ LONG_MAX, 0, LONG_MAX, 0 };
            for (int i = 0; i < n; i++) {
                IUIAutomationElement* el = nullptr;
                if (SUCCEEDED(arr->GetElement(i, &el)) && el) {
                    RECT r{};
                    if (SUCCEEDED(el->get_CurrentBoundingRectangle(&r)) &&
                        (r.right - r.left) > 4 && (r.bottom - r.top) > 4 && r.left < best.left) {
                        best = r;
                        found = true;
                    }
                    el->Release();
                }
            }
            if (found) outRc = best;
            arr->Release();
        }
        cond->Release();
    }
    root->Release();
    return found;
}

bool FindTrayLeftEdge(Bar& b, RECT& outRc) {
    HWND tray = b.tray;
    HWND notify = FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr);
    if (!notify) return false;

    HWND xaml = FindWindowExW(tray, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr);
    if (xaml) {
        DWORD now = GetTickCount();
        if (b.chevronTick == 0 || now - b.chevronTick > 3000) {
            b.chevronTick = now;
            if (!FindXamlTrayOverflowLeft(tray, b.chevronRc)) b.chevronRc = {};
        }
        if (b.chevronRc.right > b.chevronRc.left) {
            outRc = b.chevronRc;
            return true;
        }
    }

    HWND chev = FindWindowExW(notify, nullptr, L"Button", nullptr);
    if (chev && IsWindowVisible(chev)) {
        RECT r{};
        if (GetWindowRect(chev, &r) && (r.right - r.left) > 4 && (r.bottom - r.top) > 4) {
            outRc = r;
            return true;
        }
    }
    return GetWindowRect(notify, &outRc) != 0;
}

void PositionOneBar(Bar& b) {
    HWND tray = b.tray;
    if (!IsWindow(tray) || !IsWindowVisible(tray)) {
        if (b.wnd) ShowWindow(b.wnd, SW_HIDE);
        return;
    }

    HWND fg = GetForegroundWindow();
    if (fg && fg != tray && (!b.wnd || fg != b.wnd)) {
        wchar_t cls[64] = {};
        GetClassNameW(fg, cls, 64);
        if (wcscmp(cls, L"Progman") != 0 && wcscmp(cls, L"WorkerW") != 0 &&
            wcscmp(cls, L"Shell_TrayWnd") != 0 && wcscmp(cls, L"Shell_SecondaryTrayWnd") != 0) {
            RECT frc{}, trc{};
            GetWindowRect(fg, &frc);
            GetWindowRect(tray, &trc);
            HMONITOR mFg   = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
            HMONITOR mTray = MonitorFromWindow(tray, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{ sizeof(mi) };
            if (mFg == mTray && GetMonitorInfoW(mTray, &mi) &&
                frc.left <= mi.rcMonitor.left && frc.top <= mi.rcMonitor.top &&
                frc.right >= mi.rcMonitor.right && frc.bottom >= mi.rcMonitor.bottom) {
                if (b.wnd) ShowWindow(b.wnd, SW_HIDE);
                return;
            }
        }
    }

    RECT trayScreen{};
    GetWindowRect(tray, &trayScreen);
    bool vertical = (trayScreen.bottom - trayScreen.top) > (trayScreen.right - trayScreen.left);

    RECT edgeScreen{};
    if (!FindTrayLeftEdge(b, edgeScreen)) edgeScreen = trayScreen;
    HWND rebar = FindWindowExW(tray, nullptr, L"ReBarWindow32", nullptr);

    g_dpi = QueryDpi(tray);
    bool light = IsTaskbarLight();
    bool themeChanged = (light != g_lightTheme) || (vertical != g_vertical);
    g_lightTheme = light;
    g_vertical = vertical;

    int w = vertical ? (trayScreen.right - trayScreen.left) : TotalWidth();
    int h = vertical ? TotalHeight() : (trayScreen.bottom - trayScreen.top);

    bool classicTaskbar = FindWindowExW(tray, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr) == nullptr;
    bool useChild = classicTaskbar;

    int x, y;
    if (vertical) {
        x = useChild ? 0 : trayScreen.left;
        y = (useChild ? (edgeScreen.top - trayScreen.top) : edgeScreen.top) - h - Px(2);
    } else {
        x = (useChild ? (edgeScreen.left - trayScreen.left) : edgeScreen.left) - w - Px(2);
        y = useChild ? 0 : trayScreen.top;
    }

    EnsureBarWindow(b, useChild, x, y, w, h);
    if (!b.wnd) return;

    if (useChild && rebar && !vertical) {
        RECT rbScreen{};
        GetWindowRect(rebar, &rbScreen);
        int rbRight = rbScreen.right - trayScreen.left;
        if (rbRight > x - Px(4)) {
            int newRbW = std::max(100, (x - Px(4)) - (int)(rbScreen.left - trayScreen.left));
            SetWindowPos(rebar, nullptr, 0, 0, newRbW, rbScreen.bottom - rbScreen.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    RECT target{ x, y, x + w, y + h };
    bool moved = memcmp(&target, &b.lastRc, sizeof(RECT)) != 0;
    bool sizeChanged = (target.right - target.left) != (b.lastRc.right - b.lastRc.left) ||
                       (target.bottom - target.top) != (b.lastRc.bottom - b.lastRc.top);
    b.lastRc = target;

    HWND zOrder = useChild ? HWND_TOP : HWND_TOPMOST;
    UINT flags = SWP_NOACTIVATE | SWP_SHOWWINDOW | (moved ? 0 : SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(b.wnd, zOrder, x, y, w, h, flags);

    if (sizeChanged || themeChanged || useChild) {
        SyncTooltip(b);
        InvalidateRect(b.wnd, nullptr, FALSE);
    }
}

void PositionOnTaskbar() {
    if (g_devices.empty()) {
        for (auto& b : g_bars)
            if (b.wnd) ShowWindow(b.wnd, SW_HIDE);
        return;
    }

    std::vector<HWND> trays;
    if (HWND main = FindWindowW(L"Shell_TrayWnd", nullptr)) trays.push_back(main);
    for (HWND sec = nullptr; (sec = FindWindowExW(nullptr, sec, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr;)
        trays.push_back(sec);

    for (size_t i = g_bars.size(); i-- > 0;) {
        if (std::find(trays.begin(), trays.end(), g_bars[i].tray) == trays.end()) {
            if (g_bars[i].tip) DestroyWindow(g_bars[i].tip);
            if (g_bars[i].wnd) DestroyWindow(g_bars[i].wnd);
            g_bars.erase(g_bars.begin() + i);
        }
    }
    for (HWND t : trays) {
        if (std::find_if(g_bars.begin(), g_bars.end(), [t](const Bar& b) { return b.tray == t; }) == g_bars.end())
            g_bars.push_back(Bar{ t });
    }

    for (auto& b : g_bars) PositionOneBar(b);
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            bool child = false;
            for (const auto& b : g_bars)
                if (b.wnd == h) { child = b.child; break; }
            Paint(h, dc, child);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_PRINTCLIENT: {
            bool child = false;
            for (const auto& b : g_bars)
                if (b.wnd == h) { child = b.child; break; }
            Paint(h, (HDC)wp, child);
            return 0;
        }
        case WM_LBUTTONUP:
            if (g_owner && g_clickMsg) PostMessageW(g_owner, g_clickMsg, 0, 0); // 0 = Toggle Flyout
            return 0;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            if (g_owner && g_clickMsg) PostMessageW(g_owner, g_clickMsg, 1, 0); // 1 = Context Menu
            return 0;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        case WM_DPICHANGED:
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            for (auto& b : g_bars) b.lastRc = {};
            PositionOnTaskbar();
            return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

}  // namespace

void Create(HINSTANCE hInst, HWND ownerWnd, UINT clickMsg) {
    g_inst = hInst;
    g_owner = ownerWnd;
    g_clickMsg = clickMsg;
    g_showNumbers = LoadOption(L"ShowNumbers", true);
    g_colorPct    = LoadOption(L"ColorPct",    false);
    g_shortNames  = LoadOption(L"ShortNames",  false);
    g_showOff     = LoadOption(L"ShowOff",     false);
    g_trayBars    = LoadOption(L"TrayBars",    false);
    LoadAccentPrefs();

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_TAB_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    SetTimer(ownerWnd, kPositionTimer, 500, [](HWND, UINT, UINT_PTR, DWORD) {
        PositionOnTaskbar();
    });
}

void Destroy() {
    if (g_owner) KillTimer(g_owner, kPositionTimer);
    for (auto& b : g_bars) {
        if (b.tip) DestroyWindow(b.tip);
        if (b.wnd) DestroyWindow(b.wnd);
    }
    g_bars.clear();
}

void Update(const std::vector<Entry>& devices) {
    g_devices = devices;
    g_tipText = BuildTooltip();
    for (auto& b : g_bars) b.lastRc = {};
    PositionOnTaskbar();
    for (auto& b : g_bars) {
        SyncTooltip(b);
        if (b.wnd) InvalidateRect(b.wnd, nullptr, FALSE);
    }
}

bool GetScreenRect(RECT& out) {
    for (const auto& b : g_bars) {
        if (b.wnd && IsWindow(b.wnd) && IsWindowVisible(b.wnd)) {
            return GetWindowRect(b.wnd, &out) != 0;
        }
    }
    return false;
}

bool ShowNumbers() { return g_showNumbers; }
void SetShowNumbers(bool on) {
    g_showNumbers = on;
    SaveOption(L"ShowNumbers", on);
    Update(g_devices);
}

bool ColorPct() { return g_colorPct; }
void SetColorPct(bool on) {
    g_colorPct = on;
    SaveOption(L"ColorPct", on);
    Update(g_devices);
}

bool ShortNames() { return g_shortNames; }
void SetShortNames(bool on) {
    g_shortNames = on;
    SaveOption(L"ShortNames", on);
    Update(g_devices);
}

bool ShowOff() { return g_showOff; }
void SetShowOff(bool on) {
    g_showOff = on;
    SaveOption(L"ShowOff", on);
    Update(g_devices);
}

bool TrayBars() { return g_trayBars; }
void SetTrayBars(bool on) {
    g_trayBars = on;
    SaveOption(L"TrayBars", on);
}

int AccentPref(const std::wstring& key) {
    auto it = g_accentPrefs.find(key);
    return it == g_accentPrefs.end() ? -1 : it->second;
}

void SetAccentPref(const std::wstring& key, int pref) {
    if (pref < 0) g_accentPrefs.erase(key);
    else          g_accentPrefs[key] = pref;
    SaveAccentPrefs();
    Update(g_devices);
}

}  // namespace widget
