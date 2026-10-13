#pragma once

#include <windows.h>

namespace native_shot {

// 运行隐藏的托盘宿主。宿主负责单实例、全局快捷键、托盘菜单和消息循环，
// 直到用户从菜单退出应用。
[[nodiscard]] int RunTrayApplication(HINSTANCE instance);

}  // namespace native_shot
