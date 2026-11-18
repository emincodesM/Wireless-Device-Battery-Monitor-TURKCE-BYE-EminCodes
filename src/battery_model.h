#pragma once
// Battery discharge/charge rate and ETA estimation model, plus a 30-sample ring buffer for Flyout sparkline rendering.
#include <cstdint>
#include <string>
#include <vector>

struct BatterySample {
    std::uint64_t tickMs = 0;
    int percent = -1;
};

struct BatteryTrackState {
    int startPct = -1;
    std::uint64_t startTick = 0;
    bool wasCharging = false;
    double ratePerHour = 0.0; // >0: %/hour change rate
    std::wstring etaText;
    std::vector<BatterySample> history; // up to 30 distinct % readings
};

// Pure calculation helpers (testable without Win32).
void UpdateBatteryTrack(BatteryTrackState& tr, int percent, bool charging, std::uint64_t nowMs, bool turkish = true);
std::wstring FormatBatteryEta(int percent, bool charging, double ratePerHour, bool turkish = true);
