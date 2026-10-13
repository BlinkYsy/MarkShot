#include "OcrEngine.h"

#include <wincodec.h>
#include <wrl/client.h>
#include <MemoryBuffer.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <limits>
#include <thread>

namespace native_shot {
namespace {

using Microsoft::WRL::ComPtr;
using winrt::Windows::Graphics::Imaging::BitmapAlphaMode;
using winrt::Windows::Graphics::Imaging::BitmapBufferAccessMode;
using winrt::Windows::Graphics::Imaging::BitmapPixelFormat;
using winrt::Windows::Graphics::Imaging::SoftwareBitmap;
using WindowsOcrEngine = winrt::Windows::Media::Ocr::OcrEngine;

std::wstring Trim(std::wstring text) {
    const auto first = std::find_if_not(text.begin(), text.end(), iswspace);
    const auto last = std::find_if_not(text.rbegin(), text.rend(), iswspace).base();
    if (first >= last) {
        return {};
    }
    return std::wstring(first, last);
}

SoftwareBitmap CreateSoftwareBitmap(const ScreenImage& image, unsigned maxDimension) {
    const int largestDimension = std::max(image.width, image.height);
    const double scale = largestDimension > static_cast<int>(maxDimension)
        ? static_cast<double>(maxDimension) / largestDimension
        : 1.0;
    const int outputWidth = std::max(1, static_cast<int>(std::lround(image.width * scale)));
    const int outputHeight = std::max(1, static_cast<int>(std::lround(image.height * scale)));

    SoftwareBitmap bitmap(BitmapPixelFormat::Bgra8, outputWidth, outputHeight,
                          BitmapAlphaMode::Ignore);
    auto bitmapBuffer = bitmap.LockBuffer(BitmapBufferAccessMode::Write);
    const auto plane = bitmapBuffer.GetPlaneDescription(0);
    auto reference = bitmapBuffer.CreateReference();

    ComPtr<::Windows::Foundation::IMemoryBufferByteAccess> byteAccess;
    winrt::check_hresult(reinterpret_cast<IUnknown*>(winrt::get_abi(reference))
                             ->QueryInterface(IID_PPV_ARGS(&byteAccess)));
    BYTE* outputPixels = nullptr;
    UINT32 outputCapacity = 0;
    winrt::check_hresult(byteAccess->GetBuffer(&outputPixels, &outputCapacity));
    outputPixels += plane.StartIndex;

    const auto sourceByteCount = static_cast<std::size_t>(image.stride) * image.height;
    if (sourceByteCount > std::numeric_limits<UINT>::max() ||
        plane.Stride <= 0 ||
        static_cast<std::uint64_t>(plane.StartIndex) +
                static_cast<std::uint64_t>(plane.Stride) * outputHeight > outputCapacity) {
        throw winrt::hresult_invalid_argument();
    }

    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmap> sourceBitmap;
    winrt::check_hresult(::CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory)));
    winrt::check_hresult(factory->CreateBitmapFromMemory(
        image.width, image.height, GUID_WICPixelFormat32bppBGRA,
        image.stride, static_cast<UINT>(sourceByteCount),
        const_cast<BYTE*>(image.pixels.data()), &sourceBitmap));

    IWICBitmapSource* pixelsToCopy = sourceBitmap.Get();
    ComPtr<IWICBitmapScaler> scaler;
    if (outputWidth != image.width || outputHeight != image.height) {
        winrt::check_hresult(factory->CreateBitmapScaler(&scaler));
        winrt::check_hresult(scaler->Initialize(
            sourceBitmap.Get(), outputWidth, outputHeight,
            WICBitmapInterpolationModeFant));
        pixelsToCopy = scaler.Get();
    }
    winrt::check_hresult(pixelsToCopy->CopyPixels(
        nullptr, plane.Stride,
        outputCapacity - static_cast<UINT32>(plane.StartIndex), outputPixels));
    return bitmap;
}

OcrTextResult RecognizeOnWorker(const ScreenImage& image) {
    OcrTextResult output;
    bool apartmentInitialized = false;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartmentInitialized = true;

        WindowsOcrEngine engine = WindowsOcrEngine::TryCreateFromUserProfileLanguages();
        if (!engine) {
            const auto languages = WindowsOcrEngine::AvailableRecognizerLanguages();
            if (languages.Size() > 0) {
                engine = WindowsOcrEngine::TryCreateFromLanguage(languages.GetAt(0));
            }
        }
        if (!engine) {
            output.error = L"未找到可用的 Windows OCR 语言，请在系统语言设置中安装语言包。";
        } else {
            const SoftwareBitmap bitmap = CreateSoftwareBitmap(
                image, WindowsOcrEngine::MaxImageDimension());
            const auto result = engine.RecognizeAsync(bitmap).get();
            output.text = Trim(result.Text().c_str());
            output.language = engine.RecognizerLanguage().DisplayName().c_str();
            if (output.text.empty()) {
                output.error = L"未在选区中识别到文字。";
            }
        }
    } catch (const winrt::hresult_error& error) {
        output.error = L"Windows OCR 失败：" + std::wstring(error.message().c_str());
    } catch (...) {
        output.error = L"Windows OCR 失败：未知错误。";
    }
    if (apartmentInitialized) {
        winrt::uninit_apartment();
    }
    return output;
}

}  // namespace

OcrTextResult RecognizeText(const ScreenImage& image, bool englishOnly) {
    if (image.Empty()) {
        return {{}, {}, L"OCR 选区为空。"};
    }
    // 两个后端都必须在读取像素前拒绝截断数据或无效行跨度。
    if (image.width > std::numeric_limits<int>::max() / 4 ||
        image.stride < image.width * 4 ||
        static_cast<std::uint64_t>(image.stride) * image.height > image.pixels.size()) {
        return {{}, {}, L"OCR 图像数据不完整或行跨度无效。"};
    }
    if (englishOnly) return RecognizeEnglishWithTesseract(image);

    OcrTextResult output;
    // 主线程是 Win32 STA；OCR 的异步操作在临时 MTA 线程上同步收口。
    std::thread worker([&] { output = RecognizeOnWorker(image); });
    worker.join();
    return output;
}

}  // namespace native_shot
