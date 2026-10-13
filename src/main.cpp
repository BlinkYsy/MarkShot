#include "TrayApplication.h"

#include <windows.h>
#include <objbase.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // Per-monitor V2 保证多显示器不同缩放比例下，窗口坐标仍使用真实像素。
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT comResult = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult)) {
        return 1;
    }

    const int exitCode = native_shot::RunTrayApplication(instance);
    ::CoUninitialize();
    return exitCode;
}
