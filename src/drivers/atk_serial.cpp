// ATK N9 Ultra (VID 373B: dongle 1204, wired 1203) and ATK N9 Pro (VID 373B: dongle 11CB, wired 11CA) headsets.
// Queried over a USB virtual COM port (9600 8N1) with 64-byte frames:
//   request [len = 2][cmd][sub][0...]        reply [len][cmd][sub][data...]
//   cmd 00 sub 04 = status  -> data[1] == 1 when the headset is connected to the dongle
//   cmd 00 sub 03 = battery -> data[1] = percent (bits 0-6), charging (bit 7)
// Protocol from ATK's own configuration software (ATK HUB).
// If ATK HUB is open and holds the COM port, the headset stays visible in asleep/cached state.
#include "drivers.h"

#include <setupapi.h>
#include <initguid.h>
#include <devguid.h>

#include <cwctype>
#include <string>

namespace {

struct Port { std::wstring name; USHORT pid; };

USHORT ExtractPid(const std::wstring& upperId) {
    size_t pos = upperId.find(L"VID_373B&PID_");
    if (pos == std::wstring::npos || pos + 17 > upperId.size()) return 0;
    return (USHORT)wcstoul(upperId.c_str() + pos + 13, nullptr, 16);
}

std::vector<Port> FindPorts() {
    std::vector<Port> out;
    HDEVINFO di = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (di == INVALID_HANDLE_VALUE) return out;
    SP_DEVINFO_DATA dd{ sizeof(dd) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(di, i, &dd); i++) {
        wchar_t inst[512];
        if (!SetupDiGetDeviceInstanceIdW(di, &dd, inst, 512, nullptr)) continue;
        std::wstring id = inst;
        for (auto& ch : id) ch = towupper(ch);
        USHORT pid = ExtractPid(id);
        if (!pid) continue;
        HKEY k = SetupDiOpenDevRegKey(di, &dd, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (k == INVALID_HANDLE_VALUE) continue;
        wchar_t name[32] = {};
        DWORD sz = sizeof(name) - sizeof(wchar_t), type = 0;
        if (RegQueryValueExW(k, L"PortName", nullptr, &type, (BYTE*)name, &sz) == ERROR_SUCCESS && type == REG_SZ)
            out.push_back({ name, pid });
        RegCloseKey(k);
    }
    SetupDiDestroyDeviceInfoList(di);
    return out;
}

bool IsWiredPid(USHORT pid) {
    return pid == 0x11CA || pid == 0x1203;
}

const wchar_t* ModelName(USHORT pid) {
    if (pid == 0x11CB || pid == 0x11CA) return L"ATK N9 Pro";
    return L"ATK N9 Ultra";
}

class Serial {
public:
    explicit Serial(const std::wstring& port) {
        h_ = CreateFileW((L"\\\\.\\" + port).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) return;
        DCB dcb{ sizeof(dcb) };
        GetCommState(h_, &dcb);
        dcb.BaudRate = CBR_9600;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fOutxCtsFlow = dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        dcb.fOutX = dcb.fInX = FALSE;
        COMMTIMEOUTS to{};
        to.ReadIntervalTimeout = 50;
        to.ReadTotalTimeoutConstant = 300;
        to.WriteTotalTimeoutConstant = 300;
        if (!SetCommState(h_, &dcb) || !SetCommTimeouts(h_, &to)) Close();
        else PurgeComm(h_, PURGE_RXCLEAR | PURGE_TXCLEAR);
    }
    ~Serial() { Close(); }
    bool ok() const { return h_ != INVALID_HANDLE_VALUE; }

    // Sends command (cmd, sub) and returns the data bytes of the matching 64-byte reply frame.
    bool Command(BYTE cmd, BYTE sub, std::vector<BYTE>& data) {
        BYTE frame[64] = { 2, cmd, sub };
        DWORD n = 0;
        if (!WriteFile(h_, frame, sizeof(frame), &n, nullptr) || n != sizeof(frame)) return false;
        std::vector<BYTE> buf;
        ULONGLONG end = GetTickCount64() + 800;
        while ((LONGLONG)end - (LONGLONG)GetTickCount64() > 0) {
            BYTE chunk[128];
            if (!ReadFile(h_, chunk, sizeof(chunk), &n, nullptr)) return false;
            buf.insert(buf.end(), chunk, chunk + n);
            while (buf.size() >= 64) {  // the headset also pushes unsolicited frames; skip those
                if (buf[1] == cmd && buf[2] == sub) {
                    data.assign(buf.begin() + 3, buf.begin() + 64);
                    return true;
                }
                buf.erase(buf.begin(), buf.begin() + 64);
            }
        }
        return false;
    }

private:
    void Close() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
    }
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

}  // namespace

void PollAtkSerial(PollContext& ctx) {
    for (const Port& p : FindPorts()) {
        if (ctx.IsClaimed(0x373B, p.pid)) continue;
        DeviceStatus s;
        s.key = L"atk-serial:" + Hex4(p.pid);
        s.name = ModelName(p.pid);
        s.kind = DeviceKind::Headset;
        s.via = IsWiredPid(p.pid) ? L"USB kablosu" : L"2.4 GHz alıcı";
        s.graceSec = 1800;  // keep last known battery for 30 min while asleep or ATK HUB holds the port

        Serial port(p.name);
        if (!port.ok()) {
            // COM port is busy (e.g. ATK HUB is open): keep the device visible in asleep/cached mode.
            s.connected = true;
            s.asleep = true;
            ctx.Add(s, 0x373B, p.pid);
            continue;
        }
        std::vector<BYTE> data;
        if (!port.Command(0x00, 0x04, data)) {
            s.connected = true;
            s.asleep = true;
            ctx.Add(s, 0x373B, p.pid);
            continue;
        }
        s.connected = data[1] == 1;
        if (s.connected && port.Command(0x00, 0x03, data)) {
            s.percent = Clamp100(data[1] & 0x7F);
            s.charging = (data[1] & 0x80) != 0;
        }
        ctx.Add(s, 0x373B, p.pid);
    }
}
