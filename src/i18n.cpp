#include "i18n.h"

#include <windows.h>
#include <cwctype>
#include <unordered_map>

namespace {

constexpr const wchar_t* kRegPath = L"Software\\WirelessBattery";

const std::vector<Language> kLanguages = {
    { L"tr",    L"T\x00FCrk\x00E7" L"e",      210 },
    { L"en",    L"English",                   0   },
    { L"de",    L"Deutsch",                   202 },
    { L"es",    L"Espa\x00F1ol",              203 },
    { L"fr",    L"Fran\x00E7" L"ais",         204 },
    { L"ja",    L"\x65E5\x672C\x8A9E",        206 },
    { L"pl",    L"Polski",                    207 },
    { L"pt-BR", L"Portugu\x00EAs (Brasil)",   208 },
    { L"ru",    L"\x0420\x0443\x0441\x0441\x043A\x0438\x0439", 209 },
    { L"ar",    L"\x0627\x0644\x0639\x0631\x0628\x064A\x0629", 201 },
    { L"he",    L"\x05E2\x05D1\x05E8\x05D9\x05EA", 205 },
    { L"zh-CN", L"\x7B80\x4F53\x4E2D\x6587",  211 },
    { L"zh-TW", L"\x7E41\x9AD4\x4E2D\x6587",  212 },
};

// Built-in Turkish fallback dictionary so Turkish translations work even in standalone unit tests
// without linked Win32 RCDATA resources.
const std::pair<const wchar_t*, const wchar_t*> kBuiltinTr[] = {
    { L"Wireless Battery", L"Kablosuz Pil \x0130zleyici" },
    { L"No wireless devices detected", L"Kablosuz cihaz alg\x0131lanmad\x0131" },
    { L"Connect your wireless mouse, keyboard, headset or gamepad", L"Kablosuz fare, klavye, kulakl\x0131k veya oyun kolunuzu ba\x011Flay\x0131n" },
    { L"charging", L"\x015Farj oluyor" },
    { L"Charging", L"\x015Earj oluyor" },
    { L"Discharging", L"De\x015Farj oluyor" },
    { L"Full", L"Dolu" },
    { L"High", L"Y\x00FCksek" },
    { L"Medium", L"Orta" },
    { L"Low", L"D\x00FC\x015F\x00FCk" },
    { L"Empty", L"Bo\x015F" },
    { L"Critical", L"Kritik!" },
    { L"Good", L"\x0130yi" },
    { L"sleeping", L"uyku modunda" },
    { L"Sleeping / Cached", L"Uyku Modu / \x00D6nbellek" },
    { L"not connected", L"ba\x011Fl\x0131 de\x011Fil" },
    { L"Disconnected", L"Ba\x011Fl\x0131 de\x011Fil" },
    { L"no battery info", L"pil bilgisi yok" },
    { L"No Battery Info", L"Pil Bilgisi Yok" },
    { L"hidden", L"gizli" },
    { L"2.4 GHz receiver", L"2.4 GHz al\x0131c\x0131" },
    { L"USB cable", L"USB kablo" },
    { L"Bluetooth", L"Bluetooth" },
    { L"Charging dock", L"\x015Earj stand\x0131" },
    { L"Omni Receiver", L"Omni Al\x0131c\x0131" },
    { L"Xbox Wireless", L"Xbox Kablosuz" },
    { L"Xbox Wireless / Bluetooth", L"Xbox Kablosuz / Bluetooth" },
    { L"Phone Link", L"Telefon Ba\x011Flant\x0131s\x0131" },
    { L"Wireless mouse", L"Kablosuz Fare" },
    { L"Wireless keyboard", L"Kablosuz Klavye" },
    { L"Wireless headset", L"Kablosuz Kulakl\x0131k" },
    { L"Wireless gamepad", L"Kablosuz Oyun Kolu" },
    { L"Wireless device", L"Kablosuz Cihaz" },
    { L"Bluetooth device", L"Bluetooth Cihaz\x0131" },
    { L"Bluetooth LE device", L"Bluetooth LE Cihaz\x0131" },
    { L"Xbox controller", L"Xbox Oyun Kolu" },
    { L"Xbox headset", L"Xbox Kulakl\x0131k" },
    { L"GameSir controller", L"GameSir Oyun Kolu" },
    { L"Show on taskbar", L"G\x00F6rev \x00E7ubu\x011Funda g\x00F6ster" },
    { L"Show panel (Flyout)", L"Paneli G\x00F6ster (Flyout)" },
    { L"Move up", L"Yukar\x0131 Ta\x015F\x0131" },
    { L"Move down", L"A\x015Fa\x011F\x0131 Ta\x015F\x0131" },
    { L"Color", L"Renk" },
    { L"Automatic", L"Otomatik" },
    { L"None (white / black)", L"Yok (beyaz / siyah)" },
    { L"Taskbar", L"G\x00F6rev \x00E7ubu\x011Fu" },
    { L"Taskbar Widget", L"G\x00F6rev \x00C7ubu\x011Fu Widget'\x0131" },
    { L"Show percentage", L"Y\x00FCzdeyi g\x00F6ster" },
    { L"Colored percentages", L"Renkli y\x00FCzdeler" },
    { L"Short names", L"K\x0131sa adlar" },
    { L"Show disconnected", L"Ba\x011Fl\x0131 olmayanlar\x0131 g\x00F6ster" },
    { L"Tray icon", L"Tepsi simgesi" },
    { L"Battery bars", L"Pil \x00E7ubuklar\x0131" },
    { L"Update interval", L"G\x00FCncelleme S\x0131kl\x0131\x011F\x0131" },
    { L"Low battery warning", L"D\x00FC\x015F\x00FCk Pil Uyar\x0131s\x0131" },
    { L"Off", L"Kapal\x0131" },
    { L"Notifications", L"Bildirimler" },
    { L"Low battery notification", L"D\x00FC\x015F\x00FCk Pil Bildirimi" },
    { L"Full charge notification", L"Tam \x015Earj Bildirimi (%100)" },
    { L"Connection notifications", L"Ba\x011Flant\x0131 Bildirimleri" },
    { L"Language", L"Dil / Language" },
    { L"System default", L"Sistem varsay\x0131lan\x0131" },
    { L"Run at Windows startup", L"Windows ba\x015Flang\x0131c\x0131nda \x00E7al\x0131\x015Ft\x0131r" },
    { L"Refresh now", L"\x015Eimdi Yenile" },
    { L"Device diagnostic report (--dump)", L"Cihaz Tan\x0131lama Raporu (--dump)" },
    { L"Exit", L"\x00C7\x0131k\x0131\x015F" },
    { L"Low battery", L"D\x00FC\x015F\x00FCk Pil Uyar\x0131s\x0131" },
    { L"Battery Charged", L"Pil Tamamen Doldu" },
    { L"Device Connected", L"Cihaz Ba\x011Fland\x0131" },
    { L"Device Disconnected", L"Cihaz Ba\x011Flant\x0131s\x0131 Kesildi" },
    { L"Settings", L"Ayarlar" },
    { L"Devices", L"Cihazlar" },
    { L"Reset hidden devices", L"Gizlenen cihazlar\x0131 s\x0131f\x0131rla" },
    { L"Close", L"Kapat" },
    { L"Refresh", L"Yenile" },
    { L"Active", L"Aktif" },
};

std::wstring g_setting;  // "" = system default
std::wstring g_current = L"tr";
std::unordered_map<std::wstring, std::wstring> g_table;

std::wstring TrimSpan(const wchar_t* s, size_t n) {
    size_t a = 0, b = n;
    while (a < b && iswspace(s[a])) a++;
    while (b > a && iswspace(s[b - 1])) b--;
    std::wstring out;
    out.reserve(b - a);
    for (size_t i = a; i < b; i++) {
        if (s[i] == L'\\' && i + 1 < b) {
            wchar_t c = s[++i];
            out.push_back(c == L'n' ? L'\n' : c == L't' ? L'\t' : c);
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

void ParseTable(const char* utf8, size_t bytes) {
    if (!utf8 || !bytes) return;
    if (bytes >= 3 && (unsigned char)utf8[0] == 0xEF && (unsigned char)utf8[1] == 0xBB && (unsigned char)utf8[2] == 0xBF) {
        utf8 += 3;
        bytes -= 3;
    }
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)bytes, nullptr, 0);
    if (wlen <= 0) return;
    std::wstring text(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, (int)bytes, text.data(), wlen);

    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = text.size();
        size_t len = eol - pos;
        if (len && text[pos + len - 1] == L'\r') len--;
        const wchar_t* line = text.data() + pos;
        if (len && line[0] != L'#' && line[0] != L'@') {
            for (size_t i = 0; i < len; i++) {
                if (line[i] == L'=') {
                    std::wstring key = TrimSpan(line, i);
                    std::wstring val = TrimSpan(line + i + 1, len - i - 1);
                    if (!key.empty() && !val.empty()) g_table[std::move(key)] = std::move(val);
                    break;
                }
            }
        }
        pos = eol + 1;
    }
}

std::wstring DetectWindowsLanguage() {
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    if (!GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH)) return L"tr";
    std::wstring loc = name;
    if (loc.rfind(L"zh-Hant", 0) == 0 || loc.rfind(L"zh-TW", 0) == 0 || loc.rfind(L"zh-HK", 0) == 0 || loc.rfind(L"zh-MO", 0) == 0)
        return L"zh-TW";
    if (loc.rfind(L"zh", 0) == 0) return L"zh-CN";
    if (loc.rfind(L"pt", 0) == 0) return L"pt-BR";
    size_t dash = loc.find(L'-');
    std::wstring primary = dash == std::wstring::npos ? loc : loc.substr(0, dash);
    for (const auto& l : kLanguages)
        if (primary == l.code) return l.code;
    return L"tr";
}

void Apply(const std::wstring& code) {
    g_table.clear();
    g_current = L"tr";
    const Language* found = nullptr;
    for (const auto& l : kLanguages) {
        if (code == l.code) {
            found = &l;
            break;
        }
    }
    if (!found) found = &kLanguages[0];
    g_current = found->code;

    if (g_current == L"tr") {
        for (const auto& kv : kBuiltinTr) {
            g_table[kv.first] = kv.second;
        }
    }
    if (found->resId != 0) {
        HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(found->resId), (LPCWSTR)RT_RCDATA);
        if (r) {
            HGLOBAL h = LoadResource(nullptr, r);
            if (h) ParseTable((const char*)LockResource(h), SizeofResource(nullptr, r));
        }
    }
}

bool StartsWithCaseInsensitive(const std::wstring& s, const wchar_t* prefix) {
    size_t n = wcslen(prefix);
    return s.size() >= n && _wcsnicmp(s.c_str(), prefix, n) == 0;
}

}  // namespace

const std::vector<Language>& AllLanguages() { return kLanguages; }
const std::wstring& CurrentLanguage() { return g_current; }
const std::wstring& LanguageSetting() { return g_setting; }
bool IsTurkish() { return g_current == L"tr"; }

void LoadLanguage() {
    g_setting.clear();
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t buf[32] = {};
        DWORD sz = sizeof(buf), type = 0;
        if (RegQueryValueExW(k, L"Language", nullptr, &type, (BYTE*)buf, &sz) == ERROR_SUCCESS && type == REG_SZ)
            g_setting = buf;
        RegCloseKey(k);
    }
    Apply(g_setting.empty() ? L"tr" : g_setting);
}

void SetLanguage(const std::wstring& code) {
    g_setting = code;
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        if (code.empty())
            RegDeleteValueW(k, L"Language");
        else
            RegSetValueExW(k, L"Language", 0, REG_SZ, (const BYTE*)code.c_str(), (DWORD)((code.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
    Apply(g_setting.empty() ? DetectWindowsLanguage() : g_setting);
}

const wchar_t* T(const wchar_t* en) {
    if (!en || !*en) return L"";
    auto it = g_table.find(en);
    return it != g_table.end() ? it->second.c_str() : en;
}

std::wstring T(const std::wstring& en) {
    auto it = g_table.find(en);
    return it != g_table.end() ? it->second : en;
}

std::wstring TranslateVia(const std::wstring& via) {
    if (via.empty()) return via;
    auto it = g_table.find(via);
    if (it != g_table.end()) return it->second;

    if (!IsTurkish()) return via;

    std::wstring out = via;
    auto replaceAll = [&](const std::wstring& from, const std::wstring& to) {
        size_t pos = 0;
        while ((pos = out.find(from, pos)) != std::wstring::npos) {
            out.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replaceAll(L"2.4 GHz receiver", L"2.4 GHz al\x0131c\x0131");
    replaceAll(L"USB cable", L"USB kablo");
    replaceAll(L"Charging dock", L"\x015Earj stand\x0131");
    replaceAll(L"Omni Receiver", L"Omni Al\x0131c\x0131");
    replaceAll(L"Xbox Wireless", L"Xbox Kablosuz");
    replaceAll(L"Phone Link", L"Telefon Ba\x011Flant\x0131s\x0131");
    replaceAll(L"\x00B7 L ", L"\x00B7 Sol ");
    replaceAll(L"  R ", L"  Sa\x011F ");
    replaceAll(L"  case ", L"  Kutu ");
    return out;
}

std::wstring TranslateDetail(const std::wstring& detail) {
    if (detail.empty() || !IsTurkish()) return detail;
    std::wstring out = detail;
    if (out.rfind(L"L ", 0) == 0) out.replace(0, 2, L"Sol ");
    size_t rPos = out.find(L", R ");
    if (rPos != std::wstring::npos) out.replace(rPos, 4, L", Sa\x011F ");
    size_t cPos = out.find(L", case ");
    if (cPos != std::wstring::npos) out.replace(cPos, 7, L", Kutu ");
    return out;
}

std::wstring TranslateLevel(const std::wstring& levelText) {
    if (levelText.empty()) return levelText;
    return T(levelText);
}

std::wstring TranslateDeviceName(const std::wstring& name) {
    if (name.empty()) return name;
    auto it = g_table.find(name);
    if (it != g_table.end()) return it->second;
    if (IsTurkish()) {
        if (name.rfind(L"Xbox controller ", 0) == 0) {
            return L"Xbox Oyun Kolu " + name.substr(16);
        }
    }
    return name;
}

bool IsBluetoothVia(const std::wstring& via) {
    return StartsWithCaseInsensitive(via, L"Bluetooth");
}

bool IsUsbCableVia(const std::wstring& via) {
    return StartsWithCaseInsensitive(via, L"USB cable") || StartsWithCaseInsensitive(via, L"USB kablo");
}

bool IsReceiverVia(const std::wstring& via) {
    return StartsWithCaseInsensitive(via, L"2.4 GHz") || StartsWithCaseInsensitive(via, L"Omni") ||
           StartsWithCaseInsensitive(via, L"Xbox");
}
