// ATK N9 Pro (VID 373B: dongle 11CB, wired 11CA) and N9 Ultra (dongle 1204, wired 1203) headsets.
// Queried over the USB virtual COM port (9600 8N1) with 64-byte frames:
//   request [len = 2][cmd][sub][0...]        reply [len][cmd][sub][data...]
//   cmd 00 sub 04 = status  -> data[1] == 1 when the headset is connected to the dongle
//   cmd 00 sub 03 = battery -> data[1] = percent (bits 0-6), charging (bit 7)
// If the ATK HUB desktop/web app is running and holds the COM port exclusively, the headset is
// reported as connected+asleep with a 30-minute grace period so the last cached battery % remains visible.
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
    { 0x11CB, L"N9Pro",   L"ATK N9 Pro",   false },
    { 0x11CA, L"N9Pro",   L"ATK N9 Pro",   true  },
    { 0x1204, L"N9Ultra", L"ATK N9 Ultra", false },
    { 0x1203, L"N9Ultra", L"ATK N9 Ultra", true  },
};

struct Port { std::wstring name; const Model* model; };

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
        const Model* model = nullptr;
        for (const Model& m : kModels)
            if (id.find(L"VID_373B&PID_" + Hex4(m.pid)) != std::wstring::npos) model = &m;
        if (!model) continue;
        HKEY k = SetupDiOpenDevRegKey(di, &dd, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (k == INVALID_HANDLE_VALUE) continue;
        wchar_t name[32] = {};
        DWORD sz = sizeof(name) - sizeof(wchar_t), type = 0;
        if (RegQueryValueExW(k, L"PortName", nullptr, &type, (BYTE*)name, &sz) == ERROR_SUCCESS && type == REG_SZ)
            out.push_back({ name, model });
        RegCloseKey(k);
    }
    SetupDiDestroyDeviceInfoList(di);
    return out;
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
        USHORT pid = p.model->pid;
        if (ctx.IsClaimed(0x373B, pid)) continue;

        DeviceStatus s;
        s.key = std::wstring(L"atk-serial:") + p.model->id;
        s.name = p.model->name;
        s.kind = DeviceKind::Headset;
        s.via = p.model->wired ? L"USB cable" : L"2.4 GHz receiver";
        s.graceSec = 1800; // 30 minutes grace period so cached battery remains visible when asleep or port busy

        Serial port(p.name);
        if (!port.ok()) {
            // Port is held by ATK HUB; keep device as asleep so the main loop uses the cached reading.
            s.connected = true;
            s.asleep = true;
            s.hint = L"ATK HUB a\x00E7\x0131kken COM portu me\x015Fgul olabilir (\x00F6nbellek kullan\x0131l\x0131yor)";
            MergeByKey(ctx, s, 0x373B, pid, false);
            continue;
        }

        std::vector<BYTE> data;
        if (!port.Command(0x00, 0x04, data) || data.size() < 2) {
            s.connected = false;
            MergeByKey(ctx, s, 0x373B, pid, false);
            continue;
        }
        s.connected = data[1] == 1;
        if (s.connected && port.Command(0x00, 0x03, data) && data.size() >= 2) {
            s.percent = Clamp100(data[1] & 0x7F);
            s.charging = (data[1] & 0x80) != 0 || p.model->wired;
        }
        MergeByKey(ctx, s, 0x373B, pid, p.model->wired);
    }
}
