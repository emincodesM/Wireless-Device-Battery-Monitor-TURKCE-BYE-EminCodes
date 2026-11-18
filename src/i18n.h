#pragma once
#include <string>
#include <vector>

// Lightweight runtime translation table.
// English is the source key used across the 70+ hardware drivers; Turkish ("tr") is the built-in
// default for this edition, and 13 additional languages are embedded from lang/<code>.txt as RCDATA.
struct Language {
    const wchar_t* code;   // registry value and file name stem ("en", "tr", "de", ...)
    const wchar_t* label;  // native name shown in the Language submenu
    int resId;             // RCDATA resource id, or 0 for English
};

const std::vector<Language>& AllLanguages();

void LoadLanguage();
void SetLanguage(const std::wstring& code);  // "" = follow Windows UI language (defaults to Turkish on TR systems)
const std::wstring& CurrentLanguage();       // active language code ("tr", "en", ...)
const std::wstring& LanguageSetting();       // stored setting ("" for system, or a language code)
bool IsTurkish();                            // true when active language is "tr"

const wchar_t* T(const wchar_t* en);
std::wstring T(const std::wstring& en);

// Translates driver connection strings ("2.4 GHz receiver", "USB cable", AirPods L/R/case, etc.)
// and coarse level strings ("Empty", "Low", "Medium", "High", "Full") into the active language.
std::wstring TranslateVia(const std::wstring& via);
std::wstring TranslateDetail(const std::wstring& detail);
std::wstring TranslateLevel(const std::wstring& levelText);
std::wstring TranslateDeviceName(const std::wstring& name);

// Language-independent connection type helpers (safe across English and Turkish strings).
bool IsBluetoothVia(const std::wstring& via);
bool IsUsbCableVia(const std::wstring& via);
bool IsReceiverVia(const std::wstring& via);
