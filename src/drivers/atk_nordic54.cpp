// ATK Zero / Blazing Sky F1 V2 / Z1 V2 / X1 V2 (Nordic nRF54L15 / VGN Hub V2 64-byte HID mice).
// Runs after PollKysona, PollAtkKeyboard, and PollAtkBitMouse so existing 17-byte Compx, keyboard,
// and bit-packed mice are untouched, and probes remaining unclaimed 64/65-byte vendor collections
// on ATK / VGN vendor IDs (373B, 3710, 373E, 3554).
#include "drivers.h"
#include "atk_nordic54_protocol.h"

namespace {

bool IsAtkMouseVid(USHORT vid, USHORT pid) {
    if (vid == 0x373E) {
        // Skip ATK Neptune N9 headsets (handled by PollAtkSerial).
        if (pid >= 0x1000 && pid <= 0x1030) return false;
        return true;
    }
    return vid == 0x373B || vid == 0x3710 || vid == 0x3554;
}

bool TryProbeCollection(const HidColl& c, AtkNordic54::BatteryReply& out) {
    HidDev d;
    if (!d.Open(c)) return false;

    // 1. Try non-intrusive Feature / Input report read first in case firmware caches status.
    std::vector<BYTE> r;
    for (BYTE rid : { (BYTE)0x08, (BYTE)0x02 }) {
        if (c.featLen() >= 8 && d.GetFeature(rid, r)) {
            out = AtkNordic54::DecodeReply(r.data(), r.size());
            if (out.valid && out.percent >= 1) return true;
        }
    }

    // 2. Try VGN / ATK Hub V2 64-byte battery queries.
    const std::vector<BYTE> queries[] = {
        AtkNordic54::MakeCompxQuery(0x08, 0x04),
        { 0x08, 0x04, 0x00, 0x00, 0x00, 0x49 },
        { 0x02, 0x04, 0x00, 0x00, 0x00 },
        { 0x00, 0x83, 0x00 },
    };

    for (const auto& q : queries) {
        d.Flush();
        if (d.SetOutputAndRead(q, r, 150) >= 8) {
            out = AtkNordic54::DecodeReply(r.data(), r.size());
            if (out.valid) return true;
        }
        d.Flush();
        if (d.Write(q) && d.Read(r, 150) >= 8) {
            out = AtkNordic54::DecodeReply(r.data(), r.size());
            if (out.valid) return true;
        }
    }
    return false;
}

}  // namespace

void PollAtkNordic54(PollContext& ctx) {
    for (auto [vid, pid] : ctx.Products()) {
        if (!IsAtkMouseVid(vid, pid) || ctx.IsClaimed(vid, pid)) continue;

        auto colls = ctx.Find(vid, pid, [](const HidColl& c) {
            return c.page() >= 0xFF00 && c.inLen() >= 64 && c.outLen() >= 64;
        });
        for (const HidColl* c : colls) {
            AtkNordic54::BatteryReply rep;
            if (!TryProbeCollection(*c, rep)) continue;

            std::wstring prod = HidProduct(*c);
            bool receiver = prod.find(L"Receiver") != std::wstring::npos ||
                            prod.find(L"Dongle") != std::wstring::npos ||
                            prod.find(L"4K") != std::wstring::npos ||
                            prod.find(L"8K") != std::wstring::npos ||
                            (pid & 0x0001) == 0;

            DeviceStatus s;
            s.key = L"atk-n54:" + Hex4(vid) + L":" + Hex4(pid);
            s.name = prod.empty() ? L"ATK Wireless Mouse" : prod;
            s.kind = GuessKind(s.name, DeviceKind::Mouse);
            s.via = (!receiver && rep.charging) ? L"USB cable" : L"2.4 GHz receiver";
            s.graceSec = 1800;

            if (!rep.awake) {
                s.connected = true;
                s.asleep = true;
            } else {
                s.percent = Clamp100(rep.percent);
                s.millivolts = rep.millivolts;
                s.charging = rep.charging;
                s.connected = true;
                s.asleep = false;
            }
            MergeByKey(ctx, s, vid, pid, !s.asleep);
            break;
        }
    }
}
