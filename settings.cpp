#include "settings.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <string>

namespace {

constexpr int SETTINGS_VERSION = 1;

struct MonitorSettings {
    bool autoBrightness = false;
    std::vector<CurvePoint> curve = DefaultBrightnessCurve();
};

bool g_StartAtLogin = false;
std::map<std::string, MonitorSettings> g_MonitorSettings;
std::filesystem::path g_SettingsPath;

std::filesystem::path GetSettingsPath() {
    std::wstring executablePath(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr,
        executablePath.data(),
        static_cast<DWORD>(executablePath.size()));
    if (length == 0 || length >= executablePath.size()) {
        return L"brightness-settings.ini";
    }

    executablePath.resize(length);
    return std::filesystem::path(executablePath).parent_path() /
           L"brightness-settings.ini";
}

std::string Encode(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char character : value) {
        const bool safe =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '-' || character == '_' || character == '.';
        if (safe) {
            result.push_back(static_cast<char>(character));
        } else {
            result.push_back('%');
            result.push_back(hex[character >> 4]);
            result.push_back(hex[character & 0x0F]);
        }
    }
    return result;
}

int HexValue(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    return -1;
}

bool Decode(const std::string& value, std::string& result) {
    result.clear();
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '%') {
            result.push_back(value[i]);
            continue;
        }
        if (i + 2 >= value.size()) return false;
        const int high = HexValue(value[i + 1]);
        const int low = HexValue(value[i + 2]);
        if (high < 0 || low < 0) return false;
        result.push_back(static_cast<char>((high << 4) | low));
        i += 2;
    }
    return true;
}

bool ParseBoolean(const std::string& value, bool& result) {
    if (value == "1" || value == "true") {
        result = true;
        return true;
    }
    if (value == "0" || value == "false") {
        result = false;
        return true;
    }
    return false;
}

bool ParseCurve(const std::string& value, std::vector<CurvePoint>& curve) {
    curve.clear();
    std::istringstream input(value);
    input.imbue(std::locale::classic());
    std::string pointText;

    while (std::getline(input, pointText, ',')) {
        const size_t separator = pointText.find(':');
        if (separator == std::string::npos) return false;

        try {
            size_t luxEnd = 0;
            size_t brightnessEnd = 0;
            const float lux = std::stof(pointText.substr(0, separator), &luxEnd);
            const std::string brightnessText = pointText.substr(separator + 1);
            const float brightness =
                std::stof(brightnessText, &brightnessEnd);
            if (luxEnd != separator ||
                brightnessEnd != brightnessText.size() ||
                !std::isfinite(lux) ||
                !std::isfinite(brightness)) {
                return false;
            }
            curve.push_back({lux, brightness});
        } catch (...) {
            return false;
        }
    }

    if (curve.size() < 2 || curve.size() > 1000) return false;
    for (size_t i = 0; i < curve.size(); ++i) {
        if (curve[i].lux < 0.0f ||
            curve[i].lux > MAX_VEML_LUX ||
            curve[i].brightness < 0.0f ||
            curve[i].brightness > 100.0f ||
            (i > 0 && curve[i].lux <= curve[i - 1].lux) ||
            (i > 0 && curve[i].brightness < curve[i - 1].brightness)) {
            return false;
        }
    }

    curve.front() = {0.0f, 0.0f};
    curve.back().brightness = 100.0f;
    return true;
}

void SaveSettings() {
    if (g_SettingsPath.empty()) g_SettingsPath = GetSettingsPath();
    const std::filesystem::path temporaryPath =
        g_SettingsPath.wstring() + L".tmp";

    std::ofstream output(temporaryPath, std::ios::trunc);
    if (!output) return;
    output.imbue(std::locale::classic());
    output << "version=" << SETTINGS_VERSION << '\n';
    output << "start_with_windows=" << (g_StartAtLogin ? 1 : 0) << '\n';
    output << "monitor_count=" << g_MonitorSettings.size() << '\n';
    output << std::setprecision(std::numeric_limits<float>::max_digits10);

    size_t index = 0;
    for (const auto& [id, settings] : g_MonitorSettings) {
        output << "monitor." << index << ".id=" << Encode(id) << '\n';
        output << "monitor." << index << ".auto="
               << (settings.autoBrightness ? 1 : 0) << '\n';
        output << "monitor." << index << ".curve=";
        for (size_t point = 0; point < settings.curve.size(); ++point) {
            if (point != 0) output << ',';
            output << settings.curve[point].lux << ':'
                   << settings.curve[point].brightness;
        }
        output << '\n';
        ++index;
    }
    output.close();
    if (!output) {
        std::error_code error;
        std::filesystem::remove(temporaryPath, error);
        return;
    }

    if (!MoveFileExW(
            temporaryPath.c_str(),
            g_SettingsPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code error;
        std::filesystem::remove(temporaryPath, error);
    }
}

bool LoadSettings() {
    std::ifstream input(g_SettingsPath);
    if (!input) return false;

    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t separator = line.find('=');
        if (separator == std::string::npos) continue;
        values[line.substr(0, separator)] = line.substr(separator + 1);
    }

    if (values["version"] != std::to_string(SETTINGS_VERSION)) return false;
    bool startAtLogin = false;
    if (!ParseBoolean(values["start_with_windows"], startAtLogin)) return false;

    size_t monitorCount = 0;
    try {
        size_t parsedLength = 0;
        monitorCount = std::stoul(values["monitor_count"], &parsedLength);
        if (parsedLength != values["monitor_count"].size() ||
            monitorCount > 1000) {
            return false;
        }
    } catch (...) {
        return false;
    }

    std::map<std::string, MonitorSettings> monitorSettings;
    for (size_t i = 0; i < monitorCount; ++i) {
        const std::string prefix = "monitor." + std::to_string(i);
        std::string id;
        bool autoBrightness = false;
        std::vector<CurvePoint> curve;
        if (!Decode(values[prefix + ".id"], id) ||
            id.empty() ||
            !ParseBoolean(values[prefix + ".auto"], autoBrightness) ||
            !ParseCurve(values[prefix + ".curve"], curve)) {
            return false;
        }
        monitorSettings[id] = {autoBrightness, std::move(curve)};
    }

    g_StartAtLogin = startAtLogin;
    g_MonitorSettings = std::move(monitorSettings);
    return true;
}

} // namespace

std::vector<CurvePoint> DefaultBrightnessCurve() {
    return {{0.0f, 0.0f}, {MAX_VEML_LUX, 100.0f}};
}

void InitializeSettings() {
    g_SettingsPath = GetSettingsPath();
    if (!LoadSettings()) {
        g_StartAtLogin = false;
        g_MonitorSettings.clear();
        SaveSettings();
    }
}

bool IsStartAtLoginConfigured() {
    return g_StartAtLogin;
}

void SetStartAtLoginConfigured(bool enabled) {
    if (g_StartAtLogin == enabled) return;
    g_StartAtLogin = enabled;
    SaveSettings();
}

void ApplySettingsToMonitor(MonitorEntry& monitor) {
    const auto existing = g_MonitorSettings.find(monitor.settingsId);
    if (existing != g_MonitorSettings.end()) {
        monitor.autoBrightness = existing->second.autoBrightness;
        monitor.curve = existing->second.curve;
        return;
    }

    monitor.autoBrightness = false;
    monitor.curve = DefaultBrightnessCurve();
    g_MonitorSettings[monitor.settingsId] = {
        monitor.autoBrightness,
        monitor.curve
    };
    SaveSettings();
}

void SaveMonitorSettings(const MonitorEntry& monitor) {
    g_MonitorSettings[monitor.settingsId] = {
        monitor.autoBrightness,
        monitor.curve
    };
    SaveSettings();
}
