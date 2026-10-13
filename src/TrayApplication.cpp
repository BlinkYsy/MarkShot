#include "TrayApplication.h"

#include "OverlayWindow.h"
#include "resource.h"

#include <shellapi.h>

#include <memory>

namespace native_shot {
namespace {

constexpr wchar_t kTrayWindowClass[] = L"MarkShot.TrayHostWindow";
constexpr wchar_t kSingleInstanceName[] = L"Local\\MarkShot.SingleInstance";
constexpr UINT kTrayIconId = 1;
constexpr int kHotkeyId = 1;
constexpr UINT kTrayCallback = WM_APP + 1;
constexpr UINT kStartCapture = WM_APP + 2;
constexpr UINT kExitApplication = WM_APP + 3;
constexpr UINT kCaptureCommand = 1001;
constexpr UINT kExitCommand = 1002;

HICON LoadIconResource(HINSTANCE instance, int resourceId,
                       int width, int height) {
    // LR_SHARED 让系统缓存资源图标，调用方不需要手动 DestroyIcon。
    HICON icon = reinterpret_cast<HICON>(::LoadImageW(
        instance, MAKEINTRESOURCEW(resourceId), IMAGE_ICON,
        width, height, LR_DEFAULTCOLOR | LR_SHARED));
    return icon ? icon : ::LoadIconW(nullptr, IDI_APPLICATION);
}

// 隐藏宿主窗口集中持有托盘图标和全局快捷键；截图层可以独立创建和销毁。
class TrayState {
public:
    explicit TrayState(HINSTANCE instance) : m_instance(instance) {}

    bool Initialize(HWND window) {
        m_window = window;
        m_taskbarCreated = ::RegisterWindowMessageW(L"TaskbarCreated");
        if (!AddTrayIcon()) {
            return false;
        }

        m_hotkeyRegistered = ::RegisterHotKey(
            window, kHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'A') != FALSE;
        if (!m_hotkeyRegistered) {
            NotifyHotkeyConflict();
        }
        return true;
    }

    void StartCapture() const {
        // 已有截图层时只激活它，避免重复创建全屏窗口。
        if (HWND overlay = ::FindWindowW(L"MarkShot.OverlayWindow", nullptr)) {
            ::ShowWindow(overlay, SW_SHOW);
            ::SetForegroundWindow(overlay);
            return;
        }
        if (!CreateOverlayWindow(m_instance)) {
            ::MessageBoxW(m_window, L"无法启动截图窗口。", L"MarkShot",
                          MB_OK | MB_ICONERROR);
        }
    }

    void ShowContextMenu() const {
        POINT cursor{};
        if (!::GetCursorPos(&cursor)) {
            return;
        }

        HMENU menu = ::CreatePopupMenu();
        if (!menu) {
            return;
        }
        ::AppendMenuW(menu, MF_STRING, kCaptureCommand, L"截图\tCtrl+A");
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, kExitCommand, L"退出");

        // 隐藏窗口也必须先取得前台权，菜单点击其他区域时才能正常关闭。
        ::SetForegroundWindow(m_window);
        const UINT command = ::TrackPopupMenu(
            menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
            cursor.x, cursor.y, 0, m_window, nullptr);
        ::DestroyMenu(menu);
        ::PostMessageW(m_window, WM_NULL, 0, 0);

        if (command == kCaptureCommand) {
            StartCapture();
        } else if (command == kExitCommand) {
            ::DestroyWindow(m_window);
        }
    }

    void HandleTrayNotification(UINT notification) const {
        if (notification == WM_CONTEXTMENU || notification == WM_RBUTTONUP) {
            ShowContextMenu();
        } else if (notification == NIN_SELECT ||
                   notification == NIN_KEYSELECT ||
                   notification == WM_LBUTTONDBLCLK) {
            StartCapture();
        }
    }

    void RestoreTrayIcon() const {
        (void)AddTrayIcon();
    }

    [[nodiscard]] UINT TaskbarCreatedMessage() const noexcept {
        return m_taskbarCreated;
    }

    void Shutdown() {
        if (m_hotkeyRegistered) {
            ::UnregisterHotKey(m_window, kHotkeyId);
            m_hotkeyRegistered = false;
        }
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = m_window;
        icon.uID = kTrayIconId;
        ::Shell_NotifyIconW(NIM_DELETE, &icon);
    }

private:
    void NotifyHotkeyConflict() const {
        // 使用非阻塞托盘通知，快捷键冲突时仍可通过托盘菜单截图或退出。
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = m_window;
        icon.uID = kTrayIconId;
        icon.uFlags = NIF_INFO;
        wcscpy_s(icon.szInfoTitle, L"MarkShot 快捷键冲突");
        wcscpy_s(icon.szInfo,
                 L"Ctrl+A 已被其他程序占用，请释放该快捷键；当前可通过托盘菜单截图。");
        icon.dwInfoFlags = NIIF_WARNING;
        (void)::Shell_NotifyIconW(NIM_MODIFY, &icon);
    }

    bool AddTrayIcon() const {
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = m_window;
        icon.uID = kTrayIconId;
        icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        icon.uCallbackMessage = kTrayCallback;
        // 托盘空间很小，使用无底板的独立前景图标，避免主体被再次缩小。
        icon.hIcon = LoadIconResource(
            m_instance, IDI_NATIVESHOT_TRAY,
            ::GetSystemMetrics(SM_CXSMICON),
            ::GetSystemMetrics(SM_CYSMICON));
        wcscpy_s(icon.szTip, L"MarkShot - Ctrl+A 截图");
        if (!::Shell_NotifyIconW(NIM_ADD, &icon)) {
            return false;
        }
        icon.uVersion = NOTIFYICON_VERSION_4;
        (void)::Shell_NotifyIconW(NIM_SETVERSION, &icon);
        return true;
    }

    HINSTANCE m_instance = nullptr;
    HWND m_window = nullptr;
    UINT m_taskbarCreated = 0;
    bool m_hotkeyRegistered = false;
};

struct TrayCreation {
    TrayState* state = nullptr;
    bool adopted = false;
};

LRESULT CALLBACK TrayProcedure(HWND window, UINT message,
                               WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<TrayState*>(
        ::GetWindowLongPtrW(window, GWLP_USERDATA));

    if (state && message == state->TaskbarCreatedMessage()) {
        state->RestoreTrayIcon();
        return 0;
    }

    switch (message) {
        case WM_NCCREATE: {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto* payload = static_cast<TrayCreation*>(create->lpCreateParams);
            ::SetWindowLongPtrW(window, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(payload->state));
            payload->adopted = true;
            return TRUE;
        }
        case WM_HOTKEY:
            if (state && wParam == kHotkeyId) {
                state->StartCapture();
            }
            return 0;
        case kStartCapture:
            if (state) {
                state->StartCapture();
            }
            return 0;
        case kExitApplication:
            ::DestroyWindow(window);
            return 0;
        case kTrayCallback:
            if (state) {
                state->HandleTrayNotification(LOWORD(lParam));
            }
            return 0;
        case WM_DESTROY:
            if (state) {
                state->Shutdown();
            }
            ::PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY:
            delete state;
            ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            return 0;
        default:
            return ::DefWindowProcW(window, message, wParam, lParam);
    }
}

bool RegisterTrayWindowClass(HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = TrayProcedure;
    windowClass.lpszClassName = kTrayWindowClass;
    windowClass.hIcon = LoadIconResource(
        instance, IDI_NATIVESHOT, ::GetSystemMetrics(SM_CXICON),
        ::GetSystemMetrics(SM_CYICON));
    windowClass.hIconSm = LoadIconResource(
        instance, IDI_NATIVESHOT, ::GetSystemMetrics(SM_CXSMICON),
        ::GetSystemMetrics(SM_CYSMICON));
    return ::RegisterClassExW(&windowClass) != 0 ||
           ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND CreateTrayWindow(HINSTANCE instance) {
    auto state = std::make_unique<TrayState>(instance);
    TrayState* rawState = state.get();
    TrayCreation payload{rawState, false};
    HWND window = ::CreateWindowExW(
        0, kTrayWindowClass, L"MarkShot Tray", WS_OVERLAPPED,
        0, 0, 0, 0, nullptr, nullptr, instance, &payload);
    // WM_NCCREATE 成功后状态由窗口过程接管，否则 unique_ptr 自动回收。
    if (payload.adopted) {
        state.release();
    }
    if (!window) {
        return nullptr;
    }
    if (!rawState->Initialize(window)) {
        ::DestroyWindow(window);
        return nullptr;
    }
    return window;
}

}  // namespace

int RunTrayApplication(HINSTANCE instance) {
    // 第二次启动只通知已有实例截图，不再创建第二个托盘进程。
    HANDLE mutex = ::CreateMutexW(nullptr, TRUE, kSingleInstanceName);
    if (!mutex) {
        return 1;
    }
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = ::FindWindowW(kTrayWindowClass, nullptr)) {
            ::PostMessageW(existing, kStartCapture, 0, 0);
        }
        ::CloseHandle(mutex);
        return 0;
    }

    if (!RegisterTrayWindowClass(instance) || !CreateTrayWindow(instance)) {
        ::MessageBoxW(nullptr, L"无法创建系统托盘图标。", L"MarkShot",
                      MB_OK | MB_ICONERROR);
        ::ReleaseMutex(mutex);
        ::CloseHandle(mutex);
        return 1;
    }

    // 隐藏宿主也必须持续泵消息，热键和托盘回调才能随时唤起截图层。
    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    ::ReleaseMutex(mutex);
    ::CloseHandle(mutex);
    return static_cast<int>(message.wParam);
}

}  // namespace native_shot
