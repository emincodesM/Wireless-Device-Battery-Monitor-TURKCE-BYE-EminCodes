// Unit tests for driver context merging (MergeByKey), battery ETA / discharge model,
// and Turkish / English i18n helpers.
#include <cassert>
#include <cstdio>
#include <vector>

#include "drivers/drivers.h"
#include "battery_model.h"
#include "i18n.h"

// Minimal stubs for HidProduct / HidDev so test_rapoo_driver.exe links cleanly without hardware I/O.
std::wstring HidProduct(const HidColl& c) { return c.product; }
std::wstring Hex4(USHORT v) { wchar_t b[8]; swprintf(b, 8, L"%04X", v); return b; }
int Clamp100(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }
int MapTo100(int v, int lo, int hi) { return hi <= lo ? Clamp100(v) : Clamp100((v - lo) * 100 / (hi - lo)); }
bool HidDev::Open(const HidColl&) { return false; }
bool HidDev::OpenReadOnly(const HidColl&) { return false; }
bool HidDev::OpenFeatureOnly(const HidColl&) { return false; }
void HidDev::Close() {}
void HidDev::Flush() {}
bool HidDev::Write(const std::vector<BYTE>&, int) { return false; }
int  HidDev::Read(std::vector<BYTE>&, int) { return -1; }
int  HidDev::SetOutputAndRead(const std::vector<BYTE>&, std::vector<BYTE>&, int) { return -1; }
bool HidDev::SetFeature(const std::vector<BYTE>&) { return false; }
bool HidDev::GetFeature(BYTE, std::vector<BYTE>&) { return false; }
bool HidDev::GetFeatureByte(BYTE, BYTE&) { return false; }
bool HidDev::GetInputReport(BYTE, std::vector<BYTE>&) { return false; }

Link InferLink(const std::wstring&) { return Link::Unknown; }
Level InferLevel(const std::wstring&) { return Level::None; }

namespace {

void TestMergeByKey() {
    PollContext ctx;

    DeviceStatus dongle;
    dongle.key = L"atk-serial:N9Ultra";
    dongle.name = L"ATK Neptune N9 Ultra";
    dongle.via = L"2.4 GHz receiver";
    dongle.connected = true;
    dongle.asleep = true;
    dongle.percent = -1;
    MergeByKey(ctx, dongle, 0x373E, 0x1029, false);
    assert(ctx.out.size() == 1);
    assert(ctx.out[0].asleep);

    DeviceStatus cable;
    cable.key = L"atk-serial:N9Ultra";
    cable.name = L"ATK Neptune N9 Ultra";
    cable.via = L"USB cable";
    cable.connected = true;
    cable.asleep = false;
    cable.charging = true;
    cable.percent = 90;
    MergeByKey(ctx, cable, 0x373E, 0x1009, true);

    // Must still be a single card, updated with the awake wired reading!
    assert(ctx.out.size() == 1);
    assert(!ctx.out[0].asleep);
    assert(ctx.out[0].percent == 90);
    assert(ctx.out[0].charging);
    assert(ctx.IsClaimed(0x373E, 0x1029));
    assert(ctx.IsClaimed(0x373E, 0x1009));
}

void TestBatteryModel() {
    BatteryTrackState tr;
    UpdateBatteryTrack(tr, 80, false, 0, true);
    assert(tr.history.size() == 1);
    assert(tr.ratePerHour == 0.0);

    // After 30 minutes (1,800,000 ms), drops to 75% (rate = 10%/hour)
    UpdateBatteryTrack(tr, 75, false, 1800000ULL, true);
    assert(tr.history.size() == 2);
    assert(tr.ratePerHour > 9.9 && tr.ratePerHour < 10.1);
    assert(!tr.etaText.empty());
}

void TestI18nHelpers() {
    assert(IsBluetoothVia(L"Bluetooth"));
    assert(IsBluetoothVia(L"Bluetooth \x00B7 Sol 80%"));
    assert(IsUsbCableVia(L"USB cable"));
    assert(IsUsbCableVia(L"USB kablo"));
    assert(IsReceiverVia(L"2.4 GHz receiver"));
    assert(IsReceiverVia(L"2.4 GHz al\x0131c\x0131"));
}

}  // namespace

int main() {
    TestMergeByKey();
    TestBatteryModel();
    TestI18nHelpers();
    std::printf("All driver & battery model unit tests passed.\n");
    return 0;
}
