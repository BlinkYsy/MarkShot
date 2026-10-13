#pragma once

#include "AnnotationDocument.h"
#include "ScreenImage.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <optional>
#include <string>
#include <vector>

namespace native_shot {

enum class ToolbarIcon {
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
    StrokeSize,
    ColorSwatch,
};

// 工具栏按钮的布局结果。bounds 与截图层一样使用客户区物理像素。
struct ToolbarButton {
    RECT bounds{};
    ToolbarIcon icon = ToolbarIcon::Rectangle;
    std::wstring tooltip;
    bool selected = false;
    bool enabled = true;
    COLORREF color = RGB(255, 75, 80);
    float radius = 4.0F;
};

// 负责截图层的 Direct2D/DirectWrite 绘制，并把选区与标注合成为最终位图。
// 工具状态和交互状态由 OverlayState 管理，渲染器本身不处理输入。
class RenderEngine {
public:
    // 保存窗口句柄并创建与窗口无关的 D2D/DWrite 资源。
    [[nodiscard]] bool Initialize(HWND window);
    // 丢弃随窗口尺寸或设备变化而需要重建的资源。
    void DiscardWindowResources();

    // 按“桌面 -> 遮罩/选区 -> 标注 -> 工具栏”的顺序绘制一帧。
    void Paint(const ScreenImage& desktop, const RECT& selection,
               const AnnotationDocument& document,
               const std::optional<Annotation>& draft,
               const std::vector<ToolbarButton>& buttons, int hoveredButton,
               const std::vector<ToolbarButton>& styleButtons, int hoveredStyle,
               const std::optional<PointF>& mosaicCursor, float mosaicRadius);

    // 生成保存、复制和置顶共用的最终选区图像，不包含遮罩和工具栏。
    [[nodiscard]] ScreenImage ComposeSelection(
        const ScreenImage& desktop, const RECT& selection,
        const AnnotationDocument& document,
        const std::optional<Annotation>& draft = std::nullopt);

private:
    // 窗口相关资源采用按需创建，处理设备丢失后下一帧自动恢复。
    [[nodiscard]] bool EnsureWindowTarget();
    [[nodiscard]] Microsoft::WRL::ComPtr<ID2D1Bitmap> CreateBitmap(
        ID2D1RenderTarget* target, const ScreenImage& image) const;
    void DrawAnnotation(ID2D1RenderTarget* target, const Annotation& annotation,
                        float offsetX, float offsetY) const;
    void DrawToolbar(ID2D1RenderTarget* target,
                     const std::vector<ToolbarButton>& buttons,
                     int hoveredButton) const;
    void DrawToolbarIcon(ID2D1RenderTarget* target, const ToolbarButton& button,
                         ID2D1SolidColorBrush* brush) const;
    void DrawTooltip(ID2D1RenderTarget* target, const ToolbarButton& button) const;

    HWND m_window = nullptr;
    Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
    Microsoft::WRL::ComPtr<IDWriteFactory> m_writeFactory;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_uiFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_iconTextFormat;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> m_windowTarget;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_desktopBitmap;
};

}  // namespace native_shot
