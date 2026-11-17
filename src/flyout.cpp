#include "flyout.h"

#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cstdio>

namespace {

const wchar_t* kFlyoutClass = L"WirelessBatteryFlyout";

HINSTANCE g_inst = nullptr;
HWND g_wnd = nullptr;
HWND g_notify = nullptr;
UINT g_actionMsg = 0;
bool g_light = false;
int g_pollSec = 60;
int g_lowBattPct = 15;
ULONGLONG g_refreshUntil = 0;

std::vector<FlyoutCard> g_cards;

struct HitZone {
    RECT rc{};
    UINT action = flyout::ACT_NONE;
};
std::vector<HitZone> g_hits;
int g_hoverIdx = -1;

int Dpi() {
    UINT d = g_wnd ? GetDpiForWindow(g_wnd) : GetDpiForSystem();
    return d ? (int)d : 96;
}

int Px(int v) { return MulDiv(v, Dpi(), 96); }

wchar_t CardGlyph(DeviceKind k) {
    switch (k) {
        case DeviceKind::Headset:  return 0xE7F6;  // Headphone
        case DeviceKind::Mouse:    return 0xE962;  // Mouse
        case DeviceKind::Keyboard: return 0xE765;  // KeyboardClassic
        case DeviceKind::Gamepad:  return 0xE7FC;  // Game
        default:                   return 0xE772;  // Devices
    }
}

HFONT MakeUiFont(int px, int weight, const wchar_t* preferred, const wchar_t* fallback) {
    for (const wchar_t* face : { preferred, fallback }) {
        HFONT h = CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
        HDC dc = CreateCompatibleDC(nullptr);
        HGDIOBJ o = SelectObject(dc, h);
        wchar_t got[LF_FACESIZE] = {};
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, o);
        DeleteDC(dc);
        if (!_wcsicmp(got, face) || face == fallback) return h;
        DeleteObject(h);
    }
    return nullptr;
}

void DrawRoundBox(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBr = SelectObject(dc, br);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, oldBr);
    SelectObject(dc, oldPen);
    DeleteObject(br);
    DeleteObject(pen);
}

int CalcHeight() {
    int pad = Px(14);
    int headerH = Px(46);
    int footerH = Px(46);
    int cardH = Px(106);
    int gap = Px(10);
    size_t count = std::max<size_t>(1, g_cards.size());
    return pad + headerH + (int)count * cardH + (int)(count - 1) * gap + Px(12) + footerH + pad;
}

void PaintFlyout(HDC targetDc, const RECT& client) {
    int W = client.right - client.left;
    int H = client.bottom - client.top;
    if (W <= 0 || H <= 0) return;

    HDC dc = CreateCompatibleDC(targetDc);
    HBITMAP bmp = CreateCompatibleBitmap(targetDc, W, H);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);

    // Theme colors
    COLORREF bgMain     = g_light ? RGB(243, 244, 246) : RGB(24, 25, 28);
    COLORREF borderMain = g_light ? RGB(209, 213, 219) : RGB(52, 55, 62);
    COLORREF cardBg     = g_light ? RGB(255, 255, 255) : RGB(34, 36, 42);
    COLORREF cardHover  = g_light ? RGB(249, 250, 251) : RGB(42, 45, 52);
    COLORREF cardBorder = g_light ? RGB(229, 231, 235) : RGB(50, 53, 62);
    COLORREF badgeBg    = g_light ? RGB(238, 242, 255) : RGB(45, 50, 62);
    COLORREF textPri    = g_light ? RGB(17, 24, 39)    : RGB(243, 244, 246);
    COLORREF textSec    = g_light ? RGB(107, 114, 128) : RGB(156, 163, 175);
    COLORREF trackBg    = g_light ? RGB(229, 231, 235) : RGB(55, 58, 66);
    COLORREF btnBg      = g_light ? RGB(255, 255, 255) : RGB(38, 41, 48);
    COLORREF btnHover   = g_light ? RGB(235, 238, 242) : RGB(52, 56, 66);

    COLORREF colGreen   = g_light ? RGB(22, 163, 74)   : RGB(52, 211, 153);
    COLORREF colOrange  = g_light ? RGB(217, 119, 6)   : RGB(251, 191, 36);
    COLORREF colRed     = g_light ? RGB(220, 38, 38)   : RGB(248, 113, 113);
    COLORREF colBlue    = g_light ? RGB(2, 132, 199)   : RGB(56, 189, 248);

    DrawRoundBox(dc, { 0, 0, W, H }, Px(12), bgMain, borderMain);

    HFONT fTitle = MakeUiFont(Px(15), FW_BOLD, L"Segoe UI Variable Display", L"Segoe UI");
    HFONT fCardTitle = MakeUiFont(Px(14), FW_SEMIBOLD, L"Segoe UI Variable Text", L"Segoe UI");
    HFONT fPct = MakeUiFont(Px(18), FW_BOLD, L"Segoe UI Variable Display", L"Segoe UI");
    HFONT fSub = MakeUiFont(Px(12), FW_NORMAL, L"Segoe UI Variable Text", L"Segoe UI");
    HFONT fSmall = MakeUiFont(Px(11), FW_NORMAL, L"Segoe UI Variable Text", L"Segoe UI");
    HFONT fIcon = MakeUiFont(Px(18), FW_NORMAL, L"Segoe Fluent Icons", L"Segoe MDL2 Assets");

    g_hits.clear();
    int pad = Px(14);
    int y = pad;

    // ---- Header ----
    SelectObject(dc, fTitle);
    SetTextColor(dc, textPri);
    RECT hdrLeft{ pad + Px(2), y, W - pad, y + Px(22) };
    DrawTextW(dc, L"Kablosuz Pil Merkezi", -1, &hdrLeft, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, fSmall);
    SetTextColor(dc, textSec);
    wchar_t hdrSub[128];
    swprintf(hdrSub, 128, L"ROG & ATK Özel Sürüm  •  Yenileme: %d sn  •  Uyarı: %%%d", g_pollSec, g_lowBattPct);
    RECT hdrSubRc{ pad + Px(2), y + Px(22), W - pad, y + Px(40) };
    DrawTextW(dc, hdrSub, -1, &hdrSubRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    y += Px(46);

    // ---- Device Cards ----
    int cardH = Px(106);
    int gap = Px(10);

    if (g_cards.empty()) {
        RECT cr{ pad, y, W - pad, y + cardH };
        DrawRoundBox(dc, cr, Px(10), cardBg, cardBorder);
        SelectObject(dc, fCardTitle);
        SetTextColor(dc, textPri);
        RECT t1{ cr.left + Px(16), cr.top + Px(26), cr.right - Px(16), cr.top + Px(50) };
        DrawTextW(dc, L"Pil bilgisi olan cihaz bulunamadı", -1, &t1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, fSub);
        SetTextColor(dc, textSec);
        RECT t2{ cr.left + Px(16), cr.top + Px(52), cr.right - Px(16), cr.top + Px(76) };
        DrawTextW(dc, L"ROG Harpe Ace veya ATK N9 Ultra alıcısını kontrol edin.", -1, &t2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        y += cardH;
    } else {
        for (size_t i = 0; i < g_cards.size(); i++) {
            const auto& c = g_cards[i];
            RECT cr{ pad, y, W - pad, y + cardH };
            UINT act = flyout::ACT_TOGGLE_DEVICE_BASE + (UINT)i;
            int hitIdx = (int)g_hits.size();
            g_hits.push_back({ cr, act });

            bool hovered = (g_hoverIdx == hitIdx);
            DrawRoundBox(dc, cr, Px(10), hovered ? cardHover : cardBg, cardBorder);

            // Icon Box
            RECT ib{ cr.left + Px(12), cr.top + Px(12), cr.left + Px(48), cr.top + Px(48) };
            DrawRoundBox(dc, ib, Px(8), badgeBg, badgeBg);
            SelectObject(dc, fIcon);
            COLORREF accent = c.charging ? colBlue : (c.low ? colRed : (c.percent >= 0 && c.percent <= 30 ? colOrange : colGreen));
            if (!c.connected || c.percent < 0) accent = textSec;
            SetTextColor(dc, accent);
            wchar_t glyphStr[2] = { CardGlyph(c.kind), 0 };
            DrawTextW(dc, glyphStr, 1, &ib, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            // Percentage on top-right
            SelectObject(dc, fPct);
            SetTextColor(dc, accent);
            std::wstring pctStr = c.percent >= 0 ? (L"%" + std::to_wstring(c.percent)) : L"—";
            if (c.charging) pctStr += L" ⚡";
            RECT pctRc{ cr.right - Px(95), cr.top + Px(10), cr.right - Px(14), cr.top + Px(34) };
            DrawTextW(dc, pctStr.c_str(), -1, &pctRc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

            // Device Name
            SelectObject(dc, fCardTitle);
            SetTextColor(dc, c.hidden ? textSec : textPri);
            RECT nameRc{ ib.right + Px(10), cr.top + Px(11), cr.right - Px(95), cr.top + Px(31) };
            DrawTextW(dc, c.name.c_str(), -1, &nameRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            // Connection & Status subtitle
            SelectObject(dc, fSub);
            SetTextColor(dc, textSec);
            std::wstring subLine = c.via + L"  •  " + c.statusText;
            RECT subRc{ ib.right + Px(10), cr.top + Px(31), cr.right - Px(14), cr.top + Px(49) };
            DrawTextW(dc, subLine.c_str(), -1, &subRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            // Progress Bar
            RECT barTrack{ cr.left + Px(12), cr.top + Px(58), cr.right - Px(12), cr.top + Px(66) };
            DrawRoundBox(dc, barTrack, Px(4), trackBg, trackBg);
            if (c.percent > 0) {
                int barW = barTrack.right - barTrack.left;
                int fillW = std::max(Px(8), (barW * std::min(100, c.percent)) / 100);
                RECT barFill{ barTrack.left, barTrack.top, barTrack.left + fillW, barTrack.bottom };
                DrawRoundBox(dc, barFill, Px(4), accent, accent);
            }

            // Bottom row: ETA / Telemetry + Visibility status
            SelectObject(dc, fSmall);
            SetTextColor(dc, textSec);
            RECT etaRc{ cr.left + Px(12), cr.top + Px(74), cr.right - Px(95), cr.bottom - Px(8) };
            DrawTextW(dc, c.etaText.c_str(), -1, &etaRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            std::wstring visStr = c.hidden ? L"Gizli (Göster)" : L"Görev çubuğunda";
            RECT visRc{ cr.right - Px(110), cr.top + Px(74), cr.right - Px(12), cr.bottom - Px(8) };
            DrawTextW(dc, visStr.c_str(), -1, &visRc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

            y += cardH + gap;
        }
        y -= gap;
    }

    y += Px(12);

    // ---- Footer Action Buttons ----
    int btnGap = Px(10);
    int btnW = (W - pad * 2 - btnGap) / 2;
    int btnH = Px(38);

    RECT btnRefresh{ pad, y, pad + btnW, y + btnH };
    int idxRef = (int)g_hits.size();
    g_hits.push_back({ btnRefresh, flyout::ACT_REFRESH });
    DrawRoundBox(dc, btnRefresh, Px(8), g_hoverIdx == idxRef ? btnHover : btnBg, cardBorder);

    RECT btnSettings{ pad + btnW + btnGap, y, W - pad, y + btnH };
    int idxSet = (int)g_hits.size();
    g_hits.push_back({ btnSettings, flyout::ACT_SETTINGS });
    DrawRoundBox(dc, btnSettings, Px(8), g_hoverIdx == idxSet ? btnHover : btnBg, cardBorder);

    SelectObject(dc, fSub);
    SetTextColor(dc, textPri);
    bool refreshing = GetTickCount64() < g_refreshUntil;
    DrawTextW(dc, refreshing ? L"⏳ Yenileniyor..." : L"🔄 Şimdi Yenile", -1, &btnRefresh, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DrawTextW(dc, L"⚙️ Ayarlar ve Menü", -1, &btnSettings, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    BitBlt(targetDc, 0, 0, W, H, dc, 0, 0, SRCCOPY);

    SelectObject(dc, oldBmp);
    DeleteObject(fTitle);
    DeleteObject(fCardTitle);
    DeleteObject(fPct);
    DeleteObject(fSub);
    DeleteObject(fSmall);
    DeleteObject(fIcon);
    DeleteObject(bmp);
    DeleteDC(dc);
}

int HitTest(POINT pt) {
    for (size_t i = 0; i < g_hits.size(); i++) {
        if (PtInRect(&g_hits[i].rc, pt)) return (int)i;
    }
    return -1;
}

void ApplyWin11Rounding(HWND h) {
    HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dwm) return;
    using DwmSetAttrFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    if (auto fn = reinterpret_cast<DwmSetAttrFn>(GetProcAddress(dwm, "DwmSetWindowAttribute"))) {
        DWORD round = 2;  // DWMWCP_ROUND
        fn(h, 33, &round, sizeof(round));  // DWMWA_WINDOW_CORNER_PREFERENCE
        BOOL dark = g_light ? FALSE : TRUE;
        fn(h, 20, &dark, sizeof(dark));    // DWMWA_USE_IMMERSIVE_DARK_MODE
    }
    FreeLibrary(dwm);
}

LRESULT CALLBACK FlyoutProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            PaintFlyout(dc, rc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int idx = HitTest(pt);
            if (idx != g_hoverIdx) {
                g_hoverIdx = idx;
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
                TrackMouseEvent(&tme);
                InvalidateRect(h, nullptr, FALSE);
            }
            SetCursor(LoadCursorW(nullptr, idx >= 0 ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_MOUSELEAVE:
            if (g_hoverIdx != -1) {
                g_hoverIdx = -1;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int idx = HitTest(pt);
            if (idx >= 0 && idx < (int)g_hits.size()) {
                UINT act = g_hits[idx].action;
                if (act == flyout::ACT_REFRESH) {
                    flyout::NotifyRefreshing();
                }
                if (g_notify && g_actionMsg) {
                    PostMessageW(g_notify, g_actionMsg, act, 0);
                }
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) flyout::Hide();
            return 0;
        case WM_TIMER:
            if (wp == 1) {
                KillTimer(h, 1);
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                flyout::Hide();
            }
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

namespace flyout {

void Init(HINSTANCE inst, HWND notifyWnd, UINT actionMsg) {
    g_inst = inst;
    g_notify = notifyWnd;
    g_actionMsg = actionMsg;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = FlyoutProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kFlyoutClass;
        wc.style = CS_DROPSHADOW;
        registered = RegisterClassExW(&wc) != 0;
    }
}

void Destroy() {
    if (g_wnd) {
        DestroyWindow(g_wnd);
        g_wnd = nullptr;
    }
}

bool IsVisible() {
    return g_wnd && IsWindowVisible(g_wnd);
}

void Hide() {
    if (g_wnd && IsWindowVisible(g_wnd)) {
        ShowWindow(g_wnd, SW_HIDE);
        g_hoverIdx = -1;
    }
}

void SetData(const std::vector<FlyoutCard>& cards, int pollSec, int lowBattPct, bool lightTheme) {
    g_cards = cards;
    g_pollSec = pollSec;
    g_lowBattPct = lowBattPct;
    g_light = lightTheme;
    if (IsVisible()) {
        RECT r;
        GetWindowRect(g_wnd, &r);
        int W = Px(360);
        int H = CalcHeight();
        SetWindowPos(g_wnd, HWND_TOPMOST, r.right - W, r.bottom - H, W, H, SWP_NOACTIVATE);
        InvalidateRect(g_wnd, nullptr, FALSE);
    }
}

void NotifyRefreshing() {
    g_refreshUntil = GetTickCount64() + 1500;
    if (g_wnd && IsWindow(g_wnd)) {
        SetTimer(g_wnd, 1, 1550, nullptr);
        InvalidateRect(g_wnd, nullptr, FALSE);
    }
}

void Toggle(const RECT& anchorRect, bool lightTheme) {
    g_light = lightTheme;
    if (IsVisible()) {
        Hide();
        return;
    }
    if (!g_wnd) {
        g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kFlyoutClass, L"Kablosuz Pil Merkezi",
                                WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, g_inst, nullptr);
        if (!g_wnd) return;
    }
    ApplyWin11Rounding(g_wnd);

    int W = Px(360);
    int H = CalcHeight();

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);

    int cx = (anchorRect.left + anchorRect.right) / 2;
    int x = cx - W / 2;
    int y = anchorRect.top - H - Px(8);

    if (x + W > work.right - Px(8)) x = work.right - W - Px(8);
    if (x < work.left + Px(8)) x = work.left + Px(8);
    if (y < work.top + Px(8)) y = work.bottom - H - Px(8);

    g_hoverIdx = -1;
    SetWindowPos(g_wnd, HWND_TOPMOST, x, y, W, H, SWP_SHOWWINDOW);
    InvalidateRect(g_wnd, nullptr, FALSE);
    SetForegroundWindow(g_wnd);
}

}  // namespace flyout
