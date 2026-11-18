#include "battery_model.h"

#include <cmath>
#include <cwchar>

std::wstring FormatBatteryEta(int percent, bool charging, double ratePerHour, bool turkish) {
    if (ratePerHour <= 0.1 || ratePerHour >= 200.0 || percent < 0 || percent > 100) return L"";
    wchar_t buf[128] = {};
    if (charging) {
        int remPct = 100 - percent;
        if (remPct <= 0) return L"";
        int mins = (int)((remPct / ratePerHour) * 60.0);
        if (mins <= 0 || mins >= 1440) return L"";
        if (mins >= 60) {
            if (turkish)
                swprintf_s(buf, L"~%d sa %d dk sonra tam dolu (+%.0f%%/sa)", mins / 60, mins % 60, ratePerHour);
            else
                swprintf_s(buf, L"~%dh %dm to full (+%.0f%%/h)", mins / 60, mins % 60, ratePerHour);
        } else {
            if (turkish)
                swprintf_s(buf, L"~%d dk sonra tam dolu (+%.0f%%/sa)", mins, ratePerHour);
            else
                swprintf_s(buf, L"~%dm to full (+%.0f%%/h)", mins, ratePerHour);
        }
        return buf;
    }

    if (percent <= 0) return L"";
    double hoursLeft = percent / ratePerHour;
    if (hoursLeft <= 0.05 || hoursLeft >= 500.0) return L"";
    int totalMins = (int)(hoursLeft * 60.0);
    if (totalMins >= 60) {
        if (turkish)
            swprintf_s(buf, L"~%d sa %d dk kald\x0131 (-%.1f%%/sa)", totalMins / 60, totalMins % 60, ratePerHour);
        else
            swprintf_s(buf, L"~%dh %dm left (-%.1f%%/h)", totalMins / 60, totalMins % 60, ratePerHour);
    } else {
        if (turkish)
            swprintf_s(buf, L"~%d dk kald\x0131 (-%.1f%%/sa)", totalMins, ratePerHour);
        else
            swprintf_s(buf, L"~%dm left (-%.1f%%/h)", totalMins, ratePerHour);
    }
    return buf;
}

void UpdateBatteryTrack(BatteryTrackState& tr, int percent, bool charging, std::uint64_t nowMs, bool turkish) {
    if (percent < 0 || percent > 100) return;

    // Record into ring buffer (max 30 samples) when value changes or at least 5 minutes elapsed.
    if (tr.history.empty() || tr.history.back().percent != percent ||
        (nowMs > tr.history.back().tickMs && nowMs - tr.history.back().tickMs >= 5 * 60 * 1000ULL)) {
        if (tr.history.size() >= 30) tr.history.erase(tr.history.begin());
        tr.history.push_back({ nowMs, percent });
    }

    if (tr.startPct < 0 || tr.wasCharging != charging ||
        (charging && percent < tr.startPct) || (!charging && percent > tr.startPct)) {
        tr.startPct = percent;
        tr.startTick = nowMs;
        tr.wasCharging = charging;
        tr.ratePerHour = 0.0;
        tr.etaText.clear();
        return;
    }

    int diff = std::abs(percent - tr.startPct);
    double hours = (nowMs > tr.startTick) ? ((nowMs - tr.startTick) / 3600000.0) : 0.0;
    if (diff >= 1 && hours >= 0.05) { // at least 3 minutes & 1% change
        tr.ratePerHour = diff / hours;
        tr.etaText = FormatBatteryEta(percent, charging, tr.ratePerHour, turkish);
    } else if (tr.ratePerHour > 0.1) {
        // Re-format ETA for the updated language or current percentage
        tr.etaText = FormatBatteryEta(percent, charging, tr.ratePerHour, turkish);
    }
}
