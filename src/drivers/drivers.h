#pragma once
// Each driver looks at the enumerated HID collections, talks to the devices it recognises and
// appends what it finds. A driver claims a device (vendor/product id) once it has handled it so
// that the generic fallbacks further down the chain do not report it twice.
#include <set>
#include <utility>
#include <vector>

#include "../devices.h"
#include "../hid.h"

struct PollContext {
    std::vector<HidColl> colls;
    std::set<std::pair<USHORT, USHORT>> claimed;
    std::set<std::wstring> btContainers; // container ids PollBluetooth already reported
    std::vector<DeviceStatus> out;

    bool IsClaimed(USHORT vid, USHORT pid) const { return claimed.count({ vid, pid }) != 0; }
    void Claim(USHORT vid, USHORT pid) { claimed.insert({ vid, pid }); }

    void Add(const DeviceStatus& s, USHORT vid, USHORT pid) {
        out.push_back(s);
        if (!out.back().vid) out.back().vid = vid;
        if (out.back().link == Link::Unknown) out.back().link = InferLink(out.back().via);
        if (out.back().level == Level::None) out.back().level = InferLevel(out.back().levelText);
        Claim(vid, pid);
    }

    // Distinct (vid, pid) pairs for a vendor (or all vendors when vid == 0) that are not claimed yet.
    std::vector<std::pair<USHORT, USHORT>> Products(USHORT vid = 0) const {
        std::set<std::pair<USHORT, USHORT>> s;
        for (const auto& c : colls)
            if ((vid == 0 || c.vid == vid) && !IsClaimed(c.vid, c.pid))
                s.insert({ c.vid, c.pid });
        return { s.begin(), s.end() };
    }

    // Collections of one product, optionally filtered.
    template <class Pred>
    std::vector<const HidColl*> Find(USHORT vid, USHORT pid, Pred pred) const {
        std::vector<const HidColl*> r;
        for (const auto& c : colls)
            if (c.vid == vid && (pid == 0 || c.pid == pid) && pred(c))
                r.push_back(&c);
        return r;
    }
};

// Merges a device status by `s.key` so dual-mode connections (e.g. USB cable + 2.4 GHz dongle
// plugged in simultaneously) update a single card rather than creating duplicate entries.
inline void MergeByKey(PollContext& ctx, const DeviceStatus& s, USHORT vid = 0, USHORT pid = 0, bool preferNew = false) {
    for (auto& existing : ctx.out) {
        if (existing.key == s.key) {
            bool takeNew = preferNew ||
                           (!existing.connected && s.connected) ||
                           (existing.asleep && !s.asleep && s.connected) ||
                           (existing.percent < 0 && s.percent >= 0);
            if (takeNew) {
                existing = s;
                if (vid && !existing.vid) existing.vid = vid;
                if (existing.link == Link::Unknown) existing.link = InferLink(existing.via);
                if (existing.level == Level::None) existing.level = InferLevel(existing.levelText);
            }
            if (vid && pid) ctx.Claim(vid, pid);
            return;
        }
    }
    if (vid && pid) {
        ctx.Add(s, vid, pid);
    } else {
        ctx.out.push_back(s);
        if (ctx.out.back().link == Link::Unknown) ctx.out.back().link = InferLink(ctx.out.back().via);
        if (ctx.out.back().level == Level::None) ctx.out.back().level = InferLevel(ctx.out.back().levelText);
    }
}

// G-Wolves model names (shared by the Glorious- and Kysona-protocol drivers).
const wchar_t* GWolvesModelName(USHORT pid);
bool GWolvesIsCable(USHORT pid);

// Raw Xenta (1D57) status frames, for `--dump`.
std::wstring DumpXentaFrames();

// Vendor drivers (run in order).
void PollGlorious(PollContext& ctx);
void PollAula(PollContext& ctx);
void PollDareu(PollContext& ctx);
void PollDarmoshark(PollContext& ctx);
void PollAtkSerial(PollContext& ctx);
void PollAtkKeyboard(PollContext& ctx);
void PollPulsar(PollContext& ctx);
void PollAtkBitmouse(PollContext& ctx);
void PollKysona(PollContext& ctx);
void PollAtkNordic54(PollContext& ctx);
void PollKeychron(PollContext& ctx);
void PollCherry(PollContext& ctx);
void PollArbiter(PollContext& ctx);
void PollLamzu(PollContext& ctx);
void PollEndgame(PollContext& ctx);
void PollEndgameWe(PollContext& ctx);
void PollVaxee(PollContext& ctx);
void PollWlmouse(PollContext& ctx);
void PollFinalmouse(PollContext& ctx);
void PollNinjutso(PollContext& ctx);
void PollSprime(PollContext& ctx);
void PollRongyuan(PollContext& ctx);
void PollSonix(PollContext& ctx);
void PollSemico(PollContext& ctx);
void PollSonyInzone(PollContext& ctx);
void PollFractal(PollContext& ctx);
void PollJabra(PollContext& ctx);
void PollJbl(PollContext& ctx);
void PollRog(PollContext& ctx);
void PollZowie(PollContext& ctx);
void PollRoyalKludge(PollContext& ctx);
void PollMchose(PollContext& ctx);
void PollRapoo(PollContext& ctx);
void PollAreson(PollContext& ctx);
void PollMchoseG3(PollContext& ctx);
void PollMsi(PollContext& ctx);
void PollMachenike(PollContext& ctx);
void PollNiceboy(PollContext& ctx);
void PollMchoseModern(PollContext& ctx);
void PollMchoseHeadset(PollContext& ctx);
void PollMiscMice(PollContext& ctx);
void PollFantech(PollContext& ctx);
void PollRazerHeadsets(PollContext& ctx);
void PollBarracudaPro(PollContext& ctx);
void PollHyperX(PollContext& ctx);
void PollLogitechG435(PollContext& ctx);
void PollAstro(PollContext& ctx);
void PollLogitech(PollContext& ctx);
void PollRazer(PollContext& ctx);
void PollSteelSeriesElite(PollContext& ctx);
void PollSteelSeries(PollContext& ctx);
void PollAudeze(PollContext& ctx);
void PollNzxt(PollContext& ctx);
void PollPlantronics(PollContext& ctx);
void PollCorsair(PollContext& ctx);
void PollCoolerMaster(PollContext& ctx);
void PollPlayStation(PollContext& ctx);
void PollSwitchPro(PollContext& ctx);
void PollSteamController(PollContext& ctx);
void PollGameSir(PollContext& ctx);
void PollFlydigi(PollContext& ctx);
void PollGameSirCyclone(PollContext& ctx);
void Poll8BitDo(PollContext& ctx);
void PollBeitong(PollContext& ctx);
void PollAirPods(PollContext& ctx);
void PollPhoneLink(PollContext& ctx);
void PollGalaxyBuds(PollContext& ctx);
void PollMoondrop(PollContext& ctx);
void PollAlienware(PollContext& ctx);
void PollMaono(PollContext& ctx);

// Fallbacks.
void PollHeadsetControl(PollContext& ctx);
void PollBluetooth(PollContext& ctx);
void PollGenericHid(PollContext& ctx);
void PollXInput(PollContext& ctx);
