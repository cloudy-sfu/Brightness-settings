#include "app_state.h"

std::vector<MonitorEntry> g_Monitors;
std::mutex g_MonitorsMutex;
ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
NOTIFYICONDATAW g_TrayIcon = {};
std::atomic<bool> g_AppRunning{true};
std::atomic<bool> g_SensorConnected{false};
std::atomic<float> g_CurrentLux{0.0f};
DWORD g_LastGuiPollTime = 0;
HANDLE g_hShutdownEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
std::atomic<bool> g_DisplaysOff{false};
HANDLE g_hDisplayStateEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
