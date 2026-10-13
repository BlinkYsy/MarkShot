#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>
#include "AnnotationDocument.h"

namespace native_shot {

// 像素固定为自顶向下 BGRA32。统一内存布局可避免截图、Direct2D、WIC 和
// 剪贴板之间反复转换。
struct ScreenImage {
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<std::uint8_t> pixels;

    [[nodiscard]] bool Empty() const noexcept {
        return width <= 0 || height <= 0 || pixels.empty();
    }
};

struct DesktopCapture {
    ScreenImage image;
    // image 左上角对应的虚拟桌面坐标；多显示器布局中可能为负数。
    POINT origin{};
};

// 一次性抓取整个虚拟桌面，返回物理像素图像及其桌面原点。
[[nodiscard]] DesktopCapture CaptureVirtualDesktop();
// sourceRect 使用 source 内部坐标；越界部分会被裁剪。
[[nodiscard]] ScreenImage CropImage(const ScreenImage& source, const RECT& sourceRect);
// 在给定区域内按块取平均色，直接修改图像以生成马赛克。
void Pixelate(ScreenImage& image, const RECT& rect, int blockSize = 12);
// 对轨迹覆盖区域打码；插值补齐快速拖动间隙，整笔使用同一份像素源。
void PaintMosaicStroke(ScreenImage& image, const Annotation& stroke, POINT origin);

// WIC 保存 PNG；调用线程必须已初始化 COM。
[[nodiscard]] bool SavePng(const ScreenImage& image, const std::wstring& filename);
// 以 CF_DIB 写入剪贴板。成功后像素副本的所有权移交给系统。
[[nodiscard]] bool CopyToClipboard(HWND owner, const ScreenImage& image);

}  // namespace native_shot
