#pragma once
// Pure protocol helpers for HyperX Cloud III S Wireless (03F0:06BE, 03F0:02CC).
// Supports both report 0x0C ([0C 02 03 01 00 <cmd>], LennardKittner/HyperHeadset) and
// legacy 0xAA ([AA 00 00 02 <cmd> 01]) framing across usage page 0x01C0 and >= 0xFF00.
#include <cstddef>
#include <cstdint>

inline bool IsCloud3SCollection(std::uint16_t page, std::uint16_t inLen, std::uint16_t outLen) {
    return (page == 0x01C0 || page >= 0xFF00) && (inLen == 64 || inLen == 65) && (outLen == 64 || outLen == 65);
}

struct Cloud3SDecodeResult {
    bool valid = false;
    bool connected = false;
    bool charging = false;
    int percent = -1;
};

// Decodes replies to report 0x0C sub-commands 0x02 (conn), 0x06 (pct), 0x48 (charge)
// or legacy 0xAA sub-commands 0x09 (pct), 0x08 (charge).
inline Cloud3SDecodeResult DecodeCloud3SReplies(const std::uint8_t* rPct, std::size_t nPct,
                                                const std::uint8_t* rChg, std::size_t nChg) {
    Cloud3SDecodeResult res;
    bool ok09 = rPct && nPct >= 7 && rPct[0] == 0xAA && rPct[1] == 0x00 && rPct[2] == 0x00 && rPct[3] == 0x02 && rPct[4] == 0x09;
    bool ok08 = rChg && nChg >= 7 && rChg[0] == 0xAA && rChg[1] == 0x00 && rChg[2] == 0x00 && rChg[3] == 0x02 && rChg[4] == 0x08;
    if (ok09 || ok08) {
        res.valid = true;
        int pct = ok09 ? rPct[6] : 0;
        std::uint8_t state = ok08 ? rChg[6] : 0;
        if (state == 0xFF || (pct == 0 && (!ok08 || state == 0))) {
            res.connected = false;
            return res;
        }
        res.connected = true;
        res.charging = (state == 1 || state == 2);
        if (pct >= 1 && pct <= 100) res.percent = pct;
        else if (res.charging)      res.percent = 100;
        else                        res.connected = false;
        return res;
    }

    // Report 0x0C framing: rPct = reply to cmd 0x06, rChg = reply to cmd 0x48
    bool ok06 = rPct && nPct >= 7 && rPct[0] == 0x0C && rPct[5] == 0x06;
    bool ok48 = rChg && nChg >= 7 && rChg[0] == 0x0C && rChg[5] == 0x48;
    if (!ok06 && !ok48) return res;
    res.valid = true;
    int pct = (ok06 && rPct[6] != 0xFF) ? rPct[6] : 0;
    std::uint8_t chg = ok48 ? rChg[6] : 0;
    if (chg == 0xFF && pct == 0) {
        res.connected = false;
        return res;
    }
    res.connected = true;
    res.charging = (chg == 1 || chg == 2);
    if (pct >= 1 && pct <= 100) res.percent = pct;
    else if (res.charging)      res.percent = 100;
    return res;
}
