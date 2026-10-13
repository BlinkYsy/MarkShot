#pragma once

#include "OcrEngine.h"

namespace native_shot {

// 只发送识别出的英文文字，不上传截图；凭据从 EXE 旁的 MarkShot.translation.ini 读取。
[[nodiscard]] OcrTextResult TranslateEnglishToChinese(const std::wstring& text);

}  // namespace native_shot
