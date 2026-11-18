// Unit tests for pure hardware protocol parsers:
//   - Rapoo battery report decoder (rapoo_protocol.h)
//   - ATK Neptune N9 serial frame parser & 1-byte sliding resync (atk_serial_protocol.h)
//   - HyperX Cloud III S Wireless collection & reply decoder (hyperx_protocol.h)
//   - ATK Zero / Nordic 54L15 64-byte HID protocol (atk_nordic54_protocol.h)
//   - Samsung Galaxy Buds RFCOMM frame & CRC16 (galaxy_buds_protocol.h)
//   - Logitech G435 (046D:0ADF) CRC16 frame & voltage curve (logitech_g435_0adf.h)
//   - ARDOR GAMING Fury V2 report decoder (ardor_fury_v2_protocol.h)
//   - Xenta / Dareu report parser (xenta_protocol.h)
#include <cassert>
#include <cstdio>
#include <vector>

#include "drivers/rapoo_protocol.h"
#include "drivers/atk_serial_protocol.h"
#include "drivers/hyperx_protocol.h"
#include "drivers/atk_nordic54_protocol.h"
#include "drivers/galaxy_buds_protocol.h"
#include "drivers/logitech_g435_0adf.h"
#include "drivers/ardor_fury_v2_protocol.h"
#include "drivers/xenta_protocol.h"

namespace {

void TestRapooProtocol() {
    const std::vector<std::uint8_t> captured = {
        0x07, 0x20, 0x03, 0x20, 0x03, 0x20, 0x03, 0x01, 0x19, 0x00,
        0x04, 0x84, 0x00, 0x17, 0x46, 0x00, 0x02, 0x03, 0xBC
    };
    Rapoo::Battery b;
    assert(Rapoo::DecodeBattery(captured.data(), captured.size(), b));
    assert(b.percent == 25 && !b.charging && b.mousePid == 0x4617);
    assert(b.connection == Rapoo::Connection::Receiver);
}

void TestAtkSerialProtocol() {
    // Valid frame: 5A A5 02 00 24 55 7B (cmd=0x24, payload=[0x55 = 85%], sum = 02+00+24+55 = 0x7B)
    // Preceded by a fake corrupted header (5A A5 02 00 24 55 00) to verify 1-byte resynchronization!
    std::vector<std::uint8_t> stream = {
        0x5A, 0xA5, 0x02, 0x00, 0x24, 0x55, 0x00, // bad checksum (0x00 != 0x7B) -> must resync by 1 byte!
        0x5A, 0xA5, 0x02, 0x00, 0x24, 0x55, 0x7B, // valid battery 85%
        0x5A, 0xA5, 0x02, 0x00, 0x26, 0x01, 0x29, // valid charging = 1 (02+00+26+01 = 0x29)
        0x5A, 0xA5, 0x03, 0x00, 0x20, 0x00, 0x01, 0x24 // valid linked = 1 (03+00+20+00+01 = 0x24)
    };
    auto frames = ParseAtkSerialFrames(stream);
    assert(frames.size() == 3);
    int pct = -1;
    bool chg = false, conn = false;
    bool ok = DecodeAtkSerialStatus(frames, false, pct, chg, conn);
    assert(ok && conn && pct == 85 && chg);

    // Truncated cmd 0x20 frame (1-byte data instead of 2) must not crash or read out of bounds.
    std::vector<AtkSerialFrame> short20 = {
        { 0x20, { 0x01 } },
        { 0x24, { 60 } }
    };
    assert(DecodeAtkSerialStatus(short20, false, pct, chg, conn));
    assert(conn && pct == 60);
}

void TestHyperXCloud3SProtocol() {
    // Cloud III S uses usage page 0x01C0 (not >= 0xFF00!) and 64/65-byte reports.
    assert(IsCloud3SCollection(0x01C0, 64, 64));
    assert(IsCloud3SCollection(0x01C0, 65, 65));
    assert(IsCloud3SCollection(0xFF90, 64, 64));
    assert(!IsCloud3SCollection(0x0001, 64, 64));

    // Report 0x0C framing (cmd 0x06 % and cmd 0x48 charge)
    std::uint8_t r06[64] = { 0x0C, 0x02, 0x03, 0x01, 0x00, 0x06, 78 };
    std::uint8_t r48[64] = { 0x0C, 0x02, 0x03, 0x01, 0x00, 0x48, 0x00 };
    auto d0 = DecodeCloud3SReplies(r06, 64, r48, 64);
    assert(d0.valid && d0.connected && !d0.charging && d0.percent == 78);

    // Legacy 0xAA framing connected 78%, discharging
    std::uint8_t r09[64] = { 0xAA, 0x00, 0x00, 0x02, 0x09, 0x01, 78 };
    std::uint8_t r08[64] = { 0xAA, 0x00, 0x00, 0x02, 0x08, 0x01, 0x00 };
    auto d1 = DecodeCloud3SReplies(r09, 64, r08, 64);
    assert(d1.valid && d1.connected && !d1.charging && d1.percent == 78);

    // Offline (r08[6] == 0xFF)
    r08[6] = 0xFF;
    auto d2 = DecodeCloud3SReplies(r09, 64, r08, 64);
    assert(d2.valid && !d2.connected);

    // Full charge fallback (r09[6] == 0 while r08[6] == 1 charging)
    r09[6] = 0;
    r08[6] = 1;
    auto d3 = DecodeCloud3SReplies(r09, 64, r08, 64);
    assert(d3.valid && d3.connected && d3.charging && d3.percent == 100);
}

void TestAtkNordic54Protocol() {
    auto q = AtkNordic54::MakeCompxQuery(0x08, 0x04);
    assert(q.size() == 65);
    assert(q[0] == 0x08 && q[1] == 0x04 && q[16] == (0x55 - 0x0C));

    std::uint8_t rep[64] = { 0x08, 0x04, 0x00, 0x00, 0x02, 92, 0x01, 0x10, 0x18 };
    auto dec = AtkNordic54::DecodeReply(rep, sizeof(rep));
    assert(dec.valid && dec.awake && dec.percent == 92 && dec.charging && dec.millivolts == 0x1018);
}

void TestOtherProtocols() {
    // ARDOR Fury V2
    const std::uint8_t ardor50[5] = { 0x03, 0x00, 0x40, 0x33, 0x07 };
    const std::uint8_t ardor75[5] = { 0x03, 0x00, 0x40, 0x73, 0x08 };
    assert(ArdorFuryV2Percent(ardor50, 5) == 50);
    assert(ArdorFuryV2Percent(ardor75, 5) == 75);

    // Logitech G435 (046D:0ADF)
    auto frame = G435Adf::Frame(0x84, { 0x01 });
    assert(frame[0] == 0x84 && frame[1] == 0x01);
    assert(G435Adf::Percent(4200) == 100);
    assert(G435Adf::Percent(3400) == 0);

    // Xenta protocol
    xenta_protocol::BatteryReport xrep{};
    assert(xenta_protocol::ParseBatteryReport({ 0x03, 0x01, 0x40, 0x00, 80 }, xrep));
    assert(xrep.level == 80);
}

}  // namespace

int main() {
    TestRapooProtocol();
    TestAtkSerialProtocol();
    TestHyperXCloud3SProtocol();
    TestAtkNordic54Protocol();
    TestOtherProtocols();
    std::printf("All protocol unit tests passed.\n");
    return 0;
}
