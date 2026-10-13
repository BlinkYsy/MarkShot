#include "OverlayWindow.h"

#include "AnnotationDocument.h"
#include "OcrDialog.h"
#include "OcrEngine.h"
#include "PinWindow.h"
#include "RenderEngine.h"
#include "ScreenImage.h"

#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace native_shot {
namespace {

constexpr wchar_t kOverlayClassName[] = L"MarkShot.OverlayWindow";
// 进程已启用 Per-Monitor DPI；这些布局值和输入坐标均表示物理像素。
constexpr int kMinimumSelection = 8;
constexpr int kToolbarHeight = 52;
constexpr int kToolbarGap = 4;

enum class Command {
    Rectangle,
    Ellipse,
    Line,
    Arrow,
    Pen,
    Text,
    Counter,
    Mosaic,
    Ocr,
    Translate,
    Undo,
    Redo,
    Save,
    Pin,
    Cancel,
    Finish,
};

struct ButtonDefinition {
    Command command;
    ToolbarIcon icon;
    const wchar_t* tooltip;
    int width;
};

// 静态表只描述命令和视觉信息；运行时位置与可用状态存入 ToolbarButton。
constexpr std::array<ButtonDefinition, 16> kButtonDefinitions{{
    {Command::Rectangle, ToolbarIcon::Rectangle, L"矩形", 48},
    {Command::Ellipse, ToolbarIcon::Ellipse, L"圆", 48},
    {Command::Line, ToolbarIcon::Line, L"直线", 48},
    {Command::Arrow, ToolbarIcon::Arrow, L"箭头", 48},
    {Command::Pen, ToolbarIcon::Pen, L"画笔", 48},
    {Command::Text, ToolbarIcon::Text, L"文本", 48},
    {Command::Counter, ToolbarIcon::Counter, L"序号", 48},
    {Command::Mosaic, ToolbarIcon::Mosaic, L"马赛克", 48},
    {Command::Ocr, ToolbarIcon::Ocr, L"识别文字", 48},
    {Command::Translate, ToolbarIcon::Translate, L"英文译中文（百度标准版）", 48},
    {Command::Undo, ToolbarIcon::Undo, L"撤销", 48},
    {Command::Redo, ToolbarIcon::Redo, L"重做", 48},
    {Command::Save, ToolbarIcon::Save, L"保存", 48},
    {Command::Pin, ToolbarIcon::Pin, L"置顶", 48},
    {Command::Cancel, ToolbarIcon::Cancel, L"取消", 48},
    {Command::Finish, ToolbarIcon::Finish, L"完成", 48},
}};

RECT NormalizeRect(POINT first, POINT second) {
    return {
        std::min(first.x, second.x), std::min(first.y, second.y),
        std::max(first.x, second.x), std::max(first.y, second.y),
    };
}

bool Contains(const RECT& rect, POINT point) {
    return point.x >= rect.left && point.x < rect.right &&
           point.y >= rect.top && point.y < rect.bottom;
}

PointF ClampPoint(POINT point, const RECT& bounds) {
    return {
        static_cast<float>(std::clamp(point.x, bounds.left, bounds.right)),
        static_cast<float>(std::clamp(point.y, bounds.top, bounds.bottom)),
    };
}

Tool ToolFor(Command command) {
    switch (command) {
        case Command::Rectangle: return Tool::Rectangle;
        case Command::Ellipse: return Tool::Ellipse;
        case Command::Line: return Tool::Line;
        case Command::Arrow: return Tool::Arrow;
        case Command::Pen: return Tool::Pen;
        case Command::Text: return Tool::Text;
        case Command::Counter: return Tool::Counter;
        case Command::Mosaic: return Tool::Mosaic;
        default: return Tool::None;
    }
}

class OverlayState {
public:
    OverlayState(HINSTANCE instance, DesktopCapture capture)
        : m_instance(instance), m_capture(std::move(capture)) {}

    bool Initialize(HWND window) {
        m_window = window;
        return m_renderer.Initialize(window);
    }

    void Paint() {
        SyncButtonStates();
        m_renderer.Paint(m_capture.image, m_selection, m_document,
                         CurrentDraft(), m_buttons, m_hoveredButton,
                         m_styleButtons, m_hoveredStyle, MosaicCursor(),
                         8.0F * (m_mosaicSize + 1));
    }

    // 圆形笔刷只在选区内显示；工具栏（含空隙）保留普通光标。
    std::optional<PointF> MosaicCursor() const {
        POINT point{};
        if (!m_selectionComplete || m_tool != Tool::Mosaic ||
            !::GetCursorPos(&point) || !::ScreenToClient(m_window, &point) ||
            !Contains(m_selection, point)) return std::nullopt;
        for (const auto* buttons : {&m_buttons, &m_styleButtons}) {
            if (buttons->empty()) continue;
            RECT panel = buttons->front().bounds;
            panel.right = buttons->back().bounds.right;
            ::InflateRect(&panel, 7, 5);
            if (Contains(panel, point)) return std::nullopt;
        }
        return PointF{static_cast<float>(point.x), static_cast<float>(point.y)};
    }

    void UpdateCursor() const {
        ::SetCursor(MosaicCursor() ? nullptr : ::LoadCursorW(nullptr, IDC_CROSS));
    }

    void OnMouseLeave() {
        Invalidate();
    }

    void OnLeftButtonDown(POINT point) {
        ::SetFocus(m_window);
        if (!m_selectionComplete) {
            BeginSelection(point);
            return;
        }

        // 子栏先处理命中；面板空隙也吃掉点击，避免在面板下方误画。
        if (ApplyStyleAt(point)) return;
        const int buttonIndex = ButtonAt(point);
        if (buttonIndex >= 0) {
            ApplyCommand(kButtonDefinitions[buttonIndex].command);
            return;
        }
        BeginAnnotation(point);
    }

    void OnMouseMove(POINT point) {
        if (m_tool == Tool::Mosaic) {
            // 悬停也重绘轮廓；离开窗口时清除，拖动捕获期间也同步隐藏系统光标。
            TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_LEAVE, m_window, 0};
            ::TrackMouseEvent(&tracking);
            UpdateCursor();
            Invalidate();
        }
        if (m_selecting) {
            m_selection = NormalizeRect(m_dragStart, point);
            Invalidate();
            return;
        }
        if (m_drawing && m_draft) {
            const PointF current = ClampPoint(point, m_selection);
            if (m_draft->tool == Tool::Pen || m_draft->tool == Tool::Mosaic) {
                m_draft->points.push_back(current);
            } else {
                m_draft->points.back() = current;
            }
            Invalidate();
            return;
        }

        const int hovered = ButtonAt(point);
        const int hoveredStyle = StyleButtonAt(point);
        if (hovered != m_hoveredButton || hoveredStyle != m_hoveredStyle) {
            m_hoveredButton = hovered;
            m_hoveredStyle = hoveredStyle;
            Invalidate();
        }
    }

    void OnLeftButtonUp(POINT point) {
        if (m_selecting) {
            FinishSelection(point);
            return;
        }
        if (m_drawing && m_draft) {
            if (m_draft->tool == Tool::Mosaic) {
                m_draft->points.push_back(ClampPoint(point, m_selection));
            }
            CommitDraft();
        }
    }

    void OnCharacter(wchar_t character) {
        if (!m_textEditing) {
            return;
        }
        if (character == L'\b') {
            if (!m_textBuffer.empty()) m_textBuffer.pop_back();
        } else if (character >= L' ') {
            m_textBuffer.push_back(character);
        }
        Invalidate();
    }

    void OnKeyDown(WPARAM key) {
        const bool control = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (m_textEditing) {
            if (key == VK_RETURN) CommitText();
            else if (key == VK_ESCAPE) CancelText();
            return;
        }

        if (key == VK_ESCAPE) {
            Close();
        } else if (key == VK_RETURN && m_selectionComplete) {
            Finish();
        } else if (control && key == 'Z') {
            m_document.Undo(); Invalidate();
        } else if (control && key == 'Y') {
            m_document.Redo(); Invalidate();
        } else if (control && key == 'S') {
            Save();
        } else if (control && key == 'C') {
            Copy();
        }
    }

private:
    void BeginSelection(POINT point) {
        m_selecting = true;
        m_dragStart = point;
        m_selection = NormalizeRect(point, point);
        ::SetCapture(m_window);
        Invalidate();
    }

    void FinishSelection(POINT point) {
        m_selecting = false;
        ::ReleaseCapture();
        m_selection = NormalizeRect(m_dragStart, point);
        const bool largeEnough =
            m_selection.right - m_selection.left >= kMinimumSelection &&
            m_selection.bottom - m_selection.top >= kMinimumSelection;
        if (largeEnough) {
            m_selectionComplete = true;
            // 完成截图选区后不预选绘图工具，避免误触时直接画出矩形。
            m_tool = Tool::None;
            LayoutToolbar();
        } else {
            // 过小的误拖不产生选区，用户可以立即重新框选。
            m_selection = {};
        }
        Invalidate();
    }

    void BeginAnnotation(POINT point) {
        if (!Contains(m_selection, point) || m_tool == Tool::None) {
            return;
        }
        if (m_tool == Tool::Counter) {
            Annotation annotation = NewAnnotation(Tool::Counter);
            annotation.points.push_back(ClampPoint(point, m_selection));
            annotation.counterValue = m_nextCounter++;
            m_document.Add(std::move(annotation));
            Invalidate();
            return;
        }
        if (m_tool == Tool::Text) {
            BeginText(point);
            return;
        }

        m_drawing = true;
        m_draft = NewAnnotation(m_tool);
        const PointF start = ClampPoint(point, m_selection);
        m_draft->points = {start, start};
        ::SetCapture(m_window);
        Invalidate();
    }

    void CommitDraft() {
        m_drawing = false;
        ::ReleaseCapture();
        const RECT bounds = BoundsOf(*m_draft);
        const bool penHasStroke = (m_draft->tool == Tool::Pen || m_draft->tool == Tool::Mosaic) &&
                                  m_draft->points.size() > 1;
        if (penHasStroke || bounds.right > bounds.left || bounds.bottom > bounds.top) {
            m_document.Add(std::move(*m_draft));
        }
        m_draft.reset();
        Invalidate();
    }

    Annotation NewAnnotation(Tool tool) const {
        Annotation annotation;
        annotation.tool = tool;
        annotation.color = RGB(255, 64, 64);
        annotation.thickness = tool == Tool::Pen ? 4.0F : 3.0F;
        m_style.Apply(annotation);
        if (tool == Tool::Mosaic) annotation.thickness = 16.0F * (m_mosaicSize + 1);
        return annotation;
    }

    std::optional<Annotation> CurrentDraft() const {
        if (m_textEditing) {
            Annotation preview = NewAnnotation(Tool::Text);
            preview.points.push_back(m_textOrigin);
            preview.text = m_textBuffer + L"|";
            return preview;
        }
        return m_draft;
    }

    void BeginText(POINT point) {
        m_textEditing = true;
        m_textBuffer.clear();
        m_textOrigin = ClampPoint(point, m_selection);
        Invalidate();
    }

    void CommitText() {
        if (!m_textBuffer.empty()) {
            Annotation annotation = NewAnnotation(Tool::Text);
            annotation.points.push_back(m_textOrigin);
            annotation.text = m_textBuffer;
            m_document.Add(std::move(annotation));
        }
        m_textEditing = false;
        m_textBuffer.clear();
        Invalidate();
    }

    void CancelText() {
        m_textEditing = false;
        m_textBuffer.clear();
        Invalidate();
    }

    void LayoutToolbar() {
        RECT client{};
        ::GetClientRect(m_window, &client);
        int totalWidth = kToolbarGap;
        for (const auto& definition : kButtonDefinitions) {
            totalWidth += definition.width + kToolbarGap;
        }

        int left = (m_selection.left + m_selection.right - totalWidth) / 2;
        const int maximumLeft = std::max(4, static_cast<int>(client.right) - totalWidth - 4);
        left = std::clamp(left, 4, maximumLeft);
        // 优先放在选区下方；空间不足时移到上方，最后限制在客户区内。
        int top = m_selection.bottom + 10;
        if (top + kToolbarHeight > client.bottom - 4) {
            top = m_selection.top - kToolbarHeight - 10;
        }
        const int maximumTop = std::max(
            4, static_cast<int>(client.bottom) - kToolbarHeight - 4);
        top = std::clamp(top, 4, maximumTop);

        m_buttons.clear();
        int x = left + kToolbarGap;
        for (const auto& definition : kButtonDefinitions) {
            ToolbarButton button;
            button.bounds = {x, top, x + definition.width, top + kToolbarHeight};
            button.icon = definition.icon;
            button.tooltip = definition.tooltip;
            m_buttons.push_back(std::move(button));
            x += definition.width + kToolbarGap;
        }
    }

    void SyncButtonStates() {
        for (std::size_t i = 0; i < m_buttons.size(); ++i) {
            const Command command = kButtonDefinitions[i].command;
            const Tool buttonTool = ToolFor(command);
            m_buttons[i].selected = buttonTool != Tool::None && buttonTool == m_tool;
            if (command == Command::Undo) m_buttons[i].enabled = m_document.CanUndo();
            else if (command == Command::Redo) m_buttons[i].enabled = m_document.CanRedo();
            else m_buttons[i].enabled = true;
        }
    }

    // 复用主栏按钮的绘制与提示，子栏只负责自己的位置和样式值。
    void LayoutStyleToolbar() {
        m_styleButtons.clear();
        m_hoveredStyle = -1;
        const bool mosaic = m_tool == Tool::Mosaic;
        if ((!AnnotationStyle::Supports(m_tool) && !mosaic) || m_buttons.empty()) return;
        RECT client{};
        ::GetClientRect(m_window, &client);
        const int count = mosaic ? 3 : 9;
        const int width = count * 48 + (mosaic ? 0 : 16);
        const int left = std::clamp(static_cast<int>(m_buttons.front().bounds.left),
            8, std::max(8, static_cast<int>(client.right) - width - 8));
        int top = m_buttons.front().bounds.bottom + 16;
        if (top + 48 + 5 > client.bottom) {
            top = m_buttons.front().bounds.top - 64;
        }
        top = std::clamp(top, 8, std::max(8, static_cast<int>(client.bottom) - 56));
        constexpr const wchar_t* colorNames[]{L"蓝色", L"绿色", L"黄色", L"灰色", L"白色", L"红色"};
        for (int i = 0; i < count; ++i) {
            ToolbarButton button;
            const int x = left + i * 48 + (i >= 3 ? 16 : 0);
            button.bounds = {x, top, x + 48, top + 48};
            if (i < 3) {
                button.icon = ToolbarIcon::StrokeSize;
                button.radius = 4.0F * (i + 1);
                button.selected = i == (mosaic ? m_mosaicSize : m_style.sizeIndex);
                button.tooltip = mosaic ? L"马赛克笔刷 " + std::to_wstring(16 * (i + 1))
                    : m_tool == Tool::Text
                    ? L"字号 " + std::to_wstring(static_cast<int>(AnnotationStyle::fontSizes[i]))
                    : L"线宽 " + std::to_wstring(static_cast<int>(AnnotationStyle::widths[i]));
            } else {
                button.icon = ToolbarIcon::ColorSwatch;
                button.color = AnnotationStyle::colors[i - 3];
                button.selected = i - 3 == m_style.colorIndex;
                button.tooltip = colorNames[i - 3];
            }
            m_styleButtons.push_back(std::move(button));
        }
    }

    int StyleButtonAt(POINT point) const {
        for (std::size_t i = 0; i < m_styleButtons.size(); ++i) {
            if (Contains(m_styleButtons[i].bounds, point)) return static_cast<int>(i);
        }
        return -1;
    }

    bool ApplyStyleAt(POINT point) {
        if (m_styleButtons.empty()) return false;
        RECT panel = m_styleButtons.front().bounds;
        panel.right = m_styleButtons.back().bounds.right;
        ::InflateRect(&panel, 7, 5);
        if (!Contains(panel, point)) return false;
        const int index = StyleButtonAt(point);
        if (index >= 0) {
            if (m_tool == Tool::Mosaic) m_mosaicSize = index;
            else if (index < 3) m_style.sizeIndex = index;
            else m_style.colorIndex = index - 3;
            LayoutStyleToolbar();
            m_hoveredStyle = index;
            Invalidate();
        }
        return true;
    }

    int ButtonAt(POINT point) const {
        for (std::size_t i = 0; i < m_buttons.size(); ++i) {
            if (Contains(m_buttons[i].bounds, point)) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    void ApplyCommand(Command command) {
        // 绘图工具只切换当前模式；操作命令在点击时立即执行。
        const Tool tool = ToolFor(command);
        if (tool != Tool::None) {
            m_tool = tool;
            CancelText();
            LayoutStyleToolbar();
            Invalidate();
            return;
        }
        switch (command) {
            case Command::Undo: m_document.Undo(); Invalidate(); break;
            case Command::Redo: m_document.Redo(); Invalidate(); break;
            case Command::Ocr: Ocr(); break;
            case Command::Translate: Translate(); break;
            case Command::Save: Save(); break;
            case Command::Pin: Pin(); break;
            case Command::Cancel: Close(); break;
            case Command::Finish: Finish(); break;
            default: break;
        }
    }

    ScreenImage Compose() {
        // 保存、复制和置顶都从同一合成入口取得完全一致的结果。
        return m_renderer.ComposeSelection(m_capture.image, m_selection, m_document);
    }

    bool Copy() {
        ScreenImage image = Compose();
        return CopyToClipboard(m_window, image);
    }

    void Ocr() {
        const ScreenImage image = Compose();
        // OCR 是一次性操作：结果窗口关闭后结束截图会话，不恢复原选区。
        ::ShowWindow(m_window, SW_HIDE);
        const OcrTextResult result = RecognizeText(image);
        ShowOcrResultDialog(nullptr, result);
        Close();
    }

    void Translate() {
        const ScreenImage image = Compose();
        // 与 OCR 一样，关闭结果后结束截图会话，托盘继续驻留。
        ::ShowWindow(m_window, SW_HIDE);
        ShowTranslationDialog(image);
        Close();
    }

    void Save() {
        wchar_t filename[MAX_PATH] = L"screenshot.png";
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = m_window;
        dialog.lpstrFilter = L"PNG 图片 (*.png)\0*.png\0\0";
        dialog.lpstrFile = filename;
        dialog.nMaxFile = MAX_PATH;
        dialog.lpstrDefExt = L"png";
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        if (::GetSaveFileNameW(&dialog)) {
            const ScreenImage image = Compose();
            if (!SavePng(image, filename)) {
                ::MessageBoxW(m_window, L"保存 PNG 失败。", L"MarkShot",
                              MB_OK | MB_ICONERROR);
            }
        }
    }

    void Pin() {
        ScreenImage image = Compose();
        POINT position{
            m_capture.origin.x + m_selection.left,
            m_capture.origin.y + m_selection.top,
        };
        // 置顶成功后立即退出截图选择层，只保留桌面上的置顶图片。
        if (CreatePinWindow(m_instance, std::move(image), position)) {
            Close();
        }
    }

    void Finish() {
        if (Copy()) {
            Close();
        } else {
            ::MessageBoxW(m_window, L"复制到剪贴板失败。", L"MarkShot",
                          MB_OK | MB_ICONERROR);
        }
    }

    void Close() {
        if (::IsWindow(m_window)) {
            ::DestroyWindow(m_window);
        }
    }

    void Invalidate() const {
        ::InvalidateRect(m_window, nullptr, FALSE);
    }

    HINSTANCE m_instance = nullptr;
    HWND m_window = nullptr;
    DesktopCapture m_capture;
    RenderEngine m_renderer;
    AnnotationDocument m_document;
    RECT m_selection{};
    POINT m_dragStart{};
    bool m_selecting = false;
    bool m_selectionComplete = false;
    bool m_drawing = false;
    Tool m_tool = Tool::None;
    std::optional<Annotation> m_draft;
    std::vector<ToolbarButton> m_buttons;
    AnnotationStyle m_style;
    int m_mosaicSize = 1;
    std::vector<ToolbarButton> m_styleButtons;
    int m_hoveredStyle = -1;
    int m_hoveredButton = -1;
    bool m_textEditing = false;
    std::wstring m_textBuffer;
    PointF m_textOrigin{};
    int m_nextCounter = 1;
};

// CreateWindowExW 在返回前会同步派发 WM_NCCREATE。用该载荷明确记录所有权
// 是否已交给窗口过程，避免创建中途失败时发生重复释放。
struct OverlayCreation {
    OverlayState* state = nullptr;
    bool adopted = false;
};

LRESULT CALLBACK OverlayProcedure(HWND window, UINT message,
                                  WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<OverlayState*>(
        ::GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
        case WM_NCCREATE: {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto* payload = static_cast<OverlayCreation*>(create->lpCreateParams);
            state = payload->state;
            payload->adopted = true;
            ::SetWindowLongPtrW(window, GWLP_USERDATA,
                               reinterpret_cast<LONG_PTR>(state));
            return TRUE;
        }
        case WM_CREATE:
            // 窗口资源失败时 Paint 会安全返回；不在创建回调中销毁状态。
            if (state) (void)state->Initialize(window);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            ::BeginPaint(window, &paint);
            if (state) state->Paint();
            ::EndPaint(window, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONDOWN:
            if (state) state->OnLeftButtonDown({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;
        case WM_MOUSEMOVE:
            if (state) state->OnMouseMove({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;
        case WM_MOUSELEAVE:
            if (state) state->OnMouseLeave();
            return 0;
        case WM_LBUTTONUP:
            if (state) state->OnLeftButtonUp({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;
        case WM_CHAR:
            if (state) state->OnCharacter(static_cast<wchar_t>(wParam));
            return 0;
        case WM_KEYDOWN:
            if (state) state->OnKeyDown(wParam);
            return 0;
        case WM_SETCURSOR:
            if (state) state->UpdateCursor();
            return TRUE;
        case WM_DESTROY:
            return 0;
        case WM_NCDESTROY:
            delete state;
            ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            return 0;
        default:
            return ::DefWindowProcW(window, message, wParam, lParam);
    }
}

bool RegisterOverlayClass(HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = OverlayProcedure;
    windowClass.lpszClassName = kOverlayClassName;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_CROSS);
    return ::RegisterClassExW(&windowClass) != 0 ||
           ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

}  // namespace

HWND CreateOverlayWindow(HINSTANCE instance) {
    DesktopCapture capture = CaptureVirtualDesktop();
    if (capture.image.Empty() || !RegisterOverlayClass(instance)) {
        return nullptr;
    }
    const POINT origin = capture.origin;
    const int width = capture.image.width;
    const int height = capture.image.height;
    auto state = std::make_unique<OverlayState>(instance, std::move(capture));
    OverlayCreation payload{state.get(), false};

    HWND window = ::CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kOverlayClassName, L"MarkShot", WS_POPUP,
        origin.x, origin.y, width, height,
        nullptr, nullptr, instance, &payload);
    if (payload.adopted) {
        state.release();
    }
    if (!window) {
        return nullptr;
    }
    ::ShowWindow(window, SW_SHOW);
    ::UpdateWindow(window);
    ::SetForegroundWindow(window);
    return window;
}

}  // namespace native_shot
