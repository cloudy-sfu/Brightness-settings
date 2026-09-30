#include "app_ui.h"

#include "app_state.h"
#include "autostart.h"
#include "curve_editor.h"
#include "monitors.h"
#include "settings.h"
#include "windows_ui.h"

#include <highlevelmonitorconfigurationapi.h>

#include "imgui.h"

void RenderApplicationUi() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin(
        "Content",
        nullptr,
        ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse);

    bool autoStart = IsStartAtLoginConfigured();
    if (ImGui::Checkbox("Start when system login", &autoStart)) {
        SetStartAtLoginConfigured(autoStart);
        SetAutoStart(autoStart);
    }

    const float currentLux = g_CurrentLux.load(std::memory_order_relaxed);
    if (g_SensorConnected.load(std::memory_order_relaxed)) {
        ImGui::TextColored(
            ImVec4(0.2f, 0.9f, 0.3f, 1.0f),
            "VEML7700: Connected | Ambient light: %.1f Lux",
            currentLux);
    } else {
        ImGui::TextDisabled(
            "VEML7700: Not detected | Waiting for device ...");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Text("Monitors");
    ImGui::Spacing();

    bool rescanRequested = false;
    {
        std::lock_guard<std::mutex> lock(g_MonitorsMutex);
        if (g_Monitors.empty()) {
            ImGui::TextDisabled("No DDC/CI compatible monitors detected.");
            rescanRequested = ImGui::Button("Rescan");
        } else if (
            ImGui::BeginTabBar("MonitorTabs", ImGuiTabBarFlags_None)) {
            for (size_t i = 0; i < g_Monitors.size(); ++i) {
                auto& monitor = g_Monitors[i];
                const std::string tabTitle = std::to_string(i + 1);
                if (!ImGui::BeginTabItem(tabTitle.c_str())) continue;

                ImGui::Spacing();
                if (ImGui::Button("Identify")) {
                    ShowIdentifyOverlay(static_cast<int>(i), monitor.rect);
                }

                ImGui::Spacing();
                ImGui::Text("Brightness: %d%%", monitor.sliderValue);
                if (!monitor.displayActive) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("Display in standby, updates paused.");
                }
                if (monitor.autoBrightness) ImGui::BeginDisabled();
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderInt(
                    "##brightness",
                    &monitor.sliderValue,
                    monitor.minBrightness,
                    monitor.maxBrightness,
                    "");
                if (!monitor.autoBrightness &&
                    ImGui::IsItemDeactivatedAfterEdit()) {
                    if (SetMonitorBrightness(
                            monitor.hPhysical,
                            static_cast<DWORD>(monitor.sliderValue))) {
                        monitor.currentBrightness = monitor.sliderValue;
                    } else {
                        monitor.sliderValue = monitor.currentBrightness;
                    }
                }
                if (monitor.autoBrightness) ImGui::EndDisabled();

                ImGui::Spacing();
                const std::string checkboxLabel =
                    "Automatically adjust based on environmental brightness##" +
                    std::to_string(i);
                if (ImGui::Checkbox(
                        checkboxLabel.c_str(),
                        &monitor.autoBrightness)) {
                    if (monitor.autoBrightness) {
                        monitor.lastBrightnessReadTick = 0;
                    }
                    SaveMonitorSettings(monitor);
                }

                ImGui::Spacing();
                ImGui::Text("Brightness function");
                const std::string curveId =
                    "CurveEditor##" + std::to_string(i);
                if (RenderCurveEditor(
                        curveId.c_str(), monitor.curve, currentLux)) {
                    SaveMonitorSettings(monitor);
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }

    ImGui::End();
    if (rescanRequested) RefreshMonitors();
}
