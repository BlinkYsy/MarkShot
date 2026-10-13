#include "ScreenImage.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace native_shot {
namespace {

using Microsoft::WRL::ComPtr;

BITMAPINFO TopDownBitmapInfo(int width, int height) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    // 负高度约定首行就是图像顶部，与 ScreenImage 的内存顺序一致。
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    return info;
}

RECT ClampRect(const RECT& rect, int width, int height) {
    RECT result = rect;
    result.left = std::clamp(result.left, 0L, static_cast<LONG>(width));
    result.right = std::clamp(result.right, 0L, static_cast<LONG>(width));
    result.top = std::clamp(result.top, 0L, static_cast<LONG>(height));
    result.bottom = std::clamp(result.bottom, 0L, static_cast<LONG>(height));
    return result;
}

}  // namespace

DesktopCapture CaptureVirtualDesktop() {
    DesktopCapture capture;
    capture.origin = {
        ::GetSystemMetrics(SM_XVIRTUALSCREEN),
        ::GetSystemMetrics(SM_YVIRTUALSCREEN),
    };

    ScreenImage& image = capture.image;
    image.width = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    image.height = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    image.stride = image.width * 4;
    if (image.width <= 0 || image.height <= 0) {
        return {};
    }

    // DIBSection 提供可直接复制的连续 BGRA 缓冲区，避免再调用 GetDIBits。
    HDC screenDc = ::GetDC(nullptr);
    HDC memoryDc = screenDc ? ::CreateCompatibleDC(screenDc) : nullptr;
    BITMAPINFO info = TopDownBitmapInfo(image.width, image.height);
    void* dibPixels = nullptr;
    HBITMAP bitmap = memoryDc
        ? ::CreateDIBSection(memoryDc, &info, DIB_RGB_COLORS, &dibPixels, nullptr, 0)
        : nullptr;
    if (!screenDc || !memoryDc || !bitmap || !dibPixels) {
        if (bitmap) ::DeleteObject(bitmap);
        if (memoryDc) ::DeleteDC(memoryDc);
        if (screenDc) ::ReleaseDC(nullptr, screenDc);
        return {};
    }

    const HGDIOBJ oldBitmap = ::SelectObject(memoryDc, bitmap);
    // CAPTUREBLT 让分层窗口也进入截图，行为更接近用户看到的桌面。
    const BOOL copied = ::BitBlt(memoryDc, 0, 0, image.width, image.height,
                                 screenDc, capture.origin.x, capture.origin.y,
                                 SRCCOPY | CAPTUREBLT);
    if (copied) {
        const std::size_t byteCount = static_cast<std::size_t>(image.stride) * image.height;
        image.pixels.resize(byteCount);
        std::memcpy(image.pixels.data(), dibPixels, byteCount);
    }

    ::SelectObject(memoryDc, oldBitmap);
    ::DeleteObject(bitmap);
    ::DeleteDC(memoryDc);
    ::ReleaseDC(nullptr, screenDc);
    return copied ? std::move(capture) : DesktopCapture{};
}

ScreenImage CropImage(const ScreenImage& source, const RECT& sourceRect) {
    const RECT rect = ClampRect(sourceRect, source.width, source.height);
    ScreenImage result;
    result.width = std::max(0L, rect.right - rect.left);
    result.height = std::max(0L, rect.bottom - rect.top);
    result.stride = result.width * 4;
    if (source.Empty() || result.width == 0 || result.height == 0) {
        return result;
    }

    result.pixels.resize(static_cast<std::size_t>(result.stride) * result.height);
    for (int y = 0; y < result.height; ++y) {
        const auto* input = source.pixels.data()
            + static_cast<std::size_t>(rect.top + y) * source.stride
            + static_cast<std::size_t>(rect.left) * 4;
        auto* output = result.pixels.data() + static_cast<std::size_t>(y) * result.stride;
        std::memcpy(output, input, static_cast<std::size_t>(result.stride));
    }
    return result;
}

void Pixelate(ScreenImage& image, const RECT& requestedRect, int blockSize) {
    if (image.Empty()) {
        return;
    }
    const RECT rect = ClampRect(requestedRect, image.width, image.height);
    blockSize = std::max(blockSize, 2);

    for (LONG top = rect.top; top < rect.bottom; top += blockSize) {
        for (LONG left = rect.left; left < rect.right; left += blockSize) {
            const LONG bottom = std::min<LONG>(top + blockSize, rect.bottom);
            const LONG right = std::min<LONG>(left + blockSize, rect.right);
            std::uint64_t blue = 0, green = 0, red = 0;
            std::uint64_t count = 0;

            for (LONG y = top; y < bottom; ++y) {
                for (LONG x = left; x < right; ++x) {
                    const auto* pixel = image.pixels.data()
                        + static_cast<std::size_t>(y) * image.stride
                        + static_cast<std::size_t>(x) * 4;
                    blue += pixel[0];
                    green += pixel[1];
                    red += pixel[2];
                    ++count;
                }
            }

            const std::uint8_t b = static_cast<std::uint8_t>(blue / count);
            const std::uint8_t g = static_cast<std::uint8_t>(green / count);
            const std::uint8_t r = static_cast<std::uint8_t>(red / count);
            for (LONG y = top; y < bottom; ++y) {
                for (LONG x = left; x < right; ++x) {
                    auto* pixel = image.pixels.data()
                        + static_cast<std::size_t>(y) * image.stride
                        + static_cast<std::size_t>(x) * 4;
                    pixel[0] = b;
                    pixel[1] = g;
                    pixel[2] = r;
                    pixel[3] = 255;
                }
            }
        }
    }
}

void PaintMosaicStroke(ScreenImage& image, const Annotation& stroke, POINT origin) {
    if (image.Empty() || stroke.points.empty() || stroke.thickness <= 0) return;
    ScreenImage mosaic = image;
    Pixelate(mosaic, {0, 0, image.width, image.height});
    const int size = std::max(2, static_cast<int>(stroke.thickness));
    // 按像素中心裁剪圆形笔刷，实际覆盖范围与窗口中的圆形轮廓一致。
    const float radius = stroke.thickness * 0.5F;
    const auto stamp = [&](PointF point) {
        const float cx = point.x - origin.x;
        const float cy = point.y - origin.y;
        const RECT rect = ClampRect({static_cast<LONG>(std::floor(cx - radius)),
            static_cast<LONG>(std::floor(cy - radius)),
            static_cast<LONG>(std::ceil(cx + radius)),
            static_cast<LONG>(std::ceil(cy + radius))}, image.width, image.height);
        for (LONG y = rect.top; y < rect.bottom; ++y) {
            for (LONG x = rect.left; x < rect.right; ++x) {
                const float dx = x + 0.5F - cx;
                const float dy = y + 0.5F - cy;
                if (dx * dx + dy * dy > radius * radius) continue;
                const auto offset = static_cast<std::size_t>(y) * image.stride + x * 4;
                std::memcpy(image.pixels.data() + offset, mosaic.pixels.data() + offset, 4);
            }
        }
    };
    stamp(stroke.points.front());
    for (std::size_t i = 1; i < stroke.points.size(); ++i) {
        const PointF first = stroke.points[i - 1];
        const PointF last = stroke.points[i];
        const float distance = std::max(std::abs(last.x - first.x), std::abs(last.y - first.y));
        const int steps = std::max(1, static_cast<int>(distance / std::max(1, size / 4)) + 1);
        for (int step = 1; step <= steps; ++step) {
            const float t = static_cast<float>(step) / steps;
            stamp({first.x + (last.x - first.x) * t, first.y + (last.y - first.y) * t});
        }
    }
}

bool SavePng(const ScreenImage& image, const std::wstring& filename) {
    if (image.Empty() || filename.empty()) {
        return false;
    }

    // ComPtr 负责释放整条 WIC 编码链，任一步失败都可直接返回。
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(filename.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, &properties)) ||
        FAILED(frame->Initialize(properties.Get())) ||
        FAILED(frame->SetSize(image.width, image.height))) {
        return false;
    }

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format)) ||
        format != GUID_WICPixelFormat32bppBGRA) {
        return false;
    }

    const UINT byteCount = static_cast<UINT>(image.pixels.size());
    return SUCCEEDED(frame->WritePixels(image.height, image.stride, byteCount,
                                        const_cast<BYTE*>(image.pixels.data()))) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

bool CopyToClipboard(HWND owner, const ScreenImage& image) {
    if (image.Empty() || !::OpenClipboard(owner)) {
        return false;
    }
    ::EmptyClipboard();

    const std::size_t pixelBytes = static_cast<std::size_t>(image.stride) * image.height;
    const std::size_t totalBytes = sizeof(BITMAPINFOHEADER) + pixelBytes;
    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, totalBytes);
    if (!memory) {
        ::CloseClipboard();
        return false;
    }

    auto* destination = static_cast<std::uint8_t*>(::GlobalLock(memory));
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = image.width;
    header.biHeight = image.height;  // CF_DIB 使用自底向上行序。
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    std::memcpy(destination, &header, sizeof(header));
    destination += sizeof(header);

    // ScreenImage 自顶向下，而传统 CF_DIB 要求正高度、自底向上排列。
    for (int y = 0; y < image.height; ++y) {
        const auto* sourceRow = image.pixels.data()
            + static_cast<std::size_t>(image.height - 1 - y) * image.stride;
        std::memcpy(destination + static_cast<std::size_t>(y) * image.stride,
                    sourceRow, image.stride);
    }
    ::GlobalUnlock(memory);

    const bool success = ::SetClipboardData(CF_DIB, memory) != nullptr;
    // SetClipboardData 成功后由系统释放 memory；失败时仍由本函数负责。
    if (!success) {
        ::GlobalFree(memory);
    }
    ::CloseClipboard();
    return success;
}

}  // namespace native_shot
