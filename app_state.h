#pragma once

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

constexpr UINT WM_TRAYICON = WM_USER + 101;
constexpr UINT IDM_TRAY_OPEN = 2001;
constexpr UINT IDM_TRAY_CLOSE = 2002;
constexpr float MAX_VEML_LUX = 65535.0f * 0.0672f;
constexpr int HYSTERESIS_THRESHOLD_PCT = 5;
constexpr DWORD READ_INTERVAL_MS = 5000;

struct CurvePoint {
    float lux;
    float brightness;
};

struct MonitorEntry {
    HMONITOR hMonitor;
    RECT rect;
    HANDLE hPhysical;
    bool hasPhysicalHandle;
    std::string name;
    int minBrightness;
    int maxBrightness;
    int currentBrightness;
    int sliderValue;
    bool autoBrightness;
    DWORD lastBrightnessReadTick;
    std::vector<CurvePoint> curve;
};

extern std::vector<MonitorEntry> g_Monitors;
extern std::mutex g_MonitorsMutex;
extern ID3D11Device* g_pd3dDevice;
extern ID3D11DeviceContext* g_pd3dDeviceContext;
extern IDXGISwapChain* g_pSwapChain;
extern ID3D11RenderTargetView* g_mainRenderTargetView;
extern NOTIFYICONDATAW g_TrayIcon;
extern std::atomic<bool> g_AppRunning;
extern std::atomic<bool> g_SensorConnected;
extern std::atomic<float> g_CurrentLux;
extern std::atomic<float> g_SmoothedLux;
extern DWORD g_LastGuiPollTime;
extern HANDLE g_hShutdownEvent;
