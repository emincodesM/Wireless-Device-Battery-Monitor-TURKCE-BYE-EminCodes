#pragma once
// Pure protocol parser for ATK Neptune N9 Ultra / N9 Pro serial frames (5A A5 framing).
// Kept independent of Win32 I/O so frame parsing, checksum validation, and 1-byte sliding
// resynchronization can be unit-tested directly.
#include <cstddef>
#include <cstdint>
#include <vector>

struct AtkSerialFrame {
    std::uint8_t cmd = 0;
    std::vector<std::uint8_t> data;
};

inline std::vector<AtkSerialFrame> ParseAtkSerialFrames(const std::vector<std::uint8_t>& buf) {
    std::vector<AtkSerialFrame> out;
    for (std::size_t i = 0; i + 6 <= buf.size();) {
        if (buf[i] != 0x5A || buf[i + 1] != 0xA5) {
            i++;
            continue;
        }
        std::size_t len = buf[i + 2] | (static_cast<std::size_t>(buf[i + 3]) << 8);
        if (len < 1 || i + 4 + len + 1 > buf.size()) {
            i++;
            continue;
        }
        std::uint8_t sum = 0;
        for (std::size_t k = 2; k < 4 + len; k++) sum = static_cast<std::uint8_t>(sum + buf[i + k]);
        if (sum == buf[i + 4 + len]) {
            AtkSerialFrame f;
            f.cmd = buf[i + 4];
            f.data.assign(buf.begin() + static_cast<std::ptrdiff_t>(i + 5),
                          buf.begin() + static_cast<std::ptrdiff_t>(i + 4 + len));
            out.push_back(std::move(f));
            i += 5 + len;
        } else {
            // 1-byte resync: do not jump past `len` if a stray 5A A5 appeared in noise.
            i++;
        }
    }
    return out;
}

inline bool DecodeAtkSerialStatus(const std::vector<AtkSerialFrame>& frames, bool wired,
                                  int& percent, bool& charging, bool& connected) {
    int pct = -1;
    int chg = -1;
    int linked = wired ? 1 : -1;
    for (const auto& f : frames) {
        if (f.cmd == 0x24 && !f.data.empty() && f.data[0] <= 100) pct = f.data[0];
        if (f.cmd == 0x26 && !f.data.empty()) chg = f.data[0] != 0;
        if (f.cmd == 0x20 && f.data.size() >= 2) linked = f.data[1] != 0;
    }
    if (linked == 0) {
        connected = false;
        return true;
    }
    if (pct < 0) return false;
    connected = true;
    percent = pct;
    charging = (chg == 1);
    return true;
}
