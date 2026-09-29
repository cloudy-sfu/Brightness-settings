#include "monitors.h"

#include "app_state.h"

#include <highlevelmonitorconfigurationapi.h>
#include <lowlevelmonitorconfigurationapi.h>

namespace {

BOOL CALLBACK MonitorEnumProc(
    HMONITOR monitor,
    HDC,
    LPRECT monitorRect,
    LPARAM
) {
    DWORD physicalCount = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(
            monitor, &physicalCount) ||
        physicalCount == 0) {
        return TRUE;
    }

    std::vector<PHYSICAL_MONITOR> physicalMonitors(physicalCount);
    if (!GetPhysicalMonitorsFromHMONITOR(
            monitor, physicalCount, physicalMonitors.data())) {
        return TRUE;
    }

    for (const auto& physical : physicalMonitors) {
        DWORD minimum = 0;
        DWORD current = 0;
        DWORD maximum = 100;

        if (!GetMonitorBrightness(
                physical.hPhysicalMonitor,
                &minimum,
                &current,
                &maximum)) {
            DestroyPhysicalMonitor(physical.hPhysicalMonitor);
            continue;
        }

        MonitorEntry entry{};
        entry.hMonitor = monitor;
        entry.rect = *monitorRect;
        entry.hPhysical = physical.hPhysicalMonitor;
        entry.hasPhysicalHandle = true;

        char description[128] = {};
        WideCharToMultiByte(
            CP_UTF8,
            0,
            physical.szPhysicalMonitorDescription,
            -1,
            description,
            sizeof(description),
            nullptr,
            nullptr);
        entry.name = description[0] != '\0'
            ? description
            : "Display " + std::to_string(g_Monitors.size() + 1);

        entry.minBrightness = static_cast<int>(minimum);
        entry.maxBrightness = static_cast<int>(maximum);
        entry.currentBrightness = static_cast<int>(current);
        entry.sliderValue = static_cast<int>(current);
        entry.autoBrightness = false;
        entry.lastBrightnessReadTick = 0;
        entry.curve = {{0.0f, 0.0f}, {MAX_VEML_LUX, 100.0f}};
        g_Monitors.push_back(std::move(entry));
    }

    return TRUE;
}

void ReleaseMonitorsUnlocked() {
    for (auto& monitor : g_Monitors) {
        if (monitor.hasPhysicalHandle) {
            DestroyPhysicalMonitor(monitor.hPhysical);
            monitor.hasPhysicalHandle = false;
        }
    }
    g_Monitors.clear();
}

} // namespace

void RefreshMonitors() {
    std::lock_guard<std::mutex> lock(g_MonitorsMutex);
    ReleaseMonitorsUnlocked();
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, 0);
}

void PollManualMonitorBrightness() {
    const DWORD now = GetTickCount();
    if (now - g_LastGuiPollTime < READ_INTERVAL_MS) {
        return;
    }

    g_LastGuiPollTime = now;
    std::lock_guard<std::mutex> lock(g_MonitorsMutex);
    for (auto& monitor : g_Monitors) {
        if (monitor.autoBrightness || !monitor.hasPhysicalHandle) {
            continue;
        }

        DWORD minimum = 0;
        DWORD current = 0;
        DWORD maximum = 100;
        if (GetMonitorBrightness(
                monitor.hPhysical,
                &minimum,
                &current,
                &maximum)) {
            monitor.minBrightness = static_cast<int>(minimum);
            monitor.maxBrightness = static_cast<int>(maximum);
            monitor.currentBrightness = static_cast<int>(current);
            monitor.sliderValue = static_cast<int>(current);
        }
    }
}

void ReleaseMonitors() {
    std::lock_guard<std::mutex> lock(g_MonitorsMutex);
    ReleaseMonitorsUnlocked();
}
