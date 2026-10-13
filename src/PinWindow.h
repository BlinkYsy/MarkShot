#pragma once

#include "ScreenImage.h"

#include <windows.h>

namespace native_shot {

// 创建无边框置顶图。image 会被窗口接管，并按原始像素尺寸 1:1 显示；
// screenPosition 是虚拟桌面坐标。
[[nodiscard]] HWND CreatePinWindow(HINSTANCE instance, ScreenImage image,
                                   POINT screenPosition);

}  // namespace native_shot
