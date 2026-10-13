#include "PinWindow.h"

#include <memory>
#include <utility>

namespace native_shot {
namespace {

constexpr wchar_t kPinClassName[] = L"MarkShot.PinWindow";

// 置顶窗口独占一份合成后的像素，生命周期与窗口完全一致。
struct PinState {
    ScreenImage image;
};

struct PinCreation {
    PinState* state = nullptr;
    bool adopted = false;
};

void PaintPin(HWND window, const PinState& state) {
    PAINTSTRUCT paint{};
    HDC dc = ::BeginPaint(window, &paint);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = state.image.width;
    info.bmiHeader.biHeight = -state.image.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    // 窗口尺寸与截图像素尺寸一致，直接逐像素显示，不做任何缩放。
    ::SetDIBitsToDevice(dc, 0, 0, state.image.width, state.image.height,
                        0, 0, 0, state.image.height,
                        state.image.pixels.data(), &info, DIB_RGB_COLORS);
    ::EndPaint(window, &paint);
}

LRESULT CALLBACK PinProcedure(HWND window, UINT message,
                              WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<PinState*>(
        ::GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
        case WM_NCCREATE: {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto* payload = static_cast<PinCreation*>(create->lpCreateParams);
            ::SetWindowLongPtrW(window, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(payload->state));
            payload->adopted = true;
            return TRUE;
        }
        case WM_PAINT:
            if (state) PaintPin(window, *state);
            return 0;
        case WM_NCHITTEST:
            return HTCAPTION;  // 置顶图任意位置都可以拖动。
        case WM_RBUTTONUP:
            ::DestroyWindow(window);
            return 0;
        case WM_LBUTTONDBLCLK:
        case WM_NCLBUTTONDBLCLK:
            // HTCAPTION 会把双击转换成非客户区消息，因此两种消息都处理。
            ::DestroyWindow(window);
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                ::DestroyWindow(window);
                return 0;
            }
            if ((::GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'C' && state) {
                (void)CopyToClipboard(window, state->image);
                return 0;
            }
            break;
        case WM_NCDESTROY:
            delete state;
            ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

bool RegisterPinClass(HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_DBLCLKS;
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = PinProcedure;
    windowClass.lpszClassName = kPinClassName;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_SIZEALL);
    windowClass.hbrBackground = static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
    return ::RegisterClassExW(&windowClass) != 0 ||
           ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

}  // namespace

HWND CreatePinWindow(HINSTANCE instance, ScreenImage image,
                     POINT screenPosition) {
    if (image.Empty() || !RegisterPinClass(instance)) {
        return nullptr;
    }
    auto state = std::make_unique<PinState>();
    state->image = std::move(image);
    PinCreation payload{state.get(), false};
    const int width = state->image.width;
    const int height = state->image.height;

    HWND window = ::CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kPinClassName, L"MarkShot Pin", WS_POPUP | WS_VISIBLE,
        screenPosition.x, screenPosition.y, width, height,
        nullptr, nullptr, instance, &payload);
    // WM_NCCREATE 成功后状态交给窗口过程，并在 WM_NCDESTROY 中释放。
    if (payload.adopted) {
        state.release();
    }
    if (!window) {
        return nullptr;
    }
    ::SetForegroundWindow(window);
    return window;
}

}  // namespace native_shot
