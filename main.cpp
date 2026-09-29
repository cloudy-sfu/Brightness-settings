#include "ambient_sensor.h"
#include "app_state.h"
#include "app_ui.h"
#include "autostart.h"
#include "graphics.h"
#include "monitors.h"
#include "settings.h"
#include "windows_ui.h"

#include <windows.h>

#include <string>
#include <thread>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

namespace {

void ConfigureImGui(float dpiScale) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();

    constexpr float baseFontSize = 18.0f;
    ImFont* font = nullptr;
    char windowsDirectory[MAX_PATH];
    if (GetWindowsDirectoryA(windowsDirectory, MAX_PATH)) {
        const std::string fontPath =
            std::string(windowsDirectory) + "\\Fonts\\segoeui.ttf";
        font = io.Fonts->AddFontFromFileTTF(
            fontPath.c_str(), baseFontSize * dpiScale);
    }
    if (!font) {
        ImFontConfig config;
        config.SizePixels = baseFontSize * dpiScale;
        io.Fonts->AddFontDefault(&config);
    }

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(dpiScale);
    style.WindowRounding = 0.0f;
    style.FrameRounding = 4.0f * dpiScale;
    style.GrabRounding = 4.0f * dpiScale;
}

void RenderFrame() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    RenderApplicationUi();
    ImGui::Render();

    constexpr float clearColor[4] = {0.11f, 0.11f, 0.11f, 1.0f};
    g_pd3dDeviceContext->OMSetRenderTargets(
        1, &g_mainRenderTargetView, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(
        g_mainRenderTargetView, clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_pSwapChain->Present(1, 0);
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    constexpr wchar_t instanceMutexName[] =
        L"Local\\BrightnessSettings.SingleInstance."
        L"{A76A33CC-DBA7-480A-99C4-667497F69C24}";
    HANDLE instanceMutex = CreateMutexW(nullptr, FALSE, instanceMutexName);
    if (!instanceMutex) {
        MessageBoxW(
            nullptr,
            L"Brightness Settings could not check whether another instance "
            L"is running.",
            L"Brightness Settings",
            MB_OK | MB_ICONERROR);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(instanceMutex);
        MessageBoxW(
            nullptr,
            L"Brightness Settings is already running. Use the existing "
            L"instance from the system tray.",
            L"Brightness Settings",
            MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        return 0;
    }

    SetProcessDpiAwarenessContext(
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW windowClass = {
        sizeof(WNDCLASSEXW),
        CS_CLASSDC,
        WndProc,
        0L,
        0L,
        instance,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        L"BrightnessSettingsClass",
        nullptr
    };
    RegisterClassExW(&windowClass);

    constexpr int baseWidth = 550;
    constexpr int baseHeight = 850;
    HWND window = CreateWindowW(
        windowClass.lpszClassName,
        L"Brightness Settings",
        WS_OVERLAPPEDWINDOW,
        100,
        100,
        baseWidth,
        baseHeight,
        nullptr,
        nullptr,
        windowClass.hInstance,
        nullptr);

    const UINT dpi = GetDpiForWindow(window);
    const float dpiScale =
        dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
    SetWindowPos(
        window,
        nullptr,
        0,
        0,
        static_cast<int>(baseWidth * dpiScale),
        static_cast<int>(baseHeight * dpiScale),
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    if (!CreateDeviceD3D(window)) {
        CleanupDeviceD3D();
        UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
        CloseHandle(instanceMutex);
        return 1;
    }

    InstallTrayIcon(window);
    ShowWindow(window, SW_SHOWDEFAULT);
    UpdateWindow(window);
    InitializeSettings();
    SetAutoStart(IsStartAtLoginConfigured());
    RefreshMonitors();

    std::thread sensorThread(SensorPollingWorker);
    ConfigureImGui(dpiScale);
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    while (g_AppRunning) {
        MSG message;
        while (PeekMessage(&message, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessage(&message);
            if (message.message == WM_QUIT) g_AppRunning = false;
        }
        if (!g_AppRunning) break;

        if (!IsWindowVisible(window) || IsIconic(window)) {
            Sleep(50);
            continue;
        }

        PollManualMonitorBrightness();
        RenderFrame();
    }

    g_AppRunning = false;
    SetEvent(g_hShutdownEvent);
    if (sensorThread.joinable()) sensorThread.join();
    CloseHandle(g_hShutdownEvent);
    ReleaseMonitors();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(window);
    UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
    CloseHandle(instanceMutex);
    return 0;
}
