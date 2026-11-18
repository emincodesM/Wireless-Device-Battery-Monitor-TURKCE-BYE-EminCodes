// ATK Neptune N9 Pro / N9 Ultra wireless headsets (VID 373E, BES chip).
// Despite exposing two vendor HID collections, ATK HUB talks to these headsets over the
// USB CDC virtual serial port (MI_03 -> COMx) at 115200 8N1.
// Frame format: 5A A5 [len_lo] [len_hi] [cmd] [payload...] [sum], where len = 1 + payload size
// and sum = low byte of (len_lo + len_hi + cmd + payload).
//   Host -> device: 5A A5 01 00 22 23  (status query)
//   Device -> host: cmd 0x20 [?, connected]   (1 when headset linked to dongle)
//                   cmd 0x24 [percent]        (0..100)
//                   cmd 0x26 [charging]       (0 or 1)
#include "drivers.h"
#include "atk_serial_protocol.h"

#include <setupapi.h>
#include <initguid.h>
#include <devguid.h>
#include <cwctype>
#include <string>

namespace {

struct Model { USHORT pid; const wchar_t* id; const wchar_t* name; bool wired; };
constexpr Model kModels[] = {
    { 0x1007, L"N9Pro",   L"ATK Neptune N9 Pro",   true  },
    { 0x1009, L"N9Ultra", L"ATK Neptune N9 Ultra", true  },
    { 0x1022, L"N9Pro",   L"ATK Neptune N9 Pro",   false },
    { 0x1023, L"N9Pro",   L"ATK Neptune N9 Pro",   false },
    { 0x1028, L"N9Ultra", L"ATK Neptune N9 Ultra", false },
    { 0x1029, L"N9Ultra", L"ATK Neptune N9 Ultra", false },
};

const Model* FindModel(const std::wstring& hwId) {
    std::wstring u = hwId;
    for (auto& ch : u) ch = towupper(ch);
    if (u.find(L"VID_373E") == std::wstring::npos) return nullptr;
    auto p = u.find(L"PID_");
    if (p == std::wstring::npos) return nullptr;
    USHORT pid = (USHORT)wcstoul(u.c_str() + p + 4, nullptr, 16);
    for (const auto& m : kModels)
        if (m.pid == pid) return &m;
    return nullptr;
}

std::wstring ExtractComName(const std::wstring& friendly) {
    auto l = friendly.rfind(L"(COM");
    if (l == std::wstring::npos) return {};
    auto r = friendly.find(L')', l);
    if (r == std::wstring::npos) return {};
    return friendly.substr(l + 1, r - l - 1);
}

struct SerialHandle {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~SerialHandle() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    bool ok() const { return h != INVALID_HANDLE_VALUE; }
};

SerialHandle OpenCom(const std::wstring& com) {
    SerialHandle s;
    std::wstring path = L"\\\\.\\" + com;
    s.h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (!s.ok()) return s;

    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (GetCommState(s.h, &dcb)) {
        dcb.BaudRate = 115200;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        SetCommState(s.h, &dcb);
    }
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout = 30;
    to.ReadTotalTimeoutConstant = 250;
    to.WriteTotalTimeoutConstant = 200;
    SetCommTimeouts(s.h, &to);
    PurgeComm(s.h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return s;
}

}  // namespace

void PollAtkSerial(PollContext& ctx) {
    HDEVINFO di = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (di == INVALID_HANDLE_VALUE) return;

    SP_DEVINFO_DATA dd{ sizeof(dd) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(di, i, &dd); i++) {
        wchar_t id[512] = {};
        if (!SetupDiGetDeviceInstanceIdW(di, &dd, id, 512, nullptr)) continue;
        const Model* m = FindModel(id);
        if (!m) continue;

        wchar_t friendly[256] = {};
        SetupDiGetDeviceRegistryPropertyW(di, &dd, SPDRP_FRIENDLYNAME, nullptr, (BYTE*)friendly, sizeof(friendly), nullptr);
        std::wstring com = ExtractComName(friendly);
        if (com.empty()) continue;

        DeviceStatus s;
        // Stable model-based key so switching USB ports or connecting USB cable + dongle simultaneously
        // updates the same device card.
        s.key = std::wstring(L"atk-serial:") + m->id;
        s.name = m->name;
        s.kind = DeviceKind::Headset;
        s.via = m->wired ? L"USB cable" : L"2.4 GHz receiver";
        s.graceSec = 1800; // Keep last known battery for up to 30 min when busy or asleep

        auto port = OpenCom(com);
        if (!port.ok()) {
            // Port is busy (e.g. ATK HUB / Web HUB holds exclusive access to the COM port).
            // Report connected+asleep so the main loop preserves the last cached battery level.
            s.connected = true;
            s.asleep = true;
            s.hint = L"ATK HUB a\x00E7\x0131kken COM portu me\x015Fgul olabilir (\x00F6nbellek kullan\x0131l\x0131yor)";
            MergeByKey(ctx, s, 0x373E, m->pid, false);
            continue;
        }

        const BYTE query[] = { 0x5A, 0xA5, 0x01, 0x00, 0x22, 0x23 };
        DWORD wr = 0;
        if (!WriteFile(port.h, query, sizeof(query), &wr, nullptr) || wr != sizeof(query)) {
            s.connected = false;
            MergeByKey(ctx, s, 0x373E, m->pid, false);
            continue;
        }

        std::vector<BYTE> buf(256);
        DWORD rd = 0;
        ReadFile(port.h, buf.data(), (DWORD)buf.size(), &rd, nullptr);
        buf.resize(rd);

        auto frames = ParseAtkSerialFrames(buf);
        int pct = -1;
        bool chg = false;
        bool connected = true;
        if (!DecodeAtkSerialStatus(frames, m->wired, pct, chg, connected)) {
            if (!connected) {
                s.connected = false;
                MergeByKey(ctx, s, 0x373E, m->pid, false);
            } else {
                // Dongle did not reply in time; treat as temporary sleep/cache hit.
                s.connected = true;
                s.asleep = true;
                MergeByKey(ctx, s, 0x373E, m->pid, false);
            }
            continue;
        }

        s.percent = Clamp100(pct);
        s.charging = chg;
        s.connected = true;
        s.asleep = false;
        MergeByKey(ctx, s, 0x373E, m->pid, m->wired);
    }
    SetupDiDestroyDeviceInfoList(di);
}
