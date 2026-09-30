#pragma once

#include "app_state.h"

void RefreshMonitors();
void PollManualMonitorBrightness();
void ReleaseMonitors();

// Queries the monitor's DDC/CI power mode. Requires g_MonitorsMutex.
bool UpdateMonitorDisplayActive(MonitorEntry& monitor);
