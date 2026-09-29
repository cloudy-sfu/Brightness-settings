#pragma once

#include "app_state.h"

#include <vector>

void InitializeSettings();
bool IsStartAtLoginConfigured();
void SetStartAtLoginConfigured(bool enabled);
void ApplySettingsToMonitor(MonitorEntry& monitor);
void SaveMonitorSettings(const MonitorEntry& monitor);
std::vector<CurvePoint> DefaultBrightnessCurve();
