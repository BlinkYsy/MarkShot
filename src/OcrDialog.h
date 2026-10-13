#pragma once

#include "OcrEngine.h"

#include <windows.h>

namespace native_shot {

// 显示 OCR 结果；复制或关闭后结束本次截图会话。
void ShowOcrResultDialog(HWND owner, const OcrTextResult& result);

// 后台识别英文并请求百度通用翻译，弹窗显示中文，可展开核对英文原文。
void ShowTranslationDialog(const ScreenImage& image);

}  // namespace native_shot
