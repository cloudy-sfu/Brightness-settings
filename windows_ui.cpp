#include "windows_ui.h"

#include "app_state.h"
#include "graphics.h"
#include "resource.h"

#include <shellapi.h>

#include <cstring>

#include "imgui.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam);

namespace {

LRESULT CALLBACK OverlayWndProc(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam
) {
    switch (message) {
        case WM_TIMER:
            DestroyWindow(window);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC deviceContext = BeginPaint(window, &paint);
            RECT clientRect;
            GetClientRect(window, &clientRect);

            HBRUSH brush = CreateSolidBrush(RGB(24, 24, 24));
            FillRect(deviceContext, &clientRect, brush);
            DeleteObject(brush);

            SetBkMode(deviceContext, TRANSPARENT);
            SetTextColor(deviceContext, RGB(255, 255, 255));
            HFONT font = CreateFontW(
                72,
                0,
                0,
                0,
                FW_BOLD,
                FALSE,
                FALSE,
                FALSE,
                DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE,
                L"Segoe UI");
            HFONT oldFont = static_cast<HFONT>(
                SelectObject(deviceContext, font));

            wchar_t text[16];
            GetWindowTextW(window, text, 16);
            DrawTextW(
                deviceContext,
                text,
                -1,
                &clientRect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            SelectObject(deviceContext, oldFont);
            DeleteObject(font);
            EndPaint(window, &paint);
            return 0;
        }
        default:
            return DefWindowProcW(window, message, wParam, lParam);
    }
}

void OpenMainWindow(HWND window) {
    ShowWindow(window, SW_SHOW);
    ShowWindow(window, SW_RESTORE);
    SetForegroundWindow(window);
    g_LastGuiPollTime = 0;
}

// GUID_CONSOLE_DISPLAY_STATE
constexpr GUID CONSOLE_DISPLAY_STATE_GUID = {
    0x6FE69556, 0x704A, 0x47A0,
    {0x8F, 0x24, 0xC2, 0x8D, 0x93, 0x6F, 0xDA, 0x47}};

void HandleDisplayStateChange(DWORD state) {
    const bool displaysOff = state == 0;
    if (g_DisplaysOff.exchange(displaysOff) == displaysOff) return;

    if (!displaysOff) g_LastGuiPollTime = 0;
    SetEvent(g_hDisplayStateEvent);
}

const UINT TASKBAR_CREATED_MESSAGE = RegisterWindowMessageW(L"TaskbarCreated");
bool g_TrayIconLight = false;
int g_TrayIconSize = 0;

// Before Windows 10 the value does not exist and the taskbar is dark.
bool IsTaskbarLightTheme() {
    HKEY key;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0,
            KEY_QUERY_VALUE,
            &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD value = 0;
    DWORD type = 0;
    DWORD size = sizeof(value);
    const LONG result = RegQueryValueExW(
        key,
        L"SystemUsesLightTheme",
        nullptr,
        &type,
        reinterpret_cast<BYTE*>(&value),
        &size);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && type == REG_DWORD && value != 0;
}

HICON LoadTrayIcon(bool light, int size) {
    return static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(light ? IDI_TRAY_LIGHT : IDI_TRAY_DARK),
        IMAGE_ICON,
        size,
        size,
        LR_DEFAULTCOLOR));
}

void RefreshTrayIcon(bool force) {
    const bool light = IsTaskbarLightTheme();
    const int size = GetSystemMetrics(SM_CXSMICON);
    if (!force && light == g_TrayIconLight && size == g_TrayIconSize) return;

    HICON icon = LoadTrayIcon(light, size);
    if (!icon) return;

    HICON oldIcon = g_TrayIcon.hIcon;
    g_TrayIcon.hIcon = icon;
    g_TrayIconLight = light;
    g_TrayIconSize = size;
    if (!Shell_NotifyIconW(NIM_MODIFY, &g_TrayIcon)) {
        Shell_NotifyIconW(NIM_ADD, &g_TrayIcon);
    }
    if (oldIcon) DestroyIcon(oldIcon);
}

} // namespace

HPOWERNOTIFY RegisterDisplayStateNotification(HWND window) {
    return RegisterPowerSettingNotification(
        window, &CONSOLE_DISPLAY_STATE_GUID, DEVICE_NOTIFY_WINDOW_HANDLE);
}

void InstallTrayIcon(HWND window) {
    g_TrayIcon.cbSize = sizeof(NOTIFYICONDATAW);
    g_TrayIcon.hWnd = window;
    g_TrayIcon.uID = 1001;
    g_TrayIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_TrayIcon.uCallbackMessage = WM_TRAYICON;
    g_TrayIconLight = IsTaskbarLightTheme();
    g_TrayIconSize = GetSystemMetrics(SM_CXSMICON);
    g_TrayIcon.hIcon = LoadTrayIcon(g_TrayIconLight, g_TrayIconSize);
    if (!g_TrayIcon.hIcon) {
        g_TrayIcon.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    }
    wcscpy_s(g_TrayIcon.szTip, L"Brightness Controller");
    Shell_NotifyIconW(NIM_ADD, &g_TrayIcon);
}

void ShowIdentifyOverlay(int monitorIndex, const RECT& monitorRect) {
    static HWND overlay = nullptr;
    if (overlay && IsWindow(overlay)) {
        DestroyWindow(overlay);
        overlay = nullptr;
    }

    constexpr wchar_t className[] = L"IdentifyOverlayWindow";
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW windowClass = {sizeof(WNDCLASSEXW)};
        windowClass.lpfnWndProc = OverlayWndProc;
        windowClass.hInstance = GetModuleHandle(nullptr);
        windowClass.lpszClassName = className;
        RegisterClassExW(&windowClass);
        registered = true;
    }

    constexpr int boxSize = 180;
    const int x =
        monitorRect.left + (monitorRect.right - monitorRect.left - boxSize) / 2;
    const int y =
        monitorRect.top + (monitorRect.bottom - monitorRect.top - boxSize) / 2;
    overlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        className,
        std::to_wstring(monitorIndex + 1).c_str(),
        WS_POPUP,
        x,
        y,
        boxSize,
        boxSize,
        nullptr,
        nullptr,
        GetModuleHandle(nullptr),
        nullptr);

    SetLayeredWindowAttributes(overlay, 0, 204, LWA_ALPHA);
    ShowWindow(overlay, SW_SHOW);
    UpdateWindow(overlay);
    SetTimer(overlay, 1, 2000, nullptr);
}

LRESULT WINAPI WndProc(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam
) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam)) {
        return true;
    }

    switch (message) {
        case WM_SIZE:
            if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
                CleanupRenderTarget();
                g_pSwapChain->ResizeBuffers(
                    0,
                    static_cast<UINT>(LOWORD(lParam)),
                    static_cast<UINT>(HIWORD(lParam)),
                    DXGI_FORMAT_UNKNOWN,
                    0);
                CreateRenderTarget();
            }
            return 0;
        case WM_DPICHANGED: {
            const auto* newRect = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(
                window,
                nullptr,
                newRect->left,
                newRect->top,
                newRect->right - newRect->left,
                newRect->bottom - newRect->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            RefreshTrayIcon(false);
            return 0;
        }
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
        case WM_DISPLAYCHANGE:
            RefreshTrayIcon(false);
            break;
        case WM_POWERBROADCAST:
            if (wParam == PBT_POWERSETTINGCHANGE) {
                const auto* setting =
                    reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);
                if (setting &&
                    IsEqualGUID(
                        setting->PowerSetting, CONSOLE_DISPLAY_STATE_GUID) &&
                    setting->DataLength >= sizeof(DWORD)) {
                    DWORD state = 0;
                    std::memcpy(&state, setting->Data, sizeof(state));
                    HandleDisplayStateChange(state);
                }
            }
            return TRUE;
        case WM_CLOSE:
            ShowWindow(window, SW_HIDE);
            return 0;
        case WM_TRAYICON:
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                POINT point;
                GetCursorPos(&point);
                HMENU menu = CreatePopupMenu();
                InsertMenuW(
                    menu, 0, MF_BYPOSITION | MF_STRING, IDM_TRAY_OPEN, L"Open");
                InsertMenuW(
                    menu, 1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
                InsertMenuW(
                    menu, 2, MF_BYPOSITION | MF_STRING, IDM_TRAY_CLOSE, L"Close");
                SetForegroundWindow(window);
                TrackPopupMenu(
                    menu,
                    TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                    point.x,
                    point.y,
                    0,
                    window,
                    nullptr);
                DestroyMenu(menu);
            } else if (
                lParam == WM_LBUTTONDBLCLK ||
                lParam == WM_LBUTTONUP) {
                OpenMainWindow(window);
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDM_TRAY_OPEN) {
                OpenMainWindow(window);
            } else if (LOWORD(wParam) == IDM_TRAY_CLOSE) {
                Shell_NotifyIconW(NIM_DELETE, &g_TrayIcon);
                g_AppRunning = false;
                DestroyWindow(window);
            }
            return 0;
        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &g_TrayIcon);
            g_AppRunning = false;
            PostQuitMessage(0);
            return 0;
        default:
            if (TASKBAR_CREATED_MESSAGE != 0 &&
                message == TASKBAR_CREATED_MESSAGE) {
                RefreshTrayIcon(true);
                return 0;
            }
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
