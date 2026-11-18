#pragma once
// Pure protocol helpers for ATK Zero / Blazing Sky F1 V2 / Z1 V2 / X1 V2 (Nordic nRF54L15 / VGN Hub V2
// 64-byte vendor HID collections on VID 373B, 3710, 373E, 3554).
// Kept independent of Win32 I/O so frame builders and reply decoders can be unit-tested.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace AtkNordic54 {

struct BatteryReply {
    bool valid = false;
    bool awake = true;
    int percent = -1;
    int millivolts = 0;
    bool charging = false;
};

// Builds a 17-byte or 65-byte VGN/Compx Gen-2 battery query with sum-complement byte at index 16 (`0x55 - sum`).
inline std::vector<std::uint8_t> MakeCompxQuery(std::uint8_t rid, std::uint8_t cmd) {
    std::vector<std::uint8_t> buf(65, 0);
    buf[0] = rid;
    buf[1] = cmd;
    std::uint8_t sum = static_cast<std::uint8_t>(rid + cmd);
    buf[16] = static_cast<std::uint8_t>(0x55 - sum);
    return buf;
}

// Decodes a 64/65-byte reply from ATK / VGN Hub V2 / Nordic 54L15 mice.
inline BatteryReply DecodeReply(const std::uint8_t* r, std::size_t n) {
    BatteryReply out;
    if (!r || n < 8) return out;

    // Normalize leading report ID when unnumbered (0x00) vs numbered (0x02, 0x08).
    std::size_t off = (r[0] == 0x00 && (r[1] == 0x08 || r[1] == 0x04 || r[1] == 0x07 || r[1] == 0x83)) ? 1 : 0;
    if (off + 7 >= n) return out;
    const std::uint8_t* p = r + off;

    // Layout A: VGN/Compx 64-byte extended report [08, 04, 00, 00, 00/02, pct(0..100), chg(0/1), mv_hi, mv_lo]
    if (p[0] == 0x08 && p[1] == 0x04) {
        if (p[5] >= 1 && p[5] <= 100 && p[6] <= 1) {
            out.valid = true;
            out.percent = p[5];
            out.charging = p[6] == 1;
            if (off + 8 < n) {
                int mv = (p[7] << 8) | p[8];
                if (mv >= 3000 && mv <= 4500) out.millivolts = mv;
            }
            return out;
        }
        // Layout B: [08, 04, status, pct(1..100), chg(0/1), mv_hi, mv_lo]
        if (p[3] >= 1 && p[3] <= 100 && p[4] <= 1) {
            out.valid = true;
            out.percent = p[3];
            out.charging = p[4] == 1;
            int mv = (p[5] << 8) | p[6];
            if (mv >= 3000 && mv <= 4500) out.millivolts = mv;
            return out;
        }
        // Sleeping mouse on receiver: [08, 04, 00, 00, 00, 00, ...]
        if (p[2] == 0 && p[3] == 0 && p[5] == 0) {
            out.valid = true;
            out.awake = false;
            return out;
        }
    }

    // Layout C: ATK Hub V2 / Nordic 54L15 direct command reply [rid/cmd=04 or 83, status=00/01, pct(1..100), chg(0/1), mv_lo, mv_hi]
    if ((p[0] == 0x04 || p[0] == 0x83 || (p[0] == 0x02 && (p[1] == 0x04 || p[1] == 0x83 || p[1] == 0x07)))) {
        std::size_t base = (p[0] == 0x02) ? 1 : 0;
        std::uint8_t pct = p[base + 2];
        std::uint8_t chg = p[base + 3];
        if (pct >= 1 && pct <= 100 && chg <= 1) {
            out.valid = true;
            out.percent = pct;
            out.charging = chg == 1;
            int mvLe = p[base + 4] | (p[base + 5] << 8);
            int mvBe = (p[base + 4] << 8) | p[base + 5];
            if (mvLe >= 3000 && mvLe <= 4500) out.millivolts = mvLe;
            else if (mvBe >= 3000 && mvBe <= 4500) out.millivolts = mvBe;
            return out;
        }
    }

    return out;
}

}  // namespace AtkNordic54
