// Ref: https://learn.microsoft.com/en-us/windows/win32/monitor/monitor-configuration
// Ref: https://github.com/ocornut/imgui
// Ref: https://learn.microsoft.com/en-us/windows/win32/hidpi/high-dpi-desktop-application-development-on-windows
// Ref: https://ww1.microchip.com/downloads/en/DeviceDoc/20005292B.pdf
// Ref: https://www.vishay.com/docs/84286/veml7700.pdf
// Ref: https://learn.microsoft.com/en-us/windows/win32/shell/trayicon

#include <windows.h>
#include <shellapi.h>
#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
}

#include <lowlevelmonitorconfigurationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <d3d11.h>
#include <vector>
#include <string>
#include <algorithm>
#include <atomic>
#include <thread>
#include <mutex>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

// Forward declaration from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

constexpr UINT WM_TRAYICON       = WM_USER + 101;
constexpr UINT IDM_TRAY_OPEN     = 2001;
constexpr UINT IDM_TRAY_CLOSE    = 2002;
constexpr float MAX_VEML_LUX     = 65535.0f * 0.0672f; // 4403.95 lx

// A target change smaller than 5 percentage points is ignored.
// Ref: https://learn.microsoft.com/en-us/windows/win32/api/highlevelmonitorconfigurationapi/nf-highlevelmonitorconfigurationapi-setmonitorbrightness
constexpr int HYSTERESIS_THRESHOLD_PCT = 5;
constexpr DWORD READ_INTERVAL_MS = 5000;
// Ref: https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createeventw
static HANDLE g_hShutdownEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

struct CurvePoint {
    float lux;        // X: [0, MAX_VEML_LUX]
    float brightness; // Y: [0, 100]
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

static std::vector<MonitorEntry> g_Monitors;
static std::mutex               g_MonitorsMutex;
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;
static NOTIFYICONDATAW          g_TrayIcon = {};
static std::atomic<bool>        g_AppRunning{ true };
static std::atomic<bool>        g_SensorConnected{ false };
static std::atomic<float>       g_CurrentLux{ 0.0f };
// Ref: https://en.cppreference.com/w/cpp/atomic/atomic
static std::atomic<float>       g_SmoothedLux{ 0.0f };
static DWORD                    g_LastGuiPollTime = 0;

// --- Piecewise Monotonic Interpolation ---

float EvaluateBrightnessCurve(const std::vector<CurvePoint>& points, float lux) {
    if (points.empty()) return 0.0f;
    if (lux <= points.front().lux) return points.front().brightness;
    if (lux >= points.back().lux) return points.back().brightness;

    for (size_t i = 0; i + 1 < points.size(); ++i) {
        if (lux >= points[i].lux && lux <= points[i + 1].lux) {
            float dx = points[i + 1].lux - points[i].lux;
            if (dx <= 1e-4f) return points[i].brightness;
            float t = (lux - points[i].lux) / dx;
            return points[i].brightness + t * (points[i + 1].brightness - points[i].brightness);
        }
    }
    return points.back().brightness;
}

// --- Windows Autostart Registry Management ---

bool IsAutoStartEnabled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t path[MAX_PATH];
        DWORD size = sizeof(path);
        DWORD type = REG_SZ;
        LONG res = RegQueryValueExW(hKey, L"BrightnessControllerApp", nullptr, &type, reinterpret_cast<LPBYTE>(path), &size);
        RegCloseKey(hKey);
        return (res == ERROR_SUCCESS);
    }
    return false;
}

void SetAutoStart(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            RegSetValueExW(hKey, L"BrightnessControllerApp", 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(exePath),
                           static_cast<DWORD>((wcslen(exePath) + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hKey, L"BrightnessControllerApp");
        }
        RegCloseKey(hKey);
    }
}

// --- MCP2221 + VEML7700 USB HID Driver ---

// Ref: https://ww1.microchip.com/downloads/aemDocuments/documents/APID/ProductDocuments/DataSheets/MCP2221A-Data-Sheet-DS20005565D.pdf
// Ref: https://www.vishay.com/docs/84286/veml7700.pdf
class Mcp2221VemlDriver {
private:
    static constexpr DWORD HID_REPORT_SIZE = 65; // Report ID + 64-byte MCP2221 report
    static constexpr BYTE VEML7700_ADDRESS = 0x10;

    HANDLE m_hDev = INVALID_HANDLE_VALUE;

    bool ExchangeReport(const BYTE* outReport, BYTE* inReport) {
        if (!IsOpen()) return false;

        DWORD bytesWritten = 0;
        if (!WriteFile(
                m_hDev,
                outReport,
                HID_REPORT_SIZE,
                &bytesWritten,
                nullptr) ||
            bytesWritten != HID_REPORT_SIZE) {
            Close();
            return false;
        }

        memset(inReport, 0, HID_REPORT_SIZE);

        DWORD bytesRead = 0;
        if (!ReadFile(
                m_hDev,
                inReport,
                HID_REPORT_SIZE,
                &bytesRead,
                nullptr) ||
            bytesRead != HID_REPORT_SIZE) {
            Close();
            return false;
        }

        // Byte 0 is the Windows HID report ID. Byte 1 is the
        // MCP2221 command echo.
        if (inReport[1] != outReport[1]) {
            Close();
            return false;
        }

        return true;
    }

    bool SendI2CCommand(
        BYTE command,
        BYTE address8,
        const BYTE* data,
        uint16_t length
    ) {
        if (!IsOpen() || length > 60) return false;

        // Retry if the MCP2221 I2C engine is temporarily busy.
        for (int retry = 0; retry < 20; ++retry) {
            BYTE report[HID_REPORT_SIZE] = {};
            BYTE response[HID_REPORT_SIZE] = {};

            report[0] = 0x00;                  // HID report ID
            report[1] = command;
            report[2] = static_cast<BYTE>(length & 0xFF);
            report[3] = static_cast<BYTE>((length >> 8) & 0xFF);
            report[4] = address8;

            if (data && length > 0) {
                memcpy(&report[5], data, length);
            }

            if (!ExchangeReport(report, response)) {
                return false;
            }

            // MCP2221 response byte 1:
            // 0x00 = accepted, 0x01 = I2C engine busy.
            if (response[2] == 0x00) {
                return true;
            }

            if (response[2] != 0x01) {
                return false;
            }

            Sleep(2);
        }

        return false;
    }

    bool SetI2CSpeed100kHz() {
        if (!IsOpen()) return false;

        // Divider = (12 MHz / 100 kHz) - 2 = 118.
        BYTE report[HID_REPORT_SIZE] = {};
        BYTE response[HID_REPORT_SIZE] = {};

        report[0] = 0x00;
        report[1] = 0x10; // Status / Set Parameters
        report[4] = 0x20; // Set I2C speed
        report[5] = 118;  // 100 kHz divider

        if (!ExchangeReport(report, response)) {
            return false;
        }

        return response[2] == 0x00 &&
               (response[4] == 0x20 || response[4] == 0x00);
    }

    bool I2CCancel() {
        if (!IsOpen()) return false;

        BYTE report[HID_REPORT_SIZE] = {};
        BYTE response[HID_REPORT_SIZE] = {};

        report[0] = 0x00;
        report[1] = 0x10; // Status / Set Parameters
        report[3] = 0x10; // Cancel current I2C transfer

        if (!ExchangeReport(report, response)) {
            return false;
        }

        Sleep(2);
        return response[2] == 0x00;
    }

    bool I2CWrite(BYTE addr7, const BYTE* data, uint16_t length) {
        return SendI2CCommand(
            0x90,
            static_cast<BYTE>(addr7 << 1),
            data,
            length
        );
    }

    bool I2CWriteNoStop(BYTE addr7, const BYTE* data, uint16_t length) {
        return SendI2CCommand(
            0x94,
            static_cast<BYTE>(addr7 << 1),
            data,
            length
        );
    }

    bool I2CReadRepeatedStart(BYTE addr7, uint16_t length) {
        return SendI2CCommand(
            0x93,
            static_cast<BYTE>((addr7 << 1) | 0x01),
            nullptr,
            length
        );
    }

    bool GetI2CData(BYTE* destination, BYTE requestedLength) {
        if (!IsOpen() || !destination || requestedLength == 0) {
            return false;
        }

        for (int retry = 0; retry < 50; ++retry) {
            BYTE report[HID_REPORT_SIZE] = {};
            BYTE response[HID_REPORT_SIZE] = {};

            report[0] = 0x00;
            report[1] = 0x40; // Get I2C Data

            if (!ExchangeReport(report, response)) {
                return false;
            }

            const BYTE status = response[2];
            const BYTE receivedLength = response[4];

            if (status == 0x00 &&
                receivedLength != 0x7F &&
                receivedLength >= requestedLength) {
                memcpy(destination, &response[5], requestedLength);
                return true;
            }

            // 0x41 or zero bytes normally means the I2C transaction
            // has not completed yet.
            if (status != 0x00 && status != 0x41) {
                return false;
            }

            Sleep(2);
        }

        return false;
    }

public:
    Mcp2221VemlDriver() = default;

    ~Mcp2221VemlDriver() {
        Close();
    }

    void Close() {
        if (m_hDev != INVALID_HANDLE_VALUE) {
            CloseHandle(m_hDev);
            m_hDev = INVALID_HANDLE_VALUE;
        }
    }

    bool IsOpen() const {
        return m_hDev != INVALID_HANDLE_VALUE;
    }

    bool Open() {
        if (IsOpen()) return true;

        GUID hidGuid;
        HidD_GetHidGuid(&hidGuid);

        HDEVINFO hInfo = SetupDiGetClassDevsW(
            &hidGuid,
            nullptr,
            nullptr,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
        );

        if (hInfo == INVALID_HANDLE_VALUE) {
            return false;
        }

        SP_DEVICE_INTERFACE_DATA ifData = {};
        ifData.cbSize = sizeof(ifData);

        for (DWORD i = 0;
             SetupDiEnumDeviceInterfaces(
                 hInfo, nullptr, &hidGuid, i, &ifData);
             ++i) {

            DWORD requiredSize = 0;

            SetupDiGetDeviceInterfaceDetailW(
                hInfo,
                &ifData,
                nullptr,
                0,
                &requiredSize,
                nullptr
            );

            if (requiredSize == 0) {
                continue;
            }

            std::vector<BYTE> detailBuffer(requiredSize);
            auto* detail =
                reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(
                    detailBuffer.data());

            detail->cbSize =
                sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

            if (!SetupDiGetDeviceInterfaceDetailW(
                    hInfo,
                    &ifData,
                    detail,
                    requiredSize,
                    nullptr,
                    nullptr)) {
                continue;
            }

            HANDLE device = CreateFileW(
                detail->DevicePath,
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                0,
                nullptr
            );

            if (device == INVALID_HANDLE_VALUE) {
                continue;
            }

            HIDD_ATTRIBUTES attributes = {};
            attributes.Size = sizeof(attributes);

            const bool isMcp2221 =
                HidD_GetAttributes(device, &attributes) &&
                attributes.VendorID == 0x04D8 &&
                attributes.ProductID == 0x00DD;

            if (!isMcp2221) {
                CloseHandle(device);
                continue;
            }

            m_hDev = device;
            HidD_FlushQueue(m_hDev);

            const bool initialized =
                I2CCancel() &&
                SetI2CSpeed100kHz() &&
                InitVeml7700();

            if (initialized) {
                SetupDiDestroyDeviceInfoList(hInfo);
                return true;
            }

            Close();
        }

        SetupDiDestroyDeviceInfoList(hInfo);
        return false;
    }

    bool InitVeml7700() {
        if (!IsOpen()) return false;

        // Register 0x00:
        // ALS gain = 1, integration time = 100 ms, shutdown = false.
        const BYTE configCommand[3] = {
            0x00, // ALS configuration register
            0x00, // Low byte
            0x00  // High byte
        };

        if (!I2CWrite(
                VEML7700_ADDRESS,
                configCommand,
                sizeof(configCommand))) {
            return false;
        }

        // Allow the first 100 ms integration cycle to complete.
        Sleep(120);
        return true;
    }

    bool ReadLux(float& outLux) {
        if (!IsOpen() && !Open()) {
            return false;
        }

        // Set the VEML7700 register pointer without a STOP, then
        // issue a repeated-START read.
        const BYTE alsRegister = 0x04;

        if (!I2CWriteNoStop(
                VEML7700_ADDRESS,
                &alsRegister,
                1)) {
            I2CCancel();
            Close();
            return false;
        }

        if (!I2CReadRepeatedStart(VEML7700_ADDRESS, 2)) {
            I2CCancel();
            Close();
            return false;
        }

        BYTE data[2] = {};
        if (!GetI2CData(data, sizeof(data))) {
            I2CCancel();
            Close();
            return false;
        }

        const uint16_t rawCount =
            static_cast<uint16_t>(data[0]) |
            (static_cast<uint16_t>(data[1]) << 8);

        // Matches gain 1 and 100 ms integration time used by test_lux.py.
        outLux = static_cast<float>(rawCount) * 0.0672f;
        return true;
    }
};

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
                MonitorEntry entry{};
                entry.hMonitor = hMonitor;
                entry.rect = *lprcMonitor;
                entry.hPhysical = physList[i].hPhysicalMonitor;
                entry.hasPhysicalHandle = true;

                char desc[128] = { 0 };
                WideCharToMultiByte(CP_UTF8, 0, physList[i].szPhysicalMonitorDescription, -1, desc, sizeof(desc), NULL, NULL);
                entry.name = (desc[0] != '\0') ? desc : ("Display " + std::to_string(g_Monitors.size() + 1));
                entry.minBrightness = static_cast<int>(minB);
                entry.maxBrightness = static_cast<int>(maxB);
                entry.currentBrightness = static_cast<int>(curB);
                entry.sliderValue = static_cast<int>(curB);
                // Ref: https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-gettickcount
                entry.autoBrightness = false;
                entry.autoBrightness = false;
                entry.lastBrightnessReadTick = 0;
                entry.lastBrightnessReadTick = 0;

                // Default linear curve: (0 lx -> 0%), (MAX_LUX -> 100%)
                entry.curve.push_back({ 0.0f, 0.0f });
                entry.curve.push_back({ MAX_VEML_LUX, 100.0f });

                g_Monitors.push_back(entry);
            } else {
                DestroyPhysicalMonitor(physList[i].hPhysicalMonitor);
            }
        }
    }
    return TRUE;
}

void RefreshMonitors() {
    std::lock_guard<std::mutex> lock(g_MonitorsMutex);
    for (auto& m : g_Monitors) {
        if (m.hasPhysicalHandle) {
            DestroyPhysicalMonitor(m.hPhysical);
            m.hasPhysicalHandle = false;
        }
    }
    g_Monitors.clear();
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, 0);
}

// --- Background Sensor & Auto-Brightness Thread ---

// Ref: https://learn.microsoft.com/en-us/windows/win32/api/highlevelmonitorconfigurationapi/nf-highlevelmonitorconfigurationapi-getmonitorbrightness
// Ref: https://learn.microsoft.com/en-us/windows/win32/api/highlevelmonitorconfigurationapi/nf-highlevelmonitorconfigurationapi-setmonitorbrightness
void SensorPollingWorker() {
    Mcp2221VemlDriver driver;

    while (g_AppRunning.load()) {
        float rawLux = 0.0f;

        if (driver.ReadLux(rawLux)) {
            g_SensorConnected.store(
                true,
                std::memory_order_relaxed
            );

            g_CurrentLux.store(
                rawLux,
                std::memory_order_relaxed
            );

            /*
             * No EMA is required. The 5-percentage-point brightness
             * deadband already rejects insignificant sensor variation.
             *
             * Keeping this value synchronized with rawLux avoids a long
             * delay when the room suddenly becomes dark.
             */
            g_SmoothedLux.store(
                rawLux,
                std::memory_order_relaxed
            );

            const DWORD now = GetTickCount();

            std::lock_guard<std::mutex> lock(
                g_MonitorsMutex
            );

            for (auto& mon : g_Monitors) {
                if (!mon.autoBrightness || !mon.hasPhysicalHandle) {
                    continue;
                }

                /*
                 * Periodically read the actual monitor brightness so that
                 * changes made through the monitor OSD or another program
                 * are detected.
                 */
                if (now - mon.lastBrightnessReadTick >= READ_INTERVAL_MS) {

                    DWORD minimum = 0;
                    DWORD current = 0;
                    DWORD maximum = 100;

                    if (GetMonitorBrightness(
                            mon.hPhysical,
                            &minimum,
                            &current,
                            &maximum)) {

                        mon.minBrightness =
                            static_cast<int>(minimum);

                        mon.maxBrightness =
                            static_cast<int>(maximum);

                        mon.currentBrightness =
                            static_cast<int>(current);

                        mon.sliderValue =
                            mon.currentBrightness;
                    }

                    mon.lastBrightnessReadTick = now;
                }

                const float targetFloat =
                    EvaluateBrightnessCurve(
                        mon.curve,
                        rawLux
                    );

                int target =
                    static_cast<int>(
                        std::round(targetFloat)
                    );

                target = (std::max)(
                    mon.minBrightness,
                    (std::min)(
                        mon.maxBrightness,
                        target
                    )
                );

                const int difference =
                    std::abs(
                        target -
                        mon.currentBrightness
                    );

                /*
                 * Do not send any brightness command unless the required
                 * change is at least 5 percentage points.
                 */
                if (difference <
                    HYSTERESIS_THRESHOLD_PCT) {
                    continue;
                }

                if (SetMonitorBrightness(
                        mon.hPhysical,
                        static_cast<DWORD>(target))) {

                    mon.currentBrightness = target;
                    mon.sliderValue = target;

                    /*
                     * The successful write confirms the cached value, so an
                     * immediate readback is unnecessary.
                     */
                    mon.lastBrightnessReadTick = now;
                }
            }
        } else {
            g_SensorConnected.store(
                false,
                std::memory_order_relaxed
            );
        }

        if (WaitForSingleObject(g_hShutdownEvent, READ_INTERVAL_MS) != WAIT_TIMEOUT) {
            break; // Shutdown signaled, exit thread immediately
        }
    }
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
            SetWindowPos(hWnd, NULL,
                prcNewWindow->left, prcNewWindow->top,
                prcNewWindow->right - prcNewWindow->left,
                prcNewWindow->bottom - prcNewWindow->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_CLOSE:
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        case WM_TRAYICON:
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                POINT pt;
                GetCursorPos(&pt);
                HMENU hMenu = CreatePopupMenu();
                InsertMenuW(hMenu, 0, MF_BYPOSITION | MF_STRING, IDM_TRAY_OPEN, L"Open");
                InsertMenuW(hMenu, 1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
                InsertMenuW(hMenu, 2, MF_BYPOSITION | MF_STRING, IDM_TRAY_CLOSE, L"Close");

                SetForegroundWindow(hWnd);
                TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, nullptr);
                DestroyMenu(hMenu);
            } else if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP) {
                ShowWindow(hWnd, SW_SHOW);
                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd);
                g_LastGuiPollTime = 0;
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDM_TRAY_OPEN) {
                ShowWindow(hWnd, SW_SHOW);
                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd);
                g_LastGuiPollTime = 0;
            } else if (LOWORD(wParam) == IDM_TRAY_CLOSE) {
                Shell_NotifyIconW(NIM_DELETE, &g_TrayIcon);
                g_AppRunning = false;
                DestroyWindow(hWnd);
            }
            return 0;
        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &g_TrayIcon);
            g_AppRunning = false;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// --- Editable Monotonic Piecewise Line Plot UI ---

// Ref: https://github.com/ocornut/imgui
void AddTextRotated90CounterClockwise(
    ImDrawList* drawList,
    const ImVec2& targetCenter,
    ImU32 color,
    const char* text
) {
    if (!drawList || !text || text[0] == '\0') {
        return;
    }

    const int firstVertex = drawList->VtxBuffer.Size;

    // Generate the text vertices at a temporary position.
    drawList->AddText(
        ImVec2(0.0f, 0.0f),
        color,
        text
    );

    const int finalVertex = drawList->VtxBuffer.Size;

    if (finalVertex <= firstVertex) {
        return;
    }

    ImVec2 minimum =
        drawList->VtxBuffer[firstVertex].pos;

    ImVec2 maximum = minimum;

    for (int i = firstVertex;
         i < finalVertex;
         ++i) {

        const ImVec2 position =
            drawList->VtxBuffer[i].pos;

        minimum.x = (std::min)(minimum.x, position.x);
        minimum.y = (std::min)(minimum.y, position.y);
        maximum.x = (std::max)(maximum.x, position.x);
        maximum.y = (std::max)(maximum.y, position.y);
         }

    const ImVec2 sourceCenter(
        (minimum.x + maximum.x) * 0.5f,
        (minimum.y + maximum.y) * 0.5f
    );

    for (int i = firstVertex;
         i < finalVertex;
         ++i) {

        ImDrawVert& vertex =
            drawList->VtxBuffer[i];

        const float relativeX =
            vertex.pos.x - sourceCenter.x;

        const float relativeY =
            vertex.pos.y - sourceCenter.y;

        // Screen coordinates use a downward-positive Y axis.
        vertex.pos.x =
            targetCenter.x + relativeY;

        vertex.pos.y =
            targetCenter.y - relativeX;
         }
}

// Ref: https://github.com/ocornut/imgui
void RenderCurveEditor(
    const char* id,
    std::vector<CurvePoint>& points,
    float currentLux
) {
    if (points.size() < 2) {
        points.clear();
        points.push_back({ 0.0f, 0.0f });
        points.push_back({ MAX_VEML_LUX, 100.0f });
    }

    // Endpoints have fixed screen brightness values but movable lux values.
    points.front().brightness = 0.0f;
    points.back().brightness = 100.0f;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 originPos = ImGui::GetCursorScreenPos();

    const float topMargin = 12.0f;
    const float yMargin = 70.0f;
    const float xMargin = 58.0f;

    // Previous maximum was 260. 572 is exactly 2.2 times larger.
    constexpr float requestedSide = 572.0f;

    const float availableWidth = ImGui::GetContentRegionAvail().x;
    float side = (std::min)(
        requestedSide,
        availableWidth - yMargin - 20.0f
    );

    if (side < 180.0f) {
        side = 180.0f;
    }

    const ImVec2 canvasPos(
        originPos.x + yMargin,
        originPos.y + topMargin
    );

    const ImVec2 canvasEnd(
        canvasPos.x + side,
        canvasPos.y + side
    );

    const ImVec2 totalSize(
        yMargin + side + 12.0f,
        topMargin + side + xMargin
    );

    ImGui::InvisibleButton(id, totalSize);

    drawList->AddRectFilled(
        canvasPos,
        canvasEnd,
        IM_COL32(30, 30, 34, 255),
        4.0f
    );

    drawList->AddRect(
        canvasPos,
        canvasEnd,
        IM_COL32(70, 70, 78, 255),
        4.0f
    );

    auto ToScreen = [&](const CurvePoint& point) -> ImVec2 {
        const float normalizedX = (std::max)(
            0.0f,
            (std::min)(1.0f, point.lux / MAX_VEML_LUX)
        );

        const float normalizedY = (std::max)(
            0.0f,
            (std::min)(1.0f, point.brightness / 100.0f)
        );

        return ImVec2(
            canvasPos.x + normalizedX * side,
            canvasEnd.y - normalizedY * side
        );
    };

    // Y-axis grid and values.
    for (int brightness = 0;
         brightness <= 100;
         brightness += 25) {

        const float y =
            canvasEnd.y -
            (brightness / 100.0f) * side;

        if (brightness > 0 && brightness < 100) {
            drawList->AddLine(
                ImVec2(canvasPos.x, y),
                ImVec2(canvasEnd.x, y),
                IM_COL32(50, 50, 56, 180)
            );
        }

        char valueText[16];
        snprintf(
            valueText,
            sizeof(valueText),
            "%d",
            brightness
        );

        const ImVec2 textSize =
            ImGui::CalcTextSize(valueText);

        drawList->AddText(
            ImVec2(
                canvasPos.x - textSize.x - 6.0f,
                y - textSize.y * 0.5f
            ),
            IM_COL32(160, 160, 170, 255),
            valueText
        );
    }

    // X-axis grid and dynamically calculated lux values.
    for (int tick = 0; tick <= 4; ++tick) {
        const float normalized = tick * 0.25f;
        const float x =
            canvasPos.x + normalized * side;

        if (tick > 0 && tick < 4) {
            drawList->AddLine(
                ImVec2(x, canvasPos.y),
                ImVec2(x, canvasEnd.y),
                IM_COL32(50, 50, 56, 180)
            );
        }

        char valueText[24];
        snprintf(
            valueText,
            sizeof(valueText),
            "%.0f",
            MAX_VEML_LUX * normalized
        );

        const ImVec2 textSize =
            ImGui::CalcTextSize(valueText);

        drawList->AddText(
            ImVec2(
                x - textSize.x * 0.5f,
                canvasEnd.y + 5.0f
            ),
            IM_COL32(160, 160, 170, 255),
            valueText
        );
    }

    // X-axis label.
    const char* xAxisLabel = "Environment";
    const ImVec2 xLabelSize =
        ImGui::CalcTextSize(xAxisLabel);

    drawList->AddText(
        ImVec2(
            canvasPos.x +
                (side - xLabelSize.x) * 0.5f,
            canvasEnd.y + 30.0f
        ),
        IM_COL32(210, 210, 220, 255),
        xAxisLabel
    );

    // Ref: https://github.com/ocornut/imgui
    // Y-axis label rotated 90 degrees counter-clockwise.
    AddTextRotated90CounterClockwise(
        drawList,
        ImVec2(
            originPos.x + 14.0f,
            canvasPos.y + side * 0.5f
        ),
        IM_COL32(210, 210, 220, 255),
        "Screen"
    );

    // Draw the constant region before the first endpoint.
    const ImVec2 firstPointPosition =
        ToScreen(points.front());

    drawList->AddLine(
        ImVec2(canvasPos.x, firstPointPosition.y),
        firstPointPosition,
        IM_COL32(50, 160, 255, 255),
        2.0f
    );

    // Draw the editable response-curve segments.
    for (size_t i = 0;
         i + 1 < points.size();
         ++i) {

        drawList->AddLine(
            ToScreen(points[i]),
            ToScreen(points[i + 1]),
            IM_COL32(50, 160, 255, 255),
            2.0f
        );
    }

    // Draw the constant 100% region after the final endpoint.
    const ImVec2 finalPointPosition =
        ToScreen(points.back());

    drawList->AddLine(
        finalPointPosition,
        ImVec2(canvasEnd.x, finalPointPosition.y),
        IM_COL32(50, 160, 255, 255),
        2.0f
    );

    // Current sensor position.
    if (g_SensorConnected) {
        const float displayedLux =
            (std::max)(
                0.0f,
                (std::min)(currentLux, MAX_VEML_LUX)
            );

        const float currentX =
            canvasPos.x +
            (displayedLux / MAX_VEML_LUX) *
            side;

        drawList->AddLine(
            ImVec2(currentX, canvasPos.y),
            ImVec2(currentX, canvasEnd.y),
            IM_COL32(255, 180, 0, 180),
            1.5f
        );

        const float evaluatedBrightness =
            EvaluateBrightnessCurve(
                points,
                currentLux
            );

        const float currentY =
            canvasEnd.y -
            (evaluatedBrightness / 100.0f) *
            side;

        drawList->AddCircleFilled(
            ImVec2(currentX, currentY),
            5.0f,
            IM_COL32(255, 200, 0, 255)
        );
    }

    static int draggingIndex = -1;
    static std::string activeEditorId;

    ImGuiIO& io = ImGui::GetIO();
    int hoveredIndex = -1;

    // Existing control points.
    for (size_t i = 0; i < points.size(); ++i) {
        const ImVec2 pointPosition =
            ToScreen(points[i]);

        const bool isEndpoint =
            i == 0 ||
            i == points.size() - 1;

        ImU32 pointColor = isEndpoint
            ? IM_COL32(200, 200, 200, 255)
            : IM_COL32(0, 220, 120, 255);

        const float distance = std::hypot(
            io.MousePos.x - pointPosition.x,
            io.MousePos.y - pointPosition.y
        );

        const bool hovered = distance <= 9.0f;

        if (hovered) {
            hoveredIndex = static_cast<int>(i);
            pointColor =
                IM_COL32(255, 255, 0, 255);

            if (!isEndpoint &&
                ImGui::IsMouseClicked(
                    ImGuiMouseButton_Right)) {

                points.erase(points.begin() + i);
                hoveredIndex = -1;
                break;
            }

            // Endpoints can now also be dragged.
            if (ImGui::IsMouseClicked(
                    ImGuiMouseButton_Left)) {
                draggingIndex =
                    static_cast<int>(i);
                activeEditorId = id;
            }
        }

        drawList->AddCircleFilled(
            pointPosition,
            isEndpoint ? 6.0f : 5.0f,
            pointColor
        );
    }

    const bool mouseInsidePlot =
        io.MousePos.x >= canvasPos.x &&
        io.MousePos.x <= canvasEnd.x &&
        io.MousePos.y >= canvasPos.y &&
        io.MousePos.y <= canvasEnd.y;

    // Add a new interior point.
    if (mouseInsidePlot &&
        hoveredIndex == -1 &&
        ImGui::IsMouseClicked(
            ImGuiMouseButton_Left)) {

        const float normalizedX =
            (io.MousePos.x - canvasPos.x) /
            side;

        const float normalizedY =
            (canvasEnd.y - io.MousePos.y) /
            side;

        const float clickedLux =
            normalizedX * MAX_VEML_LUX;

        const float clickedBrightness =
            normalizedY * 100.0f;

        for (size_t i = 0;
             i + 1 < points.size();
             ++i) {

            if (clickedLux >= points[i].lux &&
                clickedLux <= points[i + 1].lux) {

                if (points[i + 1].lux -
                        points[i].lux >
                    5.0f) {

                    const float validLux =
                        (std::max)(
                            points[i].lux + 1.0f,
                            (std::min)(
                                points[i + 1].lux - 1.0f,
                                clickedLux
                            )
                        );

                    const float validBrightness =
                        (std::max)(
                            points[i].brightness,
                            (std::min)(
                                points[i + 1].brightness,
                                clickedBrightness
                            )
                        );

                    points.insert(
                        points.begin() + i + 1,
                        {
                            validLux,
                            validBrightness
                        }
                    );

                    draggingIndex =
                        static_cast<int>(i + 1);

                    activeEditorId = id;
                }

                break;
            }
        }
    }

    // Update the dragged point.
    if (draggingIndex >= 0 &&
        activeEditorId == id) {

        if (ImGui::IsMouseDown(
                ImGuiMouseButton_Left)) {

            const size_t index =
                static_cast<size_t>(
                    draggingIndex);

            if (index < points.size()) {
                const float normalizedX =
                    (io.MousePos.x -
                     canvasPos.x) /
                    side;

                const float normalizedY =
                    (canvasEnd.y -
                     io.MousePos.y) /
                    side;

                float newLux =
                    normalizedX *
                    MAX_VEML_LUX;

                float newBrightness =
                    normalizedY *
                    100.0f;

                if (index == 0) {
                    // First endpoint moves only horizontally.
                    newLux = (std::max)(
                        0.0f,
                        (std::min)(
                            points[1].lux - 0.5f,
                            newLux
                        )
                    );

                    points[index].lux = newLux;
                    points[index].brightness = 0.0f;
                } else if (
                    index == points.size() - 1) {

                    // Final endpoint moves only horizontally.
                    newLux = (std::max)(
                        points[index - 1].lux + 0.5f,
                        (std::min)(
                            MAX_VEML_LUX,
                            newLux
                        )
                    );

                    points[index].lux = newLux;
                    points[index].brightness = 100.0f;
                } else {
                    // Interior points move in both dimensions while
                    // preserving a monotonic response curve.
                    newLux = (std::max)(
                        points[index - 1].lux + 0.5f,
                        (std::min)(
                            points[index + 1].lux - 0.5f,
                            newLux
                        )
                    );

                    newBrightness = (std::max)(
                        points[index - 1].brightness,
                        (std::min)(
                            points[index + 1].brightness,
                            newBrightness
                        )
                    );

                    points[index].lux = newLux;
                    points[index].brightness =
                        newBrightness;
                }
            }
        } else {
            draggingIndex = -1;
            activeEditorId.clear();
        }
    }

    ImGui::Spacing();
    ImGui::TextWrapped(
        "Left click: add a point. Drag: move a point. "
        "Gray endpoints move horizontally. "
        "Right click: delete an interior point."
    );

    const std::string resetButtonId =
        "Reset Linear##" +
        std::string(id);

    if (ImGui::Button(
            resetButtonId.c_str())) {
        points.clear();
        points.push_back({
            0.0f,
            0.0f
        });
        points.push_back({
            MAX_VEML_LUX,
            100.0f
        });
    }
}

// --- Entry Point ---

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc = {
        sizeof(WNDCLASSEXW), CS_CLASSDC, WndProc, 0L, 0L,
        hInstance, nullptr, nullptr, nullptr, nullptr,
        L"BrightnessSettingsClass", nullptr
    };
    RegisterClassExW(&wc);

    // Initial default dimensions before scaling
    int baseWidth = 550;
    int baseHeight = 850;

    HWND hwnd = CreateWindowW(
        wc.lpszClassName, L"Brightness Settings",
        WS_OVERLAPPEDWINDOW, 100, 100, baseWidth, baseHeight,
        nullptr, nullptr, wc.hInstance, nullptr
    );

    UINT dpi = GetDpiForWindow(hwnd);
    float dpiScale = (dpi > 0) ? (static_cast<float>(dpi) / 96.0f) : 1.0f;

    SetWindowPos(hwnd, nullptr, 0, 0,
                 static_cast<int>(baseWidth * dpiScale),
                 static_cast<int>(baseHeight * dpiScale),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    g_TrayIcon.cbSize = sizeof(NOTIFYICONDATAW);
    g_TrayIcon.hWnd = hwnd;
    g_TrayIcon.uID = 1001;
    g_TrayIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_TrayIcon.uCallbackMessage = WM_TRAYICON;
    g_TrayIcon.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcscpy_s(g_TrayIcon.szTip, L"Brightness Controller");
    Shell_NotifyIconW(NIM_ADD, &g_TrayIcon);

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    RefreshMonitors();

    std::thread sensorThread(SensorPollingWorker);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();

    float baseFontSize = 18.0f;
    float scaledFontSize = baseFontSize * dpiScale;

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

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(dpiScale);
    style.WindowRounding = 0.0f;
    style.FrameRounding = 4.0f * dpiScale;
    style.GrabRounding = 4.0f * dpiScale;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    while (g_AppRunning) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                g_AppRunning = false;
        }
        if (!g_AppRunning) break;

        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
            Sleep(50);
            continue;
        }

        // Live monitor polling: only when window is active and monitor is in manual mode
        DWORD now = GetTickCount();
        if (now - g_LastGuiPollTime >= READ_INTERVAL_MS) {
            g_LastGuiPollTime = now;
            std::lock_guard<std::mutex> lock(g_MonitorsMutex);
            for (auto& mon : g_Monitors) {
                if (!mon.autoBrightness && mon.hasPhysicalHandle) {
                    DWORD minB = 0, curB = 0, maxB = 100;
                    if (GetMonitorBrightness(mon.hPhysical, &minB, &curB, &maxB)) {
                        mon.currentBrightness = static_cast<int>(curB);
                        mon.sliderValue = static_cast<int>(curB);
                    }
                }
            }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Content", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        // Global Checkboxes above "Monitors"
        bool autoStart = IsAutoStartEnabled();
        if (ImGui::Checkbox("Start when system login", &autoStart)) {
            SetAutoStart(autoStart);
        }

        // Read the atomic sensor values once for this GUI frame.
        // Ref: https://en.cppreference.com/w/cpp/atomic/atomic/load
        const float displayedRawLux =
            g_CurrentLux.load(std::memory_order_relaxed);

        const float displayedSmoothedLux =
            g_SmoothedLux.load(std::memory_order_relaxed);

        if (g_SensorConnected.load(std::memory_order_relaxed)) {
            ImGui::TextColored(
                ImVec4(0.2f, 0.9f, 0.3f, 1.0f),
                "VEML7700: Connected (Raw: %.1f lx, Filtered: %.1f lx)",
                displayedRawLux,
                displayedSmoothedLux
            );
        } else {
            ImGui::TextDisabled(
                "VEML7700: Not detected (Waiting for device...)"
            );
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Monitors");
        ImGui::Spacing();

        std::lock_guard<std::mutex> lock(g_MonitorsMutex);
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
                    ImGui::Text("Brightness: %d%%", mon.sliderValue);

                    if (mon.autoBrightness) ImGui::BeginDisabled();
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderInt("##brightness", &mon.sliderValue, mon.minBrightness, mon.maxBrightness, "");
                    // Ref: https://learn.microsoft.com/en-us/windows/win32/api/highlevelmonitorconfigurationapi/nf-highlevelmonitorconfigurationapi-setmonitorbrightness
                    if (!mon.autoBrightness &&
                        ImGui::IsItemDeactivatedAfterEdit()) {

                        if (SetMonitorBrightness(
                                mon.hPhysical,
                                static_cast<DWORD>(mon.sliderValue))) {

                            mon.currentBrightness = mon.sliderValue;
                                } else {
                                    // Restore the last confirmed value if the DDC/CI write failed.
                                    mon.sliderValue = mon.currentBrightness;
                                }
                    }
                    if (mon.autoBrightness) ImGui::EndDisabled();

                    ImGui::Spacing();
                    // Ref: https://github.com/ocornut/imgui
                    std::string chkLabel =
                        "Automatically adjust based on environmental brightness##" +
                        std::to_string(i);

                    if (ImGui::Checkbox( chkLabel.c_str(), &mon.autoBrightness)) {
                        if (mon.autoBrightness) {
                            mon.lastBrightnessReadTick = 0;
                        }
                    }

                    ImGui::Spacing();
                    ImGui::Text("Response Curve");
                    std::string curveId = "CurveEditor##" + std::to_string(i);
                    RenderCurveEditor(curveId.c_str(), mon.curve, displayedSmoothedLux);

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

    // Termination & Cleanup
    g_AppRunning = false;
    SetEvent(g_hShutdownEvent);
    if (sensorThread.joinable()) {
        sensorThread.join();
    }
    CloseHandle(g_hShutdownEvent);
    for (auto& m : g_Monitors) {
        if (m.hasPhysicalHandle) {
            DestroyPhysicalMonitor(m.hPhysical);
            m.hasPhysicalHandle = false;
        }
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}