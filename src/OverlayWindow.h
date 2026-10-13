#pragma once

#include <windows.h>

namespace native_shot {

// 抓取当前虚拟桌面并创建全屏截图层。返回值由 Win32 窗口系统管理，
// 关闭窗口时内部状态会在 WM_NCDESTROY 中释放。
[[nodiscard]] HWND CreateOverlayWindow(HINSTANCE instance);

}  // namespace native_shot
