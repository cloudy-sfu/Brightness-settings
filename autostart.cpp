#include "autostart.h"

#include <windows.h>

#include <string>

bool IsAutoStartEnabled() {
    HKEY key;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0,
            KEY_READ,
            &key) != ERROR_SUCCESS) {
        return false;
    }

    wchar_t path[MAX_PATH];
    DWORD size = sizeof(path);
    DWORD type = REG_SZ;
    const LONG result = RegQueryValueExW(
        key,
        L"BrightnessControllerApp",
        nullptr,
        &type,
        reinterpret_cast<LPBYTE>(path),
        &size);
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

void SetAutoStart(bool enable) {
    HKEY key;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0,
            KEY_SET_VALUE,
            &key) != ERROR_SUCCESS) {
        return;
    }

    if (enable) {
        wchar_t executablePath[MAX_PATH];
        if (GetModuleFileNameW(nullptr, executablePath, MAX_PATH) == 0) {
            RegCloseKey(key);
            return;
        }
        const std::wstring command = L"\"" + std::wstring(executablePath) + L"\"";
        RegSetValueExW(
            key,
            L"BrightnessControllerApp",
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"BrightnessControllerApp");
    }

    RegCloseKey(key);
}
