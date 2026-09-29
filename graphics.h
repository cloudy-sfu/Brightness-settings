#pragma once

#include <windows.h>

bool CreateDeviceD3D(HWND window);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
