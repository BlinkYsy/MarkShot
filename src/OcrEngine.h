#pragma once

#include "ScreenImage.h"

#include <string>

namespace native_shot {

// OCR 引擎的单一输出：成功时返回文字和实际语言，失败时返回可显示的原因。
struct OcrTextResult {
    std::wstring text;
    std::wstring language;
    std::wstring error;

    [[nodiscard]] bool Succeeded() const noexcept {
        return !text.empty() && error.empty();
    }
};

// 识别 BGRA32：普通 OCR 使用 Windows；英文翻译使用随软件附带的 Tesseract。
[[nodiscard]] OcrTextResult RecognizeText(const ScreenImage& image, bool englishOnly = false);

// 英文引擎内部入口；图像布局由 RecognizeText 统一检查。
[[nodiscard]] OcrTextResult RecognizeEnglishWithTesseract(const ScreenImage& image);

}  // namespace native_shot
