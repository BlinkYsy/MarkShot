#include "AnnotationDocument.h"
#include "OcrEngine.h"
#include "ScreenImage.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <objbase.h>

int main() {
    using namespace native_shot;

    // 六个工具的所有档位均写入标注；历史应保留提交时的样式副本。
    AnnotationStyle style;
    for (Tool tool : {Tool::Rectangle, Tool::Ellipse, Tool::Line,
                      Tool::Arrow, Tool::Pen, Tool::Text}) {
        assert(AnnotationStyle::Supports(tool));
        for (int size = 0; size < 3; ++size) {
            for (int color = 0; color < 6; ++color) {
                style.sizeIndex = size;
                style.colorIndex = color;
                Annotation annotation;
                annotation.tool = tool;
                style.Apply(annotation);
                assert(annotation.thickness == AnnotationStyle::widths[size]);
                assert(annotation.fontSize == AnnotationStyle::fontSizes[size]);
                assert(annotation.color == AnnotationStyle::colors[color]);
                AnnotationDocument history;
                history.Add(annotation);
                assert(history.Undo());
                assert(history.Redo());
                assert(history.Items().front().color == annotation.color);
                assert(history.Items().front().fontSize == annotation.fontSize);
                assert(history.Items().front().thickness == annotation.thickness);
            }
        }
    }
    for (Tool tool : {Tool::None, Tool::Counter, Tool::Mosaic}) {
        assert(!AnnotationStyle::Supports(tool));
    }

    AnnotationDocument document;
    // 只有两个远离的采样点也必须连续打码，轨迹外像素保持不变。
    ScreenImage brushImage;
    brushImage.width = 96;
    brushImage.height = 64;
    brushImage.stride = 96 * 4;
    brushImage.pixels.resize(96 * 64 * 4, 255);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 96; ++x) {
            brushImage.pixels[y * brushImage.stride + x * 4] = (x % 2) ? 255 : 0;
        }
    }
    const ScreenImage original = brushImage;
    // 三档圆形笔刷：圆内均打码，外接矩形四角保持原图，防止退回方形笔刷。
    for (int diameter : {16, 32, 48}) {
        ScreenImage circular = original;
        Annotation dot;
        dot.tool = Tool::Mosaic;
        dot.thickness = static_cast<float>(diameter);
        dot.points = {{148.25F, 232.25F}};
        PaintMosaicStroke(circular, dot, {100, 200});
        const float radius = diameter * 0.5F;
        for (int y = 0; y < circular.height; ++y) {
            for (int x = 0; x < circular.width; ++x) {
                const float dx = x + 0.5F - 48.25F;
                const float dy = y + 0.5F - 32.25F;
                const auto offset = y * circular.stride + x * 4;
                const auto expected = dx * dx + dy * dy <= radius * radius
                    ? 127 : original.pixels[offset];
                assert(circular.pixels[offset] == expected);
            }
        }
    }
    Annotation mosaic;
    mosaic.tool = Tool::Mosaic;
    mosaic.thickness = 16;
    mosaic.points = {{110, 232}, {186, 232}};
    PaintMosaicStroke(brushImage, mosaic, {100, 200});
    for (int x = 10; x <= 86; ++x) {
        assert(brushImage.pixels[32 * brushImage.stride + x * 4] == 127);
        assert(brushImage.pixels[4 * brushImage.stride + x * 4] == original.pixels[4 * original.stride + x * 4]);
    }
    // 单击也能打码；触及选区边缘时裁剪，越界轨迹不得写出缓冲区。
    mosaic.points = {{100, 200}};
    PaintMosaicStroke(brushImage, mosaic, {100, 200});
    assert(brushImage.pixels[0] == 127);
    AnnotationDocument mosaicHistory;
    mosaicHistory.Add(mosaic);
    assert(mosaicHistory.Undo());
    assert(mosaicHistory.Items().empty());
    assert(mosaicHistory.Redo());
    assert(mosaicHistory.Items().front().points.size() == 1);
    assert(mosaicHistory.Items().front().thickness == 16);
    mosaic.points = {{-100, -100}};
    PaintMosaicStroke(brushImage, mosaic, {100, 200});

    Annotation line;
    line.tool = Tool::Line;
    line.points = {{10.0F, 20.0F}, {50.0F, 60.0F}};
    document.Add(line);

    assert(document.CanUndo());
    assert(document.Items().size() == 1);
    assert(document.Undo());
    assert(document.Items().empty());
    assert(document.CanRedo());
    assert(document.Redo());
    assert(document.Items().size() == 1);

    const RECT bounds = BoundsOf(document.Items().front());
    assert(bounds.left == 10 && bounds.top == 20);
    assert(bounds.right == 50 && bounds.bottom == 60);

    // 用一个 2x2 图像验证 WIC 编码链，避免“能编译但保存按钮不可用”。
    assert(SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)));
    ScreenImage image;
    image.width = 2;
    image.height = 2;
    image.stride = 8;
    image.pixels = {
        0, 0, 255, 255, 0, 255, 0, 255,
        255, 0, 0, 255, 255, 255, 255, 255,
    };
    const auto output = std::filesystem::temp_directory_path() /
                        L"native_shot_wic_test.png";
    assert(SavePng(image, output.wstring()));
    assert(std::filesystem::file_size(output) > 8);
    std::filesystem::remove(output);

    // 空选区必须在进入 WinRT 前稳定失败，保持 OCR 模块的输入契约。
    const OcrTextResult emptyOcr = RecognizeText({});
    assert(!emptyOcr.Succeeded());
    assert(!emptyOcr.error.empty());

    ::CoUninitialize();
    return 0;
}
