#pragma once

#include <windows.h>

LRESULT WINAPI WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
void InstallTrayIcon(HWND window);
void ShowIdentifyOverlay(int monitorIndex, const RECT& monitorRect);
