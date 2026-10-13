#include "OcrEngine.h"

#include <cassert>
#include <cstring>
#include <iostream>

namespace {

native_shot::ScreenImage CreateTextImage() {
    constexpr int width = 900;
    constexpr int height = 220;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    HDC screen = ::GetDC(nullptr);
    HDC dc = ::CreateCompatibleDC(screen);
    void* pixels = nullptr;
    HBITMAP bitmap = ::CreateDIBSection(
        dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    const HGDIOBJ previousBitmap = ::SelectObject(dc, bitmap);
    RECT bounds{0, 0, width, height};
    ::FillRect(dc, &bounds, static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH)));
    HFONT font = ::CreateFontW(
        -88, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Arial");
    const HGDIOBJ previousFont = ::SelectObject(dc, font);
    ::SetTextColor(dc, RGB(0, 0, 0));
    ::SetBkMode(dc, TRANSPARENT);
    ::DrawTextW(dc, L"MARKSHOT OCR 123", -1, &bounds,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    native_shot::ScreenImage image;
    image.width = width;
    image.height = height;
    image.stride = width * 4;
    image.pixels.resize(static_cast<std::size_t>(image.stride) * height);
    std::memcpy(image.pixels.data(), pixels, image.pixels.size());

    ::SelectObject(dc, previousFont);
    ::DeleteObject(font);
    ::SelectObject(dc, previousBitmap);
    ::DeleteObject(bitmap);
    ::DeleteDC(dc);
    ::ReleaseDC(nullptr, screen);
    return image;
}

}  // namespace

int main(int argc, char** argv) {
    // 英译中必须在没有 Windows 英文 OCR 的电脑上识别成功，不能把缺依赖算作通过。
    const auto english = native_shot::RecognizeText(CreateTextImage(), true);
    // 可在不带 ocr 文件夹的临时部署目录运行，确认缺文件只报错、不崩溃。
    if (argc == 2 && std::strcmp(argv[1], "--expect-missing-model") == 0) {
        return !english.Succeeded() &&
            english.error.find(L"缺少 Tesseract 英文模型") != std::wstring::npos ? 0 : 1;
    }
    if (!english.Succeeded() || english.language != L"English (Tesseract)" ||
        english.text.find(L"MARKSHOT OCR 123") == std::wstring::npos) {
        std::wcerr << L"Tesseract English OCR failed: " << english.error
                   << L" text=" << english.text << L'\n';
        return 1;
    }
    auto invalid = CreateTextImage();
    invalid.pixels.resize(1);
    if (native_shot::RecognizeText(invalid, true).Succeeded()) return 1;
    std::cout << "Tesseract English OCR passed\n";
    const native_shot::OcrTextResult result =
        native_shot::RecognizeText(CreateTextImage());
    if (!result.Succeeded()) {
        std::wcerr << result.error << L'\n';
        // 77 由 CTest 解释为系统未安装 OCR 语言时的跳过，而非代码失败。
        return result.error.find(L"未找到可用的 Windows OCR 语言") != std::wstring::npos
            ? 77
            : 1;
    }
    std::wcout << L"language=" << result.language
               << L" text=" << result.text << L'\n';
    assert(!result.text.empty());
    assert(result.text.find(L"123") != std::wstring::npos);
    return 0;
}
