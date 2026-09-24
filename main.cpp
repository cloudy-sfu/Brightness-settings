// Ref: https://learn.microsoft.com/en-us/windows/win32/monitor/monitor-configuration
// Ref: https://github.com/ocornut/imgui
// Ref: https://learn.microsoft.com/en-us/windows/win32/hidpi/high-dpi-desktop-application-development-on-windows

#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>
#include <d3d11.h>
#include <vector>
#include <string>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

// Forward declaration from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

struct MonitorEntry {
    HMONITOR hMonitor;
    RECT rect;
    HANDLE hPhysical;
    std::string name;
    int minBrightness;
    int maxBrightness;
    int currentBrightness;
    int sliderValue;
};

static std::vector<MonitorEntry> g_Monitors;
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

// --- DDC/CI Monitor Enumeration & Management ---

BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC, LPRECT lprcMonitor, LPARAM) {
    DWORD numPhysical = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMonitor, &numPhysical) || numPhysical == 0)
        return TRUE;

    std::vector<PHYSICAL_MONITOR> physList(numPhysical);
    if (GetPhysicalMonitorsFromHMONITOR(hMonitor, numPhysical, physList.data())) {
        for (DWORD i = 0; i < numPhysical; ++i) {
            DWORD minB = 0, curB = 0, maxB = 100;
            if (GetMonitorBrightness(physList[i].hPhysicalMonitor, &minB, &curB, &maxB)) {
                MonitorEntry entry;
                entry.hMonitor = hMonitor;
                entry.rect = *lprcMonitor;
                entry.hPhysical = physList[i].hPhysicalMonitor;

                char desc[128] = { 0 };
                WideCharToMultiByte(CP_UTF8, 0, physList[i].szPhysicalMonitorDescription, -1, desc, sizeof(desc), NULL, NULL);
                entry.name = (desc[0] != '\0') ? desc : ("Display " + std::to_string(g_Monitors.size() + 1));
                entry.minBrightness = static_cast<int>(minB);
                entry.maxBrightness = static_cast<int>(maxB);
                entry.currentBrightness = static_cast<int>(curB);
                entry.sliderValue = static_cast<int>(curB);

                g_Monitors.push_back(entry);
            } else {
                DestroyPhysicalMonitor(physList[i].hPhysicalMonitor);
            }
        }
    }
    return TRUE;
}

void RefreshMonitors() {
    for (auto& m : g_Monitors) {
        if (m.hPhysical) DestroyPhysicalMonitor(m.hPhysical);
    }
    g_Monitors.clear();
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, 0);
}

// --- Identify Overlay Window ---

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TIMER:
            DestroyWindow(hwnd);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);

            HBRUSH brush = CreateSolidBrush(RGB(24, 24, 24));
            FillRect(hdc, &rc, brush);
            DeleteObject(brush);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));
            HFONT hFont = CreateFontW(72, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            HFONT oldFont = (HFONT)SelectObject(hdc, hFont);

            wchar_t text[16];
            GetWindowTextW(hwnd, text, 16);
            DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            SelectObject(hdc, oldFont);
            DeleteObject(hFont);
            EndPaint(hwnd, &ps);
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void ShowIdentifyOverlay(int monitorIndex, const RECT& monitorRect) {
    static HWND s_hOverlay = NULL;
    if (s_hOverlay && IsWindow(s_hOverlay)) {
        DestroyWindow(s_hOverlay);
        s_hOverlay = NULL;
    }

    static bool registered = false;
    const wchar_t CLASS_NAME[] = L"IdentifyOverlayWindow";
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
        wc.lpfnWndProc = OverlayWndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = CLASS_NAME;
        RegisterClassExW(&wc);
        registered = true;
    }

    int boxSize = 180;
    int x = monitorRect.left + (monitorRect.right - monitorRect.left - boxSize) / 2;
    int y = monitorRect.top + (monitorRect.bottom - monitorRect.top - boxSize) / 2;

    s_hOverlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        CLASS_NAME,
        std::to_wstring(monitorIndex + 1).c_str(),
        WS_POPUP,
        x, y, boxSize, boxSize,
        NULL, NULL, GetModuleHandle(NULL), NULL
    );

    SetLayeredWindowAttributes(s_hOverlay, 0, 204, LWA_ALPHA);
    ShowWindow(s_hOverlay, SW_SHOW);
    UpdateWindow(s_hOverlay);
    SetTimer(s_hOverlay, 1, 2000, NULL);
}

// --- DirectX 11 Helpers ---

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
        &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);

    if (FAILED(hr)) return false;
    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
        case WM_SIZE:
            if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
                CleanupRenderTarget();
                g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget();
            }
            return 0;
        case WM_DPICHANGED: {
            RECT* const prcNewWindow = (RECT*)lParam;
            SetWindowPos(hWnd,
                NULL,
                prcNewWindow->left,
                prcNewWindow->top,
                prcNewWindow->right - prcNewWindow->left,
                prcNewWindow->bottom - prcNewWindow->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// --- Entry Point ---

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    // 1. Enable Per-Monitor DPI Awareness V2 so Windows does not stretch the window
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc = {
        sizeof(WNDCLASSEXW), CS_CLASSDC, WndProc, 0L, 0L,
        hInstance, nullptr, nullptr, nullptr, nullptr,
        L"BrightnessSettingsClass", nullptr
    };
    RegisterClassExW(&wc);

    // Initial default dimensions before scaling
    int baseWidth = 480;
    int baseHeight = 260;

    HWND hwnd = CreateWindowW(
        wc.lpszClassName, L"Brightness Settings",
        WS_OVERLAPPEDWINDOW, 100, 100, baseWidth, baseHeight,
        nullptr, nullptr, wc.hInstance, nullptr
    );

    // Calculate real monitor DPI scale (e.g. 1.5 for 150%, 2.0 for 200%)
    UINT dpi = GetDpiForWindow(hwnd);
    float dpiScale = (dpi > 0) ? (static_cast<float>(dpi) / 96.0f) : 1.0f;

    // Resize window according to the active DPI
    SetWindowPos(hwnd, nullptr, 0, 0,
                 static_cast<int>(baseWidth * dpiScale),
                 static_cast<int>(baseHeight * dpiScale),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    RefreshMonitors();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // Stateless window
    ImGui::StyleColorsDark();

    // 2. Load crisp fonts rendered at native high-DPI resolution
    float baseFontSize = 18.0f;
    float scaledFontSize = baseFontSize * dpiScale;

    // Attempt loading Segoe UI or Fallback to default rasterized cleanly
    ImFont* font = nullptr;
    char winDir[MAX_PATH];
    if (GetWindowsDirectoryA(winDir, MAX_PATH)) {
        std::string fontPath = std::string(winDir) + "\\Fonts\\segoeui.ttf";
        font = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), scaledFontSize);
    }
    if (!font) {
        ImFontConfig config;
        config.SizePixels = scaledFontSize;
        io.Fonts->AddFontDefault(&config);
    }

    // 3. Scale UI frames, margins, and padding accurately to DPI
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(dpiScale);
    style.WindowRounding = 0.0f;
    style.FrameRounding = 4.0f * dpiScale;
    style.GrabRounding = 4.0f * dpiScale;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                running = false;
        }
        if (!running) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Dock UI canvas to window bounds
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Content", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        ImGui::Text("Monitors");
        ImGui::Spacing();

        if (g_Monitors.empty()) {
            ImGui::TextDisabled("No DDC/CI compatible monitors detected.");
            if (ImGui::Button("Rescan")) {
                RefreshMonitors();
            }
        } else if (ImGui::BeginTabBar("MonitorTabs", ImGuiTabBarFlags_None)) {
            for (size_t i = 0; i < g_Monitors.size(); ++i) {
                auto& mon = g_Monitors[i];
                std::string tabTitle = std::to_string(i + 1);

                if (ImGui::BeginTabItem(tabTitle.c_str())) {
                    ImGui::Spacing();

                    if (ImGui::Button("Identify")) {
                        ShowIdentifyOverlay(static_cast<int>(i), mon.rect);
                    }

                    ImGui::Spacing();
                    ImGui::Text("%d%%", mon.sliderValue);

                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderInt("##brightness", &mon.sliderValue, mon.minBrightness, mon.maxBrightness, "");

                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        SetMonitorBrightness(mon.hPhysical, static_cast<DWORD>(mon.sliderValue));
                        mon.currentBrightness = mon.sliderValue;
                    }

                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }

        ImGui::End();

        // Rendering
        ImGui::Render();
        const float clear_color[4] = { 0.11f, 0.11f, 0.11f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    // Cleanup resources
    for (auto& m : g_Monitors) {
        if (m.hPhysical) DestroyPhysicalMonitor(m.hPhysical);
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}