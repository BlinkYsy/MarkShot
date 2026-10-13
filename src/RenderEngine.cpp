#include "RenderEngine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace native_shot {
namespace {

using Microsoft::WRL::ComPtr;

D2D1_COLOR_F ToColor(COLORREF color, float alpha = 1.0F) {
    return D2D1::ColorF(GetRValue(color) / 255.0F,
                        GetGValue(color) / 255.0F,
                        GetBValue(color) / 255.0F, alpha);
}

D2D1_POINT_2F ToPoint(PointF point, float offsetX, float offsetY) {
    return D2D1::Point2F(point.x + offsetX, point.y + offsetY);
}

D2D1_RECT_F ToRect(const Annotation& annotation, float offsetX, float offsetY) {
    const RECT bounds = BoundsOf(annotation);
    return D2D1::RectF(bounds.left + offsetX, bounds.top + offsetY,
                       bounds.right + offsetX, bounds.bottom + offsetY);
}

BITMAPINFO BitmapInfoFor(const ScreenImage& image) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = image.width;
    info.bmiHeader.biHeight = -image.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    return info;
}

void DrawPenIcon(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                 float left, float right, float top, float bottom, float stroke) {
    // 画笔由封闭的斜向笔身和独立笔触组成，不依赖系统图标字体。
    const D2D1_POINT_2F tip = D2D1::Point2F(left + 2.0F, bottom - 1.0F);
    const D2D1_POINT_2F lowerEdge = D2D1::Point2F(left + 5.0F, bottom - 8.0F);
    const D2D1_POINT_2F upperEdge = D2D1::Point2F(right - 3.0F, top);
    const D2D1_POINT_2F cap = D2D1::Point2F(right, top + 3.0F);
    const D2D1_POINT_2F innerEdge = D2D1::Point2F(left + 8.0F, bottom - 5.0F);

    target->DrawLine(tip, lowerEdge, brush, stroke);
    target->DrawLine(lowerEdge, upperEdge, brush, stroke);
    target->DrawLine(upperEdge, cap, brush, stroke);
    target->DrawLine(cap, innerEdge, brush, stroke);
    target->DrawLine(innerEdge, tip, brush, stroke);
    target->DrawLine(lowerEdge, innerEdge, brush, stroke);
    target->DrawLine(D2D1::Point2F(left - 1.0F, bottom + 4.0F),
                     D2D1::Point2F(right + 1.0F, bottom + 4.0F), brush, stroke);
}

void DrawHistoryIcon(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                     float left, float right, float top, float bottom,
                     bool redo, float stroke) {
    const float centerX = (left + right) * 0.5F;
    const float centerY = (top + bottom) * 0.5F + 1.0F;
    constexpr float radius = 11.5F;
    constexpr int segmentCount = 15;
    constexpr float startDegrees = 210.0F;
    constexpr float sweepDegrees = 300.0F;

    // 用基础线段逼近接近整圈的圆弧，不依赖字体或路径几何。
    // 重做图标水平镜像同一套坐标，保证两个图标严格对称。
    const auto mirrorX = [&](float x) {
        return redo ? left + right - x : x;
    };
    const auto arcPoint = [&](int index) {
        const float degrees = startDegrees +
            sweepDegrees * static_cast<float>(index) / segmentCount;
        const float radians = degrees * std::numbers::pi_v<float> / 180.0F;
        return D2D1::Point2F(
            mirrorX(centerX + std::cos(radians) * radius),
            centerY + std::sin(radians) * radius);
    };

    D2D1_POINT_2F previous = arcPoint(0);
    for (int index = 1; index <= segmentCount; ++index) {
        const D2D1_POINT_2F current = arcPoint(index);
        target->DrawLine(previous, current, brush, stroke);
        previous = current;
    }

    const D2D1_POINT_2F arrowTip = arcPoint(0);
    target->DrawLine(
        arrowTip, D2D1::Point2F(arrowTip.x, arrowTip.y - 7.0F), brush, stroke);
    target->DrawLine(
        arrowTip,
        D2D1::Point2F(arrowTip.x + (redo ? -7.0F : 7.0F), arrowTip.y),
        brush, stroke);
}

void DrawPinIcon(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                 float centerX, float top, float bottom, float stroke) {
    // 直立图钉使用对称轮廓，避免字形旋转后的边界裁切。
    const float shoulderY = top + 10.0F;
    const float baseY = bottom - 5.0F;
    target->DrawLine(D2D1::Point2F(centerX - 7.0F, top),
                     D2D1::Point2F(centerX + 7.0F, top), brush, stroke);
    target->DrawLine(D2D1::Point2F(centerX - 5.0F, top),
                     D2D1::Point2F(centerX - 5.0F, shoulderY), brush, stroke);
    target->DrawLine(D2D1::Point2F(centerX + 5.0F, top),
                     D2D1::Point2F(centerX + 5.0F, shoulderY), brush, stroke);
    target->DrawLine(D2D1::Point2F(centerX - 5.0F, shoulderY),
                     D2D1::Point2F(centerX - 9.0F, baseY), brush, stroke);
    target->DrawLine(D2D1::Point2F(centerX + 5.0F, shoulderY),
                     D2D1::Point2F(centerX + 9.0F, baseY), brush, stroke);
    target->DrawLine(D2D1::Point2F(centerX - 9.0F, baseY),
                     D2D1::Point2F(centerX + 9.0F, baseY), brush, stroke);
    target->DrawLine(D2D1::Point2F(centerX, baseY),
                     D2D1::Point2F(centerX, bottom + 5.0F), brush, stroke);
}

}  // namespace

bool RenderEngine::Initialize(HWND window) {
    m_window = window;
    if (FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   m_d2dFactory.ReleaseAndGetAddressOf()))) {
        return false;
    }
    if (FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                     __uuidof(IDWriteFactory),
                                     reinterpret_cast<IUnknown**>(
                                         m_writeFactory.ReleaseAndGetAddressOf())))) {
        return false;
    }
    if (FAILED(m_writeFactory->CreateTextFormat(
        L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0F, L"zh-CN",
        m_uiFormat.ReleaseAndGetAddressOf()))) {
        return false;
    }
    if (FAILED(m_writeFactory->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 23.0F, L"en-US",
        m_iconTextFormat.ReleaseAndGetAddressOf()))) {
        return false;
    }
    return true;
}

void RenderEngine::DiscardWindowResources() {
    m_desktopBitmap.Reset();
    m_windowTarget.Reset();
}

bool RenderEngine::EnsureWindowTarget() {
    if (m_windowTarget) {
        return true;
    }
    RECT client{};
    ::GetClientRect(m_window, &client);
    const HRESULT result = m_d2dFactory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(
            m_window, D2D1::SizeU(client.right, client.bottom)),
        m_windowTarget.ReleaseAndGetAddressOf());
    if (SUCCEEDED(result)) {
        // 整条截图链都使用物理像素；禁用 Direct2D 的额外 DPI 换算，避免在
        // 150%/200%/400% 显示器上选框和工具栏被重复放大。
        m_windowTarget->SetDpi(96.0F, 96.0F);
    }
    return SUCCEEDED(result);
}

ComPtr<ID2D1Bitmap> RenderEngine::CreateBitmap(
    ID2D1RenderTarget* target, const ScreenImage& image) const {
    ComPtr<ID2D1Bitmap> bitmap;
    if (!target || image.Empty()) {
        return bitmap;
    }
    const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_IGNORE));
    target->CreateBitmap(D2D1::SizeU(image.width, image.height),
                         image.pixels.data(), image.stride, properties, &bitmap);
    return bitmap;
}

void RenderEngine::DrawAnnotation(ID2D1RenderTarget* target,
                                  const Annotation& annotation,
                                  float offsetX, float offsetY) const {
    if (!target || annotation.points.empty()) {
        return;
    }

    ComPtr<ID2D1SolidColorBrush> brush;
    target->CreateSolidColorBrush(ToColor(annotation.color), &brush);
    if (!brush) {
        return;
    }

    // offset 把截图层坐标转换到目标坐标；窗口预览传 0，选区合成传负选区原点。
    const auto first = ToPoint(annotation.points.front(), offsetX, offsetY);
    const auto last = ToPoint(annotation.points.back(), offsetX, offsetY);
    const auto bounds = ToRect(annotation, offsetX, offsetY);
    switch (annotation.tool) {
        case Tool::Rectangle:
            target->DrawRectangle(bounds, brush.Get(), annotation.thickness);
            break;
        case Tool::Ellipse: {
            const D2D1_ELLIPSE ellipse{
                D2D1::Point2F((bounds.left + bounds.right) * 0.5F,
                               (bounds.top + bounds.bottom) * 0.5F),
                std::abs(bounds.right - bounds.left) * 0.5F,
                std::abs(bounds.bottom - bounds.top) * 0.5F};
            target->DrawEllipse(ellipse, brush.Get(), annotation.thickness);
            break;
        }
        case Tool::Line:
            target->DrawLine(first, last, brush.Get(), annotation.thickness);
            break;
        case Tool::Arrow: {
            target->DrawLine(first, last, brush.Get(), annotation.thickness);
            const float angle = std::atan2(last.y - first.y, last.x - first.x);
            const float head = std::max(12.0F, annotation.thickness * 4.0F);
            constexpr float spread = std::numbers::pi_v<float> / 6.0F;
            const D2D1_POINT_2F sideA = D2D1::Point2F(
                last.x - head * std::cos(angle - spread),
                last.y - head * std::sin(angle - spread));
            const D2D1_POINT_2F sideB = D2D1::Point2F(
                last.x - head * std::cos(angle + spread),
                last.y - head * std::sin(angle + spread));
            target->DrawLine(last, sideA, brush.Get(), annotation.thickness);
            target->DrawLine(last, sideB, brush.Get(), annotation.thickness);
            break;
        }
        case Tool::Pen:
            for (std::size_t i = 1; i < annotation.points.size(); ++i) {
                target->DrawLine(ToPoint(annotation.points[i - 1], offsetX, offsetY),
                                 ToPoint(annotation.points[i], offsetX, offsetY),
                                 brush.Get(), annotation.thickness);
            }
            break;
        case Tool::Text: {
            ComPtr<IDWriteTextFormat> format;
            m_writeFactory->CreateTextFormat(
                L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, annotation.fontSize,
                L"zh-CN", &format);
            const D2D1_RECT_F textRect = D2D1::RectF(
                first.x, first.y, first.x + 600.0F, first.y + 100.0F);
            target->DrawTextW(annotation.text.c_str(),
                              static_cast<UINT32>(annotation.text.size()),
                              format.Get(), textRect, brush.Get());
            break;
        }
        case Tool::Counter: {
            constexpr float radius = 16.0F;
            target->FillEllipse({first, radius, radius}, brush.Get());
            ComPtr<ID2D1SolidColorBrush> white;
            target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &white);
            ComPtr<IDWriteTextFormat> format;
            m_writeFactory->CreateTextFormat(
                L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 17.0F,
                L"en-US", &format);
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const std::wstring number = std::to_wstring(annotation.counterValue);
            const D2D1_RECT_F textRect = D2D1::RectF(
                first.x - radius, first.y - radius, first.x + radius, first.y + radius);
            target->DrawTextW(number.c_str(), static_cast<UINT32>(number.size()),
                              format.Get(), textRect, white.Get());
            break;
        }
        case Tool::None:
        case Tool::Mosaic:
            break;
    }
}

ScreenImage RenderEngine::ComposeSelection(
    const ScreenImage& desktop, const RECT& selection,
    const AnnotationDocument& document,
    const std::optional<Annotation>& draft) {
    ScreenImage result = CropImage(desktop, selection);
    if (result.Empty()) {
        return result;
    }

    // 在内存 DIB 上建立 D2D DC 渲染目标，最终像素可直接复制回 ScreenImage。
    HDC dc = ::CreateCompatibleDC(nullptr);
    BITMAPINFO info = BitmapInfoFor(result);
    void* dibPixels = nullptr;
    HBITMAP bitmap = ::CreateDIBSection(dc, &info, DIB_RGB_COLORS,
                                        &dibPixels, nullptr, 0);
    if (!dc || !bitmap || !dibPixels) {
        if (bitmap) ::DeleteObject(bitmap);
        if (dc) ::DeleteDC(dc);
        return {};
    }
    const HGDIOBJ oldBitmap = ::SelectObject(dc, bitmap);
    std::memcpy(dibPixels, result.pixels.data(), result.pixels.size());

    ComPtr<ID2D1DCRenderTarget> target;
    D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    properties.dpiX = 96.0F;
    properties.dpiY = 96.0F;
    m_d2dFactory->CreateDCRenderTarget(&properties, &target);
    const RECT localBounds{0, 0, result.width, result.height};
    if (target) {
        target->BindDC(dc, &localBounds);
    }

    // 逐条应用而不是先统一马赛克，确保撤销/重做后的视觉顺序完全一致。
    auto renderOne = [&](const Annotation& annotation) {
        if (annotation.tool == Tool::Mosaic) {
            // 从前序标注的合成结果取样，再只覆盖当前笔刷轨迹。
            std::memcpy(result.pixels.data(), dibPixels, result.pixels.size());
            PaintMosaicStroke(result, annotation, {selection.left, selection.top});
            std::memcpy(dibPixels, result.pixels.data(), result.pixels.size());
            return;
        }
        if (!target) {
            return;
        }
        target->BeginDraw();
        target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        DrawAnnotation(target.Get(), annotation,
                       -static_cast<float>(selection.left),
                       -static_cast<float>(selection.top));
        target->EndDraw();
    };

    for (const Annotation& annotation : document.Items()) {
        renderOne(annotation);
    }
    if (draft) {
        renderOne(*draft);
    }

    std::memcpy(result.pixels.data(), dibPixels, result.pixels.size());
    ::SelectObject(dc, oldBitmap);
    ::DeleteObject(bitmap);
    ::DeleteDC(dc);
    return result;
}

void RenderEngine::DrawToolbar(ID2D1RenderTarget* target,
                               const std::vector<ToolbarButton>& buttons,
                               int hoveredButton) const {
    if (buttons.empty()) {
        return;
    }

    const ToolbarButton& first = buttons.front();
    const ToolbarButton& last = buttons.back();
    const D2D1_RECT_F barRect = D2D1::RectF(
        static_cast<float>(first.bounds.left - 7),
        static_cast<float>(first.bounds.top - 5),
        static_cast<float>(last.bounds.right + 7),
        static_cast<float>(last.bounds.bottom + 5));
    ComPtr<ID2D1SolidColorBrush> brush;
    target->CreateSolidColorBrush(D2D1::ColorF(0.14F, 0.14F, 0.14F, 0.98F), &brush);
    target->FillRoundedRectangle(D2D1::RoundedRect(barRect, 10.0F, 10.0F), brush.Get());

    for (std::size_t i = 0; i < buttons.size(); ++i) {
        const ToolbarButton& button = buttons[i];
        const D2D1_RECT_F rect = D2D1::RectF(
            static_cast<float>(button.bounds.left),
            static_cast<float>(button.bounds.top),
            static_cast<float>(button.bounds.right),
            static_cast<float>(button.bounds.bottom));
        const bool hovered = static_cast<int>(i) == hoveredButton;
        const bool styleButton = button.icon == ToolbarIcon::StrokeSize ||
                                 button.icon == ToolbarIcon::ColorSwatch;
        const D2D1_COLOR_F background = button.selected && !styleButton
            ? D2D1::ColorF(0.10F, 0.48F, 0.88F, 0.98F)
            : (button.selected && styleButton ? D2D1::ColorF(0.25F, 0.25F, 0.25F, 0.98F)
               : hovered ? D2D1::ColorF(0.30F, 0.30F, 0.30F, 0.98F)
                       : D2D1::ColorF(0.17F, 0.17F, 0.17F, 0.96F));
        if ((button.selected && button.icon != ToolbarIcon::StrokeSize) || hovered) {
            target->CreateSolidColorBrush(background, &brush);
            target->FillRoundedRectangle(D2D1::RoundedRect(rect, 6.0F, 6.0F), brush.Get());
        }

        D2D1_COLOR_F foreground = button.enabled
            ? D2D1::ColorF(0.94F, 0.94F, 0.94F, 1.0F)
            : D2D1::ColorF(0.48F, 0.48F, 0.48F, 1.0F);
        if (button.icon == ToolbarIcon::Cancel) {
            foreground = D2D1::ColorF(1.0F, 0.24F, 0.18F, 1.0F);
        } else if (button.icon == ToolbarIcon::Finish) {
            foreground = D2D1::ColorF(0.10F, 0.88F, 0.52F, 1.0F);
        }
        target->CreateSolidColorBrush(foreground, &brush);
        DrawToolbarIcon(target, button, brush.Get());
    }

    if (hoveredButton >= 0 && hoveredButton < static_cast<int>(buttons.size())) {
        DrawTooltip(target, buttons[hoveredButton]);
    }
}

void RenderEngine::DrawToolbarIcon(ID2D1RenderTarget* target,
                                   const ToolbarButton& button,
                                   ID2D1SolidColorBrush* brush) const {
    // 所有图标共用同一内容框和中心线，避免不同来源的符号发生视觉偏移。
    const float left = static_cast<float>(button.bounds.left + 12);
    const float right = static_cast<float>(button.bounds.right - 12);
    const float top = static_cast<float>(button.bounds.top + 13);
    const float bottom = static_cast<float>(button.bounds.bottom - 13);
    const float centerX = (left + right) * 0.5F;
    const float centerY = (top + bottom) * 0.5F;
    constexpr float stroke = 2.2F;

    auto line = [&](float x1, float y1, float x2, float y2) {
        target->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2),
                         brush, stroke);
    };
    auto centeredText = [&](const wchar_t* text) {
        m_iconTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_iconTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        target->DrawTextW(text, 1, m_iconTextFormat.Get(),
                          D2D1::RectF(left - 3, top - 5, right + 3, bottom + 5), brush);
    };
    switch (button.icon) {
        case ToolbarIcon::StrokeSize:
            // 圆点仅表示档位，选中使用绿色，与色块的颜色选择分开。
            brush->SetColor(button.selected ? D2D1::ColorF(0.0F, 0.72F, 0.43F)
                                           : D2D1::ColorF(0.38F, 0.38F, 0.38F));
            target->FillEllipse({D2D1::Point2F(centerX, centerY),
                                 button.radius, button.radius}, brush);
            break;
        case ToolbarIcon::ColorSwatch: {
            const auto swatch = D2D1::RoundedRect(D2D1::RectF(
                centerX - 15, centerY - 15, centerX + 15, centerY + 15), 2, 2);
            brush->SetColor(ToColor(button.color));
            target->FillRoundedRectangle(swatch, brush);
            if (button.color == RGB(255, 255, 255)) {
                brush->SetColor(D2D1::ColorF(0.5F, 0.5F, 0.5F));
                target->DrawRoundedRectangle(swatch, brush, 1.0F);
            }
            break;
        }
        case ToolbarIcon::Rectangle:
            target->DrawRoundedRectangle(D2D1::RoundedRect(
                D2D1::RectF(left, top, right, bottom), 3.0F, 3.0F), brush, stroke);
            break;
        case ToolbarIcon::Ellipse:
            target->DrawEllipse({D2D1::Point2F(centerX, centerY),
                                 (right - left) * 0.5F, (bottom - top) * 0.5F},
                                brush, stroke);
            break;
        case ToolbarIcon::Line:
            line(left + 1, bottom - 1, right - 1, top + 1);
            break;
        case ToolbarIcon::Arrow:
            line(left, bottom, right, top);
            line(right, top, right - 9, top + 1);
            line(right, top, right - 1, top + 9);
            break;
        case ToolbarIcon::Pen:
            DrawPenIcon(target, brush, left, right, top, bottom, stroke);
            break;
        case ToolbarIcon::Text:
            // 大写 A 使用三条线段绘制，尺寸占满内容框且不依赖字体。
            line(left + 1.0F, bottom + 1.0F, centerX, top - 1.0F);
            line(centerX, top - 1.0F, right - 1.0F, bottom + 1.0F);
            line(centerX - 6.0F, centerY + 3.0F, centerX + 6.0F, centerY + 3.0F);
            break;
        case ToolbarIcon::Counter:
            // 序号图标由圆环和线段数字 1 组成，避免不同字体造成字形偏移。
            target->DrawEllipse({D2D1::Point2F(centerX, centerY), 14.0F, 14.0F},
                                brush, stroke);
            line(centerX - 4.0F, centerY - 5.0F,
                 centerX, centerY - 9.0F);
            line(centerX, centerY - 9.0F,
                 centerX, centerY + 8.0F);
            line(centerX - 4.0F, centerY + 8.0F,
                 centerX + 4.0F, centerY + 8.0F);
            break;
        case ToolbarIcon::Mosaic: {
            // 四格铺满统一的图标内容框，使视觉尺寸与圆形、文字图标一致。
            const float size = (right - left) * 0.5F;
            target->FillRectangle(D2D1::RectF(centerX - size, centerY - size,
                                              centerX, centerY), brush);
            target->FillRectangle(D2D1::RectF(centerX, centerY,
                                              centerX + size, centerY + size), brush);
            target->DrawRectangle(D2D1::RectF(centerX, centerY - size,
                                              centerX + size, centerY), brush, 1.8F);
            target->DrawRectangle(D2D1::RectF(centerX - size, centerY,
                                              centerX, centerY + size), brush, 1.8F);
            break;
        }
        case ToolbarIcon::Ocr:
            // 扫描角标配合字母 T，在小尺寸下仍能表达文字识别。
            line(left, top + 6, left, top);
            line(left, top, left + 6, top);
            line(right - 6, top, right, top);
            line(right, top, right, top + 6);
            line(left, bottom - 6, left, bottom);
            line(left, bottom, left + 6, bottom);
            line(right - 6, bottom, right, bottom);
            line(right, bottom - 6, right, bottom);
            centeredText(L"T");
            break;
        case ToolbarIcon::Translate:
            // 小写意象为 A → 中，全部使用线段，不依赖图标字体和额外字体文件。
            line(left, top + 13, left + 5, top);
            line(left + 5, top, left + 10, top + 13);
            line(left + 2, top + 8, left + 8, top + 8);
            line(left + 13, top + 3, right, top + 3);
            line(right, top + 3, right - 4, top);
            line(right, top + 3, right - 4, top + 6);
            line(left, bottom - 3, left + 9, bottom - 3);
            line(left, bottom - 3, left + 4, bottom - 6);
            line(left, bottom - 3, left + 4, bottom);
            target->DrawRectangle(D2D1::RectF(left + 13, top + 12, right, bottom - 4),
                                  brush, stroke);
            line(left + 18, top + 9, left + 18, bottom + 1);
            break;
        case ToolbarIcon::Undo:
            DrawHistoryIcon(target, brush, left, right, top, bottom,
                            false, stroke);
            break;
        case ToolbarIcon::Redo:
            DrawHistoryIcon(target, brush, left, right, top, bottom,
                            true, stroke);
            break;
        case ToolbarIcon::Save:
            line(centerX, top - 2, centerX, bottom - 5);
            line(centerX, bottom - 5, centerX - 6, bottom - 11);
            line(centerX, bottom - 5, centerX + 6, bottom - 11);
            line(left, bottom + 2, right, bottom + 2);
            line(left, bottom + 2, left, bottom - 3);
            line(right, bottom + 2, right, bottom - 3);
            break;
        case ToolbarIcon::Pin:
            DrawPinIcon(target, brush, centerX, top, bottom, stroke);
            break;
        case ToolbarIcon::Cancel:
            line(left, top, right, bottom);
            line(right, top, left, bottom);
            break;
        case ToolbarIcon::Finish:
            line(left - 1, centerY, centerX - 2, bottom);
            line(centerX - 2, bottom, right + 2, top - 1);
            break;
    }
}

void RenderEngine::DrawTooltip(ID2D1RenderTarget* target,
                               const ToolbarButton& button) const {
    const float width = std::max(54.0F,
        static_cast<float>(button.tooltip.size()) * 16.0F + 20.0F);
    const float center = (button.bounds.left + button.bounds.right) * 0.5F;
    const float bottom = static_cast<float>(button.bounds.top - 8);
    const D2D1_RECT_F rect = D2D1::RectF(
        center - width * 0.5F, std::max(4.0F, bottom - 32.0F),
        center + width * 0.5F, bottom);

    ComPtr<ID2D1SolidColorBrush> brush;
    target->CreateSolidColorBrush(D2D1::ColorF(0.17F, 0.17F, 0.17F, 0.98F), &brush);
    target->FillRoundedRectangle(D2D1::RoundedRect(rect, 5.0F, 5.0F), brush.Get());
    target->CreateSolidColorBrush(D2D1::ColorF(0.88F, 0.88F, 0.88F, 1.0F), &brush);
    m_uiFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_uiFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    target->DrawTextW(button.tooltip.c_str(),
                      static_cast<UINT32>(button.tooltip.size()),
                      m_uiFormat.Get(), rect, brush.Get());
}

void RenderEngine::Paint(const ScreenImage& desktop, const RECT& selection,
                         const AnnotationDocument& document,
                         const std::optional<Annotation>& draft,
                         const std::vector<ToolbarButton>& buttons,
                         int hoveredButton,
                         const std::vector<ToolbarButton>& styleButtons,
                         int hoveredStyle,
                         const std::optional<PointF>& mosaicCursor, float mosaicRadius) {
    if (!EnsureWindowTarget()) {
        return;
    }
    if (!m_desktopBitmap) {
        m_desktopBitmap = CreateBitmap(m_windowTarget.Get(), desktop);
    }

    // 每帧先还原桌面，再用 90% 透明的黑色遮罩轻微压暗非选区。
    // Direct2D 的 Alpha 表示不透明度，因此 90% 透明对应 0.10F。
    m_windowTarget->BeginDraw();
    m_windowTarget->SetTransform(D2D1::Matrix3x2F::Identity());
    if (m_desktopBitmap) {
        m_windowTarget->DrawBitmap(m_desktopBitmap.Get());
    }

    RECT client{};
    ::GetClientRect(m_window, &client);
    ComPtr<ID2D1SolidColorBrush> shade;
    // 修改透明度0.15F
    m_windowTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.15F), &shade);
    const bool hasSelection = selection.right > selection.left && selection.bottom > selection.top;
    if (!hasSelection) {
        m_windowTarget->FillRectangle(
            D2D1::RectF(0, 0, static_cast<float>(client.right),
                        static_cast<float>(client.bottom)),
            shade.Get());
    } else {
        m_windowTarget->FillRectangle(
            D2D1::RectF(0, 0, static_cast<float>(client.right),
                        static_cast<float>(selection.top)),
            shade.Get());
        m_windowTarget->FillRectangle(
            D2D1::RectF(0, static_cast<float>(selection.bottom),
                        static_cast<float>(client.right),
                        static_cast<float>(client.bottom)),
            shade.Get());
        m_windowTarget->FillRectangle(
            D2D1::RectF(0, static_cast<float>(selection.top),
                        static_cast<float>(selection.left),
                        static_cast<float>(selection.bottom)),
            shade.Get());
        m_windowTarget->FillRectangle(
            D2D1::RectF(static_cast<float>(selection.right),
                        static_cast<float>(selection.top),
                        static_cast<float>(client.right),
                        static_cast<float>(selection.bottom)),
            shade.Get());

        ScreenImage composed = ComposeSelection(desktop, selection, document, draft);
        ComPtr<ID2D1Bitmap> selectedBitmap = CreateBitmap(m_windowTarget.Get(), composed);
        if (selectedBitmap) {
            m_windowTarget->DrawBitmap(selectedBitmap.Get(), D2D1::RectF(
                static_cast<float>(selection.left), static_cast<float>(selection.top),
                static_cast<float>(selection.right), static_cast<float>(selection.bottom)));
        }
        ComPtr<ID2D1SolidColorBrush> border;
        m_windowTarget->CreateSolidColorBrush(D2D1::ColorF(0.05F, 0.50F, 1.0F), &border);
        m_windowTarget->DrawRectangle(D2D1::RectF(
            static_cast<float>(selection.left), static_cast<float>(selection.top),
            static_cast<float>(selection.right), static_cast<float>(selection.bottom)),
            border.Get(), 2.0F);
    }

    // 光标仅画在窗口层，不进入 ComposeSelection，因此保存/复制不会带上圆圈。
    if (mosaicCursor) {
        m_windowTarget->PushAxisAlignedClip(D2D1::RectF(
            static_cast<float>(selection.left), static_cast<float>(selection.top),
            static_cast<float>(selection.right), static_cast<float>(selection.bottom)),
            D2D1_ANTIALIAS_MODE_ALIASED);
        const D2D1_ELLIPSE circle{ToPoint(*mosaicCursor, 0, 0), mosaicRadius, mosaicRadius};
        // 黑白双描边在亮图和暗图上都能辨认，不遮住笔刷中心内容。
        shade->SetColor(D2D1::ColorF(0.0F, 0.0F, 0.0F));
        m_windowTarget->DrawEllipse(circle, shade.Get(), 3.0F);
        shade->SetColor(D2D1::ColorF(1.0F, 1.0F, 1.0F));
        m_windowTarget->DrawEllipse(circle, shade.Get(), 1.0F);
        m_windowTarget->PopAxisAlignedClip();
    }
    DrawToolbar(m_windowTarget.Get(), buttons, hoveredButton);
    DrawToolbar(m_windowTarget.Get(), styleButtons, hoveredStyle);
    if (styleButtons.size() == 9) {
        // 三档粗细与六个色块之间的分隔线。
        const auto& edge = styleButtons[2].bounds;
        shade->SetColor(D2D1::ColorF(0.23F, 0.23F, 0.23F));
        m_windowTarget->DrawLine(
            D2D1::Point2F(static_cast<float>(edge.right + 8), static_cast<float>(edge.top + 10)),
            D2D1::Point2F(static_cast<float>(edge.right + 8), static_cast<float>(edge.bottom - 10)),
            shade.Get(), 1.0F);
    }
    if (m_windowTarget->EndDraw() == D2DERR_RECREATE_TARGET) {
        DiscardWindowResources();
    }
}

}  // namespace native_shot
