// Standalone native Win32 installer for Kablosuz Pil (Wireless Battery).
// Extracts WirelessBattery.exe to %LOCALAPPDATA%\Programs\Wireless Battery,
// enables Windows startup (HKCU\...\Run\WirelessBattery), creates a Start Menu shortcut,
// and launches the app immediately.
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <shobjidl.h>
#include <string>

namespace {

const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunValue = L"WirelessBattery";

bool ExtractResource(int resId, const std::wstring& destPath) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!res) return false;
    HGLOBAL mem = LoadResource(nullptr, res);
    if (!mem) return false;
    DWORD sz = SizeofResource(nullptr, res);
    const void* data = LockResource(mem);
    if (!data || !sz) return false;

    HANDLE f = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(f, data, sz, &written, nullptr);
    CloseHandle(f);
    return ok && written == sz;
}

void CloseRunningInstance() {
    if (HWND w = FindWindowW(L"WirelessBatteryWnd", nullptr)) {
        PostMessageW(w, WM_CLOSE, 0, 0);
        for (int i = 0; i < 50 && FindWindowW(L"WirelessBatteryWnd", nullptr); i++) {
            Sleep(100);
        }
    }
}

void CreateStartMenuShortcut(const std::wstring& exePath, const std::wstring& workDir) {
    wchar_t programs[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, 0, programs))) return;
    std::wstring lnkPath = std::wstring(programs) + L"\\Kablosuz Pil.lnk";

    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        IShellLinkW* sl = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl)) && sl) {
            sl->SetPath(exePath.c_str());
            sl->SetWorkingDirectory(workDir.c_str());
            sl->SetDescription(L"Kablosuz Pil - Cihaz Pil İzleyici");
            IPersistFile* pf = nullptr;
            if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf)) && pf) {
                pf->Save(lnkPath.c_str(), TRUE);
                pf->Release();
            }
            sl->Release();
        }
        CoUninitialize();
    }
}

void RegisterAutostart(const std::wstring& exePath) {
    std::wstring val = L"\"" + exePath + L"\"";
    RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t)));
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdLine, int) {
    bool silent = cmdLine && (wcsstr(cmdLine, L"--silent") || wcsstr(cmdLine, L"/S") || wcsstr(cmdLine, L"/VERYSILENT"));

    wchar_t localAppData[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData))) {
        if (!silent) MessageBoxW(nullptr, L"LocalAppData klasörü bulunamadı.", L"Kablosuz Pil Kurulumu", MB_ICONERROR);
        return 1;
    }

    std::wstring progDir = std::wstring(localAppData) + L"\\Programs";
    std::wstring installDir = progDir + L"\\Wireless Battery";
    std::wstring exePath = installDir + L"\\WirelessBattery.exe";
    std::wstring licPath = installDir + L"\\LICENSE.txt";

    if (!silent) {
        std::wstring msg = L"Kablosuz Pil (Wireless Device Battery Monitor - Gelişmiş Sürüm) kurulacaktır.\n\n"
                           L"• Destek: Tüm kablosuz cihazlar (ROG, ATK N9 Ultra, Logitech, Razer, Corsair, vb.)\n"
                           L"• Kurulum Konumu: " + installDir + L"\n"
                           L"• Windows Başlangıcı: Otomatik başlatma aktif edilecek\n\n"
                           L"Kuruluma devam etmek istiyor musunuz?";
        if (MessageBoxW(nullptr, msg.c_str(), L"Kablosuz Pil Kurulum Sihirbazı", MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return 0;
        }
    }

    CloseRunningInstance();

    CreateDirectoryW(progDir.c_str(), nullptr);
    CreateDirectoryW(installDir.c_str(), nullptr);

    if (!ExtractResource(101, exePath)) {
        if (!silent) {
            MessageBoxW(nullptr, L"WirelessBattery.exe kopyalanamadı. Uygulama çalışıyor olabilir.",
                        L"Kablosuz Pil Kurulumu", MB_ICONERROR);
        }
        return 1;
    }
    ExtractResource(102, licPath);

    RegisterAutostart(exePath);
    CreateStartMenuShortcut(exePath, installDir);

    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exePath + L"\"";
    if (CreateProcessW(exePath.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, installDir.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    if (!silent) {
        MessageBoxW(nullptr,
                    L"Kurulum başarıyla tamamlandı!\n\n"
                    L"• Kablosuz Pil görev çubuğunda başlatıldı.\n"
                    L"• Bilgisayarınız her açıldığında otomatik olarak başlayacaktır.",
                    L"Kablosuz Pil Kurulumu", MB_OK | MB_ICONINFORMATION);
    }
    return 0;
}
