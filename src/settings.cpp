#include "settings.h"

#include <cstdarg>
#include <cstdio>
#include <cwchar>

namespace settings {

namespace {
constexpr const wchar_t* kRunRegKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kRunValue  = L"WirelessBattery";
constexpr const wchar_t* kAppRegKey = L"Software\\WirelessBattery";

std::wstring JoinSemicolon(const std::vector<std::wstring>& v) {
    std::wstring s;
    for (size_t i = 0; i < v.size(); i++) {
        if (i) s += L';';
        s += v[i];
    }
    return s;
}

std::vector<std::wstring> SplitSemicolon(const std::wstring& s) {
    std::vector<std::wstring> r;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(L';', i);
        if (j == std::wstring::npos) j = s.size();
        if (j > i) r.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return r;
}

}  // namespace

std::atomic<int> g_pollIntervalSec{ 15 };
int  g_lowBatteryThreshold = 20;
bool g_notifyLowBattery    = true;
bool g_notifyFullCharge    = true;
bool g_notifyConnect       = false;
bool g_debugLog            = false;

std::set<std::wstring>    g_hiddenDevices;
std::vector<std::wstring> g_deviceOrder;

std::wstring ExePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0) return L"";
        if (n < buf.size() - 1) {
            buf.resize(n);
            return buf;
        }
        if (buf.size() >= 32768) return L"";
        buf.resize(buf.size() * 2);
    }
}

bool IsAutoStartEnabled() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    DWORD sz = 0;
    bool ok = RegQueryValueExW(k, kRunValue, nullptr, nullptr, nullptr, &sz) == ERROR_SUCCESS && sz > 0;
    RegCloseKey(k);
    return ok;
}

void SetAutoStart(bool enable) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (enable) {
        std::wstring val = L"\"" + ExePath() + L"\"";
        RegSetValueExW(k, kRunValue, 0, REG_SZ, (const BYTE*)val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunValue);
    }
    RegCloseKey(k);
}

void Load() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kAppRegKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return;

    auto readStr = [&](const wchar_t* name) -> std::wstring {
        DWORD sz = 0, type = 0;
        if (RegQueryValueExW(k, name, nullptr, &type, nullptr, &sz) != ERROR_SUCCESS || type != REG_SZ || sz < sizeof(wchar_t))
            return {};
        std::wstring s(sz / sizeof(wchar_t), L'\0');
        RegQueryValueExW(k, name, nullptr, nullptr, (BYTE*)s.data(), &sz);
        while (!s.empty() && s.back() == L'\0') s.pop_back();
        return s;
    };
    auto readDword = [&](const wchar_t* name, DWORD def) -> DWORD {
        DWORD val = def, sz = sizeof(val), type = 0;
        if (RegQueryValueExW(k, name, nullptr, &type, (BYTE*)&val, &sz) != ERROR_SUCCESS || type != REG_DWORD) return def;
        return val;
    };

    auto h = SplitSemicolon(readStr(L"HiddenDevices"));
    g_hiddenDevices.clear();
    g_hiddenDevices.insert(h.begin(), h.end());
    g_deviceOrder = SplitSemicolon(readStr(L"DeviceOrder"));

    int poll = (int)readDword(L"PollInterval", 15);
    if (poll != 5 && poll != 10 && poll != 15 && poll != 30 && poll != 60) poll = 15;
    g_pollIntervalSec.store(poll, std::memory_order_relaxed);

    g_lowBatteryThreshold = (int)readDword(L"LowBatteryThreshold", 20);
    if (g_lowBatteryThreshold < 0 || g_lowBatteryThreshold > 50) g_lowBatteryThreshold = 20;

    g_notifyLowBattery = readDword(L"NotifyLowBattery", 1) != 0;
    g_notifyFullCharge = readDword(L"NotifyFullCharge", 1) != 0;
    g_notifyConnect    = readDword(L"NotifyConnect", 0) != 0;

    RegCloseKey(k);
}

void Save() {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kAppRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;

    auto writeStr = [&](const wchar_t* name, const std::wstring& s) {
        RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)s.c_str(), (DWORD)((s.size() + 1) * sizeof(wchar_t)));
    };
    auto writeDword = [&](const wchar_t* name, DWORD val) {
        RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
    };

    writeStr(L"HiddenDevices", JoinSemicolon({ g_hiddenDevices.begin(), g_hiddenDevices.end() }));
    writeStr(L"DeviceOrder", JoinSemicolon(g_deviceOrder));
    writeDword(L"PollInterval", (DWORD)g_pollIntervalSec.load(std::memory_order_relaxed));
    writeDword(L"LowBatteryThreshold", (DWORD)g_lowBatteryThreshold);
    writeDword(L"NotifyLowBattery", g_notifyLowBattery ? 1 : 0);
    writeDword(L"NotifyFullCharge", g_notifyFullCharge ? 1 : 0);
    writeDword(L"NotifyConnect", g_notifyConnect ? 1 : 0);
    RegCloseKey(k);
}

void EnableDebugLog(bool enable) {
    g_debugLog = enable;
}

void LogDebug(const wchar_t* fmt, ...) {
    if (!g_debugLog || !fmt) return;
    wchar_t base[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return;
    std::wstring dir = std::wstring(base) + L"\\WirelessBattery";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\debug.log";

    wchar_t msg[1024] = {};
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, args);
    va_end(args);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t line[1152] = {};
    swprintf_s(line, L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\r\n",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);

    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 1) return;
    std::string utf8(utf8Len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8.data(), utf8Len - 1, nullptr, nullptr);

    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(h, utf8.data(), (DWORD)utf8.size(), &wr, nullptr);
        CloseHandle(h);
    }
}

}  // namespace settings
