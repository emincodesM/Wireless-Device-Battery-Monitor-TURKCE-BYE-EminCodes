// Runs every driver in order. Vendor drivers go first and claim the devices they handle; the
// generic fallbacks then only look at what is left.
#include "devices.h"
#include "drivers/drivers.h"
#include "hid.h"
#include "i18n.h"

#include <setupapi.h>
#include <initguid.h>
#include <devguid.h>
#include <devpkey.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <cwctype>

namespace {

std::wstring Lower(std::wstring s) {
    for (auto& ch : s) ch = towlower(ch);
    return s;
}

bool Has(const std::wstring& hay, const wchar_t* needle) { return hay.find(needle) != std::wstring::npos; }

// Matches a word or word-prefix (`ear` matches `earbuds`, `earphone`, `in-ear`, NOT `gear` or `pearl`;
// `keys` matches `mx keys`, NOT `monkey`).
bool HasWordPrefix(const std::wstring& hay, const wchar_t* prefix, bool exactWord = false) {
    size_t plen = wcslen(prefix);
    if (!plen) return false;
    for (size_t pos = hay.find(prefix); pos != std::wstring::npos; pos = hay.find(prefix, pos + 1)) {
        bool startOk = (pos == 0) || !iswalnum(hay[pos - 1]);
        size_t end = pos + plen;
        bool endOk = !exactWord || (end == hay.size() || !iswalnum(hay[end]));
        if (startOk && endOk) return true;
    }
    return false;
}

// HKCU\Software\WirelessBattery\IgnoreDevices (REG_SZ): "VID:PID" pairs in hex, separated by spaces,
// commas or semicolons (e.g. "28BD:0947, 046D:C547"). No driver ever opens those devices (issue #232).
std::vector<std::pair<USHORT, USHORT>> IgnoredDevices() {
    std::vector<std::pair<USHORT, USHORT>> out;
    wchar_t buf[1024] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"IgnoreDevices", RRF_RT_REG_SZ, nullptr, buf, &size) !=
        ERROR_SUCCESS)
        return out;
    for (const wchar_t* p = buf; *p;) {
        unsigned vid = 0, pid = 0;
        int used = 0;
        if (swscanf(p, L" %x:%x%n", &vid, &pid, &used) == 2 && used > 0) {
            out.push_back({ (USHORT)vid, (USHORT)pid });
            p += used;
        } else {
            p++;
        }
        while (*p == L',' || *p == L';' || *p == L' ') p++;
    }
    return out;
}

const wchar_t* BrandPrefix(unsigned short vid) {
    switch (vid) {
        case 0x03F0: return L"HyperX ";
        case 0x0951: return L"HyperX ";
        case 0x046D: return L"Logitech ";
        case 0x04A5: return L"ZOWIE ";
        case 0x054C: return L"Sony ";
        case 0x093A: return L"Pulsar ";
        case 0x0B05: return L"ASUS ROG ";
        case 0x0B0E: return L"Jabra ";
        case 0x0ECB: return L"JBL ";
        case 0x1038: return L"SteelSeries ";
        case 0x1532: return L"Razer ";
        case 0x068E: return L"Razer ";
        case 0x1B1C: return L"Corsair ";
        case 0x22D4: return L" Glorious ";
        case 0x258A: return L"Glorious ";
        case 0x24AE: return L"Rapoo ";
        case 0x2516: return L"Cooler Master ";
        case 0x260D: return L"Dareu ";
        case 0x28DE: return L"Steam ";
        case 0x2DC8: return L"8BitDo ";
        case 0x2F68: return L"Keychron ";
        case 0x3367: return L"VAXEE ";
        case 0x3434: return L"Keychron ";
        case 0x3537: return L"GameSir ";
        case 0x361D: return L"Finalmouse ";
        case 0x36A7: return L"WLmouse ";
        case 0x3710: return L"ATK ";
        case 0x373B: return L"ATK ";
        case 0x373E: return L"ATK ";
        case 0x37B0: return L"Pulsar ";
        case 0x9886: return L"ASTRO ";
        default:     return nullptr;
    }
}

void ApplyBrand(DeviceStatus& s) {
    const wchar_t* p = BrandPrefix(s.vid);
    if (!p || s.name.empty()) return;
    bool allowGeneric = p[0] == L' ';
    if (allowGeneric) p++;
    else if (s.name.rfind(L"Wireless ", 0) == 0 || s.name.rfind(L"Bluetooth ", 0) == 0 ||
             s.name.rfind(L"Kablosuz ", 0) == 0)
        return;
    std::wstring n = Lower(s.name), b = Lower(p);
    while (!b.empty() && b.back() == L' ') b.pop_back();
    if (n.find(b) != std::wstring::npos) return;
    if (s.vid == 0x0B05 && (n.find(L"rog") != std::wstring::npos || n.find(L"asus") != std::wstring::npos || n.find(L"tuf") != std::wstring::npos))
        return;
    if (s.vid == 0x054C && (n.find(L"dualsense") != std::wstring::npos || n.find(L"dualshock") != std::wstring::npos || n.find(L"playstation") != std::wstring::npos))
        return;
    if (s.vid == 0x046D && (n.rfind(L"logi ", 0) == 0 || n.rfind(L"blue ", 0) == 0)) return;
    s.name = p + s.name;
}

std::wstring DumpExtraNodes() {
    std::wstring s;
    wchar_t line[512];

    s += L"\n--- Serial (COM) ports ---\n";
    HDEVINFO cdi = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    int comCount = 0;
    if (cdi != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA dd{ sizeof(dd) };
        for (DWORD i = 0; SetupDiEnumDeviceInfo(cdi, i, &dd); i++) {
            wchar_t id[512] = {}, friendly[256] = {};
            SetupDiGetDeviceInstanceIdW(cdi, &dd, id, 512, nullptr);
            SetupDiGetDeviceRegistryPropertyW(cdi, &dd, SPDRP_FRIENDLYNAME, nullptr, (BYTE*)friendly, sizeof(friendly), nullptr);
            wsprintfW(line, L"  %s  (%s)\n", friendly, id);
            s += line;
            comCount++;
        }
        SetupDiDestroyDeviceInfoList(cdi);
    }
    if (!comCount) s += L"  (none)\n";

    s += L"\n--- Unopened HID interfaces ---\n";
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO di = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    int unopenedCount = 0;
    if (di != INVALID_HANDLE_VALUE) {
        SP_DEVICE_INTERFACE_DATA ifd{ sizeof(ifd) };
        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, nullptr, &hidGuid, i, &ifd); i++) {
            DWORD need = 0;
            SetupDiGetDeviceInterfaceDetailW(di, &ifd, nullptr, 0, &need, nullptr);
            if (!need) continue;
            std::vector<BYTE> buf(need);
            auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
            det->cbSize = sizeof(*det);
            if (!SetupDiGetDeviceInterfaceDetailW(di, &ifd, det, need, nullptr, nullptr)) continue;
            HANDLE h = CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                wsprintfW(line, L"  err=%lu  %s\n", GetLastError(), det->DevicePath);
                s += line;
                unopenedCount++;
            } else {
                CloseHandle(h);
            }
        }
        SetupDiDestroyDeviceInfoList(di);
    }
    if (!unopenedCount) s += L"  (none)\n";

    s += L"\n--- Bluetooth nodes (BTHENUM / BTHLE) ---\n";
    int btCount = 0;
    for (const wchar_t* enumerator : { L"BTHENUM", L"BTHLE" }) {
        HDEVINFO bdi = SetupDiGetClassDevsW(nullptr, enumerator, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
        if (bdi == INVALID_HANDLE_VALUE) continue;
        SP_DEVINFO_DATA dd{ sizeof(dd) };
        for (DWORD i = 0; SetupDiEnumDeviceInfo(bdi, i, &dd); i++) {
            wchar_t inst[512] = {}, name[256] = {};
            if (!SetupDiGetDeviceInstanceIdW(bdi, &dd, inst, 512, nullptr)) continue;
            if (wcsncmp(inst, L"BTHENUM\\DEV_", 12) != 0 && wcsncmp(inst, L"BTHLE\\DEV_", 10) != 0) continue;
            DEVPROPTYPE t = 0;
            SetupDiGetDevicePropertyW(bdi, &dd, &DEVPKEY_Device_FriendlyName, &t, (PBYTE)name, sizeof(name) - 2, nullptr, 0);
            wsprintfW(line, L"  %s  \"%s\"\n", inst, name);
            s += line;
            btCount++;
        }
        SetupDiDestroyDeviceInfoList(bdi);
    }
    if (!btCount) s += L"  (none)\n";
    return s;
}

}  // namespace

Link InferLink(const std::wstring& via) {
    if (via.empty()) return Link::Unknown;
    std::wstring l = Lower(via);
    if (Has(l, L"headsetcontrol")) return Link::HeadsetControl;
    if (Has(l, L"xbox")) return Link::XboxWireless;
    if (Has(l, L"phone link") || Has(l, L"telefon")) return Link::PhoneLink;
    if (Has(l, L"omni")) return Link::OmniReceiver;
    if (Has(l, L"dock") || Has(l, L"stand")) return Link::ChargingDock;
    if (Has(l, L"2.4") && Has(l, L"bluetooth")) return Link::ReceiverOrBt;
    if (Has(l, L"bluetooth")) return Link::Bluetooth;
    if (Has(l, L"usb")) return Link::UsbCable;
    if (Has(l, L"2.4") || Has(l, L"receiver") || Has(l, L"al\x0131c\x0131")) return Link::Receiver24GHz;
    return Link::Unknown;
}

Level InferLevel(const std::wstring& levelText) {
    if (levelText.empty()) return Level::None;
    std::wstring l = Lower(levelText);
    if (l == L"empty" || l == L"bo\x015F") return Level::Empty;
    if (l == L"low" || l == L"d\x00FC\x015F\x00FCk") return Level::Low;
    if (l == L"medium" || l == L"orta") return Level::Medium;
    if (l == L"high" || l == L"y\x00FCksek") return Level::High;
    if (l == L"full" || l == L"dolu") return Level::Full;
    return Level::None;
}

const wchar_t* LinkText(Link l) {
    switch (l) {
        case Link::Receiver24GHz:  return T(L"2.4 GHz receiver");
        case Link::UsbCable:       return T(L"USB cable");
        case Link::Bluetooth:      return T(L"Bluetooth");
        case Link::ChargingDock:   return T(L"Charging dock");
        case Link::OmniReceiver:   return T(L"Omni Receiver");
        case Link::ReceiverOrBt:   return L"2.4 GHz / Bluetooth";
        case Link::HeadsetControl: return L"2.4 GHz (HeadsetControl)";
        case Link::XboxWireless:   return T(L"Xbox Wireless");
        case Link::PhoneLink:      return T(L"Phone Link");
        default:                   return L"";
    }
}

const wchar_t* LevelText(Level lv) {
    switch (lv) {
        case Level::Empty:  return T(L"Empty");
        case Level::Low:    return T(L"Low");
        case Level::Medium: return T(L"Medium");
        case Level::High:   return T(L"High");
        case Level::Full:   return T(L"Full");
        default:            return L"";
    }
}

DeviceKind GuessKind(const std::wstring& name, DeviceKind fallback) {
    std::wstring l = Lower(name);
    if (Has(l, L"headset") || Has(l, L"headphone") || Has(l, L"airpods") || HasWordPrefix(l, L"buds", true) ||
        HasWordPrefix(l, L"ear") || Has(l, L"arctis") || Has(l, L"virtuoso") || Has(l, L"void") ||
        Has(l, L"cloud") || Has(l, L"barracuda") || Has(l, L"blackshark") || Has(l, L"kraken") ||
        Has(l, L"zone") || Has(l, L"g733") || Has(l, L"g535") || Has(l, L"g435") ||
        Has(l, L"wh-") || Has(l, L"wf-") || Has(l, L"inzone") || Has(l, L"maxwell") ||
        Has(l, L"stealth") || Has(l, L"nova") || Has(l, L"pelta") || Has(l, L"kulakl\x0131k"))
        return DeviceKind::Headset;
    if (Has(l, L"keyboard") || Has(l, L"keychr") || Has(l, L"nuphy") || Has(l, L"k380") ||
        Has(l, L"mx keys") || HasWordPrefix(l, L"keys", true) || Has(l, L"g915") || Has(l, L"g715") ||
        Has(l, L"k70") || Has(l, L"k65") || Has(l, L"huntsman") || Has(l, L"blackwidow") ||
        Has(l, L"deathstalker") || Has(l, L"azoth") || Has(l, L"falchion") || Has(l, L"strix scope") ||
        Has(l, L"aula") || Has(l, L"f75") || Has(l, L"f87") || Has(l, L"f99") || Has(l, L"klavye"))
        return DeviceKind::Keyboard;
    if (Has(l, L"controller") || Has(l, L"gamepad") || Has(l, L"xbox") || Has(l, L"dualsense") ||
        Has(l, L"dualshock") || Has(l, L"joy-con") || Has(l, L"8bitdo") || Has(l, L"gamesir") ||
        Has(l, L"flydigi") || Has(l, L"vader") || Has(l, L"apex") || Has(l, L"oyun kolu") || Has(l, L"kumanda"))
        return DeviceKind::Gamepad;
    if (Has(l, L"phone") || Has(l, L"iphone") || Has(l, L"telefon"))
        return DeviceKind::Phone;
    if (Has(l, L"mouse") || Has(l, L"mice") || Has(l, L"trackball") || Has(l, L"viper") ||
        Has(l, L"deathadder") || Has(l, L"basilisk") || Has(l, L"naga") || Has(l, L"orochi") ||
        Has(l, L"superlight") || Has(l, L"mx master") || Has(l, L"mx anywhere") || Has(l, L"g502") ||
        Has(l, L"g305") || Has(l, L"g703") || Has(l, L"g303") || Has(l, L"g pro") || Has(l, L"aerox") ||
        Has(l, L"rival") || Has(l, L"prime") || Has(l, L"gladius") || Has(l, L"harpe") || Has(l, L"keris") ||
        Has(l, L"chakram") || Has(l, L"spath") || Has(l, L"impact") || Has(l, L"model o") ||
        Has(l, L"model d") || Has(l, L"model i") || Has(l, L"pulsar") || Has(l, L"xlite") ||
        Has(l, L"lamzu") || Has(l, L"atlantis") || Has(l, L"maya") || Has(l, L"thorn") ||
        Has(l, L"haste") || Has(l, L"kone") || Has(l, L"kysona") || Has(l, L"mchose") ||
        Has(l, L"dareu") || Has(l, L"rapoo") || Has(l, L"attack shark") || Has(l, L"ajazz") ||
        Has(l, L"delux") || Has(l, L"vgn") || Has(l, L"vxe") || Has(l, L"zaopin") ||
        Has(l, L"incott") || Has(l, L"rawm") || Has(l, L"metaphyuni") || Has(l, L"fantech") ||
        Has(l, L"wlmouse") || Has(l, L"finalmouse") || Has(l, L"vaxee") || Has(l, L"zowie") ||
        Has(l, L"ninjutso") || Has(l, L"sprime") || Has(l, L"darmoshark") || Has(l, L"fare"))
        return DeviceKind::Mouse;
    return fallback;
}

std::vector<DeviceStatus> PollAllDevices() {
    PollContext ctx;
    ctx.colls = EnumHid();
    auto ignored = IgnoredDevices();
    if (!ignored.empty()) {
        ctx.colls.erase(std::remove_if(ctx.colls.begin(), ctx.colls.end(), [&](const HidColl& c) {
                            return std::find(ignored.begin(), ignored.end(), std::make_pair(c.vid, c.pid)) != ignored.end();
                        }),
                        ctx.colls.end());
    }

    PollGlorious(ctx);
    PollAula(ctx);           // before Kysona, which shares the 3554 vendor id and would probe it first
    PollDareu(ctx);
    PollDarmoshark(ctx);
    PollAtkSerial(ctx);      // before Kysona, which shares the ATK vendor id
    PollAtkKeyboard(ctx);
    PollPulsar(ctx);         // before Kysona, which also scans the 3710 vendor id
    PollAtkBitmouse(ctx);    // before Kysona, which would probe the NK dongle with the wrong protocol
    PollKysona(ctx);
    PollAtkNordic54(ctx);    // ATK Zero / Blazing Sky V2 Nordic 54L15 64-byte HID mice
    PollKeychron(ctx);
    PollCherry(ctx);
    PollArbiter(ctx);        // before Lamzu, which shares the 373E vendor id
    PollLamzu(ctx);
    PollEndgame(ctx);
    PollEndgameWe(ctx);
    PollVaxee(ctx);
    PollWlmouse(ctx);
    PollFinalmouse(ctx);
    PollNinjutso(ctx);
    PollSprime(ctx);
    PollRongyuan(ctx);
    PollSonix(ctx);
    PollSemico(ctx);
    PollSonyInzone(ctx);
    PollFractal(ctx);
    PollJabra(ctx);
    PollJbl(ctx);
    PollRog(ctx);
    PollZowie(ctx);
    PollRoyalKludge(ctx);
    PollMchose(ctx);
    PollRapoo(ctx);
    PollAreson(ctx);
    PollMchoseG3(ctx);
    PollMsi(ctx);
    PollMachenike(ctx);      // before Ajazz in PollMiscMice, which shares the L7's 248A vendor id
    PollNiceboy(ctx);        // exact 248A:FA02 match, also before the Ajazz / Delux pollers
    PollMchoseModern(ctx);
    PollMchoseHeadset(ctx);
    PollMiscMice(ctx);
    PollFantech(ctx);
    PollRazerHeadsets(ctx);  // before the generic Razer driver, which shares the vendor id
    PollBarracudaPro(ctx);   // before the generic Razer driver as well
    PollHyperX(ctx);
    PollLogitechG435(ctx);   // before the HID++ driver, which shares the vendor id
    PollAstro(ctx);
    PollLogitech(ctx);
    PollRazer(ctx);
    PollSteelSeriesElite(ctx);
    PollSteelSeries(ctx);
    PollAudeze(ctx);
    PollNzxt(ctx);
    PollPlantronics(ctx);
    PollCorsair(ctx);
    PollCoolerMaster(ctx);
    PollPlayStation(ctx);
    PollSwitchPro(ctx);
    PollSteamController(ctx);
    PollGameSir(ctx);
    PollFlydigi(ctx);
    PollGameSirCyclone(ctx);
    Poll8BitDo(ctx);
    PollBeitong(ctx);        // before the XInput fallback, which only sees the receiver as wired
    PollAirPods(ctx);        // before the Bluetooth fallback, which has no level for AirPods
    PollPhoneLink(ctx);
    PollGalaxyBuds(ctx);     // before the Bluetooth fallback, which skips devices already listed by name
    PollMoondrop(ctx);
    PollAlienware(ctx);
    PollMaono(ctx);          // MAONO PD200W receiver (352F:040E), experimental battery query
    PollHeadsetControl(ctx);
    PollBluetooth(ctx);      // before the generic HID check: Windows' own level wins for the same device
    PollGenericHid(ctx);
    PollXInput(ctx);

    for (auto& d : ctx.out) {
        ApplyBrand(d);
        if (d.link == Link::Unknown) d.link = InferLink(d.via);
        if (d.level == Level::None) d.level = InferLevel(d.levelText);
    }
    return ctx.out;
}

std::wstring DumpHidCollections() {
    std::wstring out = L"VID:PID    IF  UsagePg:Usage  In  Out Feat  Product\n";
    for (const auto& c : EnumHid()) {
        wchar_t line[160];
        swprintf(line, 160, L"%04X:%04X  %2d  %04X:%04X    %3u %3u %3u   ", c.vid, c.pid, c.iface, c.page(), c.usage(), c.inLen(), c.outLen(),
                 c.featLen());
        out += line + HidProduct(c) + L"\n";
    }
    out += DumpExtraNodes();
    out += DumpXentaFrames();

    auto found = PollAllDevices();
    out += L"\nDetected devices (" + std::to_wstring(found.size()) + L"):\n";
    for (const auto& d : found) {
        wchar_t line[512];
        wsprintfW(line, L"  [%s] %s  via %s  -> %d%%%s%s%s",
                  d.key.c_str(), d.name.c_str(), d.via.c_str(), d.percent,
                  d.charging ? L" (charging)" : L"",
                  d.asleep ? L" (sleeping)" : L"",
                  d.connected ? L"" : L" (disconnected)");
        out += line;
        if (d.millivolts > 0) {
            wsprintfW(line, L" (%d mV)", d.millivolts);
            out += line;
        }
        if (!d.detail.empty()) out += L" [" + d.detail + L"]";
        out += L"\n";
    }
    return out;
}
