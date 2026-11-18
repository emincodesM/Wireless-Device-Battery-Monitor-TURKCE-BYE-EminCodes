// Native Win32 Turkish Installer & Uninstaller for Wireless Battery Monitor.
// Supports interactive GUI install, `--silent` / `/S` install, and `--uninstall` removal
// with Windows "Installed Apps" (Add/Remove Programs) registry registration.
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <objbase.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <string>
#include "../res/version.h"

namespace {

constexpr int ID_CHK_STARTUP = 101;
constexpr int ID_CHK_DESKTOP = 102;
constexpr int ID_CHK_LAUNCH  = 103;
constexpr int ID_BTN_INSTALL = 201;
constexpr int ID_BTN_CANCEL  = 202;

constexpr const wchar_t* kUninstallRegKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\WirelessBattery";
constexpr const wchar_t* kRunRegKey       = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

HWND g_chkStartup = nullptr;
HWND g_chkDesktop = nullptr;
HWND g_chkLaunch  = nullptr;
HWND g_statusLbl  = nullptr;
HFONT g_fontTitle = nullptr;
HFONT g_fontSub   = nullptr;
HFONT g_fontBody  = nullptr;

void KillRunningApp() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{ sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"WirelessBattery.exe") == 0) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
                if (h) {
                    TerminateProcess(h, 0);
                    WaitForSingleObject(h, 2000);
                    CloseHandle(h);
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

std::wstring SelfExePath() {
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

std::wstring GetInstallDir() {
    wchar_t localAppData[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData))) {
        std::wstring p1 = std::wstring(localAppData) + L"\\Programs";
        CreateDirectoryW(p1.c_str(), nullptr);
        std::wstring p2 = p1 + L"\\WirelessBattery";
        CreateDirectoryW(p2.c_str(), nullptr);
        return p2;
    }
    return L"C:\\WirelessBattery";
}

bool ExtractEmbeddedResource(int resId, const std::wstring& outPath) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(resId), (LPCWSTR)RT_RCDATA);
    if (!res) return false;
    HGLOBAL hGlobal = LoadResource(nullptr, res);
    if (!hGlobal) return false;
    DWORD sz = SizeofResource(nullptr, res);
    const void* data = LockResource(hGlobal);
    if (!data || sz == 0) return false;

    HANDLE f = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(f, data, sz, &wr, nullptr);
    CloseHandle(f);
    return ok && wr == sz;
}

void CreateShortcutFile(const std::wstring& targetExe, const std::wstring& lnkPath, const std::wstring& desc) {
    IShellLinkW* psl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&psl))) {
        psl->SetPath(targetExe.c_str());
        std::wstring workDir = targetExe.substr(0, targetExe.find_last_of(L"\\/"));
        psl->SetWorkingDirectory(workDir.c_str());
        psl->SetDescription(desc.c_str());
        psl->SetIconLocation(targetExe.c_str(), 0);
        IPersistFile* ppf = nullptr;
        if (SUCCEEDED(psl->QueryInterface(IID_IPersistFile, (void**)&ppf))) {
            ppf->Save(lnkPath.c_str(), TRUE);
            ppf->Release();
        }
        psl->Release();
    }
}

void SetAutoStartRegistry(const std::wstring& exePath, bool enable) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (enable) {
        std::wstring val = L"\"" + exePath + L"\"";
        RegSetValueExW(k, L"WirelessBattery", 0, REG_SZ, (const BYTE*)val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"WirelessBattery");
    }
    RegCloseKey(k);
}

void RegisterUninstaller(const std::wstring& installDir, const std::wstring& exePath, const std::wstring& uninstExe) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;

    auto setStr = [&](const wchar_t* name, const std::wstring& val) {
        RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    };
    auto setDword = [&](const wchar_t* name, DWORD val) {
        RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
    };

    setStr(L"DisplayName", L"Kablosuz Cihaz Pil \x0130zleyici (Wireless Battery)");
    setStr(L"DisplayVersion", APP_VERSION_WSTR);
    setStr(L"Publisher", L"WirelessBattery");
    setStr(L"DisplayIcon", exePath);
    setStr(L"InstallLocation", installDir);
    setStr(L"UninstallString", L"\"" + uninstExe + L"\" --uninstall");
    setDword(L"NoModify", 1);
    setDword(L"NoRepair", 1);
    RegCloseKey(k);
}

int RunUninstall(bool silent) {
    if (!silent) {
        int ans = MessageBoxW(nullptr,
                              L"Kablosuz Cihaz Pil \x0130zleyici kald\x0131r\x0131ls\x0131n m\x0131?",
                              L"Kald\x0131r", MB_YESNO | MB_ICONQUESTION);
        if (ans != IDYES) return 0;
    }

    KillRunningApp();
    SetAutoStartRegistry(L"", false);
    RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallRegKey);

    wchar_t programs[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, 0, programs))) {
        DeleteFileW((std::wstring(programs) + L"\\Kablosuz Pil \x0130zleyici.lnk").c_str());
    }
    wchar_t desktop[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop))) {
        DeleteFileW((std::wstring(desktop) + L"\\Kablosuz Pil \x0130zleyici.lnk").c_str());
    }

    std::wstring installDir = GetInstallDir();
    DeleteFileW((installDir + L"\\WirelessBattery.exe").c_str());

    if (!silent) {
        MessageBoxW(nullptr, L"Kablosuz Cihaz Pil \x0130zleyici ba\x015Far\x0131yla kald\x0131r\x0131ld\x0131.",
                    L"Kald\x0131rma Tamamland\x0131", MB_OK | MB_ICONINFORMATION);
    }
    return 0;
}

bool PerformInstall(bool wantStartup, bool wantDesktop, bool wantLaunch, std::wstring& errOut) {
    KillRunningApp();

    std::wstring installDir = GetInstallDir();
    std::wstring exePath = installDir + L"\\WirelessBattery.exe";
    std::wstring uninstPath = installDir + L"\\Uninstall-WirelessBattery.exe";

    if (!ExtractEmbeddedResource(101, exePath)) {
        errOut = L"Dosya \x00E7\x0131kar\x0131lamad\x0131: " + exePath;
        return false;
    }

    // Copy setup binary as Uninstaller inside installDir
    std::wstring self = SelfExePath();
    if (!self.empty() && _wcsicmp(self.c_str(), uninstPath.c_str()) != 0) {
        CopyFileW(self.c_str(), uninstPath.c_str(), FALSE);
    }
    RegisterUninstaller(installDir, exePath, uninstPath);

    wchar_t programs[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, 0, programs))) {
        std::wstring startMenuLnk = std::wstring(programs) + L"\\Kablosuz Pil \x0130zleyici.lnk";
        CreateShortcutFile(exePath, startMenuLnk, L"Kablosuz Cihaz Pil \x0130zleyici");
    }

    if (wantDesktop) {
        wchar_t desktop[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop))) {
            std::wstring deskLnk = std::wstring(desktop) + L"\\Kablosuz Pil \x0130zleyici.lnk";
            CreateShortcutFile(exePath, deskLnk, L"Kablosuz Cihaz Pil \x0130zleyici");
        }
    }

    SetAutoStartRegistry(exePath, wantStartup);

    if (wantLaunch) {
        ShellExecuteW(nullptr, L"open", exePath.c_str(), nullptr, installDir.c_str(), SW_SHOWNORMAL);
    }
    return true;
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_CREATE: {
            g_fontTitle = CreateFontW(-20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            g_fontSub   = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            g_fontBody  = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

            HWND title = CreateWindowExW(0, L"STATIC",
                L"Kablosuz Cihaz Pil \x0130zleyici v" APP_VERSION_WSTR,
                WS_CHILD | WS_VISIBLE, 24, 20, 420, 28, h, nullptr, nullptr, nullptr);
            SendMessageW(title, WM_SETFONT, (WPARAM)g_fontTitle, TRUE);

            HWND sub = CreateWindowExW(0, L"STATIC",
                L"T\x00FCrk\x00E7" L"e S\x00FCr\x00FCm \x2022 Modern Windows 11 Flyout Paneli & 70+ S\x00FCr\x00FCc\x00FC Deste\x011Fi",
                WS_CHILD | WS_VISIBLE, 24, 50, 430, 22, h, nullptr, nullptr, nullptr);
            SendMessageW(sub, WM_SETFONT, (WPARAM)g_fontSub, TRUE);

            std::wstring dirInfo = L"Kurulum Konumu: " + GetInstallDir();
            HWND dirLbl = CreateWindowExW(0, L"STATIC", dirInfo.c_str(),
                WS_CHILD | WS_VISIBLE, 24, 82, 430, 20, h, nullptr, nullptr, nullptr);
            SendMessageW(dirLbl, WM_SETFONT, (WPARAM)g_fontSub, TRUE);

            g_chkStartup = CreateWindowExW(0, L"BUTTON",
                L"Windows ba\x015Flang\x0131c\x0131nda otomatik \x00E7al\x0131\x015Ft\x0131r (\x00D6nerilen)",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 24, 118, 420, 24, h, (HMENU)ID_CHK_STARTUP, nullptr, nullptr);
            SendMessageW(g_chkStartup, WM_SETFONT, (WPARAM)g_fontBody, TRUE);
            SendMessageW(g_chkStartup, BM_SETCHECK, BST_CHECKED, 0);

            g_chkDesktop = CreateWindowExW(0, L"BUTTON",
                L"Masa\x00FCst\x00FC k\x0131sayolu olu\x015Ftur",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 24, 148, 420, 24, h, (HMENU)ID_CHK_DESKTOP, nullptr, nullptr);
            SendMessageW(g_chkDesktop, WM_SETFONT, (WPARAM)g_fontBody, TRUE);
            SendMessageW(g_chkDesktop, BM_SETCHECK, BST_CHECKED, 0);

            g_chkLaunch = CreateWindowExW(0, L"BUTTON",
                L"Kurulum tamamland\x0131ktan sonra uygulamay\x0131 ba\x015Flat",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 24, 178, 420, 24, h, (HMENU)ID_CHK_LAUNCH, nullptr, nullptr);
            SendMessageW(g_chkLaunch, WM_SETFONT, (WPARAM)g_fontBody, TRUE);
            SendMessageW(g_chkLaunch, BM_SETCHECK, BST_CHECKED, 0);

            g_statusLbl = CreateWindowExW(0, L"STATIC",
                L"Kuruluma haz\x0131r. 'Kur ve Ba\x015Flat' d\x00FC\x011Fmesine t\x0131klay\x0131n.",
                WS_CHILD | WS_VISIBLE, 24, 215, 420, 22, h, nullptr, nullptr, nullptr);
            SendMessageW(g_statusLbl, WM_SETFONT, (WPARAM)g_fontSub, TRUE);

            HWND btnInstall = CreateWindowExW(0, L"BUTTON", L"Kur ve Ba\x015Flat",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 224, 250, 120, 34, h, (HMENU)ID_BTN_INSTALL, nullptr, nullptr);
            SendMessageW(btnInstall, WM_SETFONT, (WPARAM)g_fontBody, TRUE);

            HWND btnCancel = CreateWindowExW(0, L"BUTTON", L"\x0130ptal",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 354, 250, 90, 34, h, (HMENU)ID_BTN_CANCEL, nullptr, nullptr);
            SendMessageW(btnCancel, WM_SETFONT, (WPARAM)g_fontBody, TRUE);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == ID_BTN_INSTALL) {
                bool wantStartup = SendMessageW(g_chkStartup, BM_GETCHECK, 0, 0) == BST_CHECKED;
                bool wantDesktop = SendMessageW(g_chkDesktop, BM_GETCHECK, 0, 0) == BST_CHECKED;
                bool wantLaunch  = SendMessageW(g_chkLaunch,  BM_GETCHECK, 0, 0) == BST_CHECKED;
                SetWindowTextW(g_statusLbl, L"Kuruluyor, l\x00FCtfen bekleyin...");
                UpdateWindow(h);
                std::wstring err;
                if (PerformInstall(wantStartup, wantDesktop, wantLaunch, err)) {
                    MessageBoxW(h, L"Kablosuz Cihaz Pil \x0130zleyici ba\x015Far\x0131yla kuruldu!",
                                L"Kurulum Tamamland\x0131", MB_OK | MB_ICONINFORMATION);
                    DestroyWindow(h);
                } else {
                    MessageBoxW(h, err.c_str(), L"Kurulum Hatas\x0131", MB_OK | MB_ICONERROR);
                }
                return 0;
            }
            if (LOWORD(wp) == ID_BTN_CANCEL) {
                DestroyWindow(h);
                return 0;
            }
            break;
        case WM_DESTROY:
            if (g_fontTitle) DeleteObject(g_fontTitle);
            if (g_fontSub)   DeleteObject(g_fontSub);
            if (g_fontBody)  DeleteObject(g_fontBody);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool silent = false, uninstall = false;
    if (argv) {
        for (int i = 1; i < argc; i++) {
            std::wstring a = argv[i];
            if (a == L"--silent" || a == L"/S" || a == L"/s") silent = true;
            else if (a == L"--uninstall" || a == L"/uninstall") uninstall = true;
        }
        LocalFree(argv);
    }

    if (uninstall) {
        int rc = RunUninstall(silent);
        CoUninitialize();
        return rc;
    }

    if (silent) {
        std::wstring err;
        PerformInstall(true, true, true, err);
        CoUninitialize();
        return 0;
    }

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"WirelessBatterySetupWnd";
    RegisterClassExW(&wc);

    int w = 478, h = 338;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"Kablosuz Pil \x0130zleyici - Kurulum Sihirbaz\x0131",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                               sx, sy, w, h, nullptr, nullptr, hInst, nullptr);
    ShowWindow(wnd, SW_SHOW);
    UpdateWindow(wnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return 0;
}
