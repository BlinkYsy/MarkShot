#include "OcrDialog.h"
#include "TranslationEngine.h"

#include <commctrl.h>
#include <winrt/base.h>

#include <cstring>
#include <future>
#include <string>

namespace native_shot {
namespace {

constexpr int kCopyButtonId = 1001;

bool CopyText(HWND owner, const std::wstring& text) {
    if (text.empty() || !::OpenClipboard(owner)) {
        return false;
    }
    const std::size_t byteCount = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, byteCount);
    if (!memory) {
        ::CloseClipboard();
        return false;
    }
    void* destination = ::GlobalLock(memory);
    if (!destination) {
        ::GlobalFree(memory);
        ::CloseClipboard();
        return false;
    }
    std::memcpy(destination, text.c_str(), byteCount);
    ::GlobalUnlock(memory);
    const bool success = ::EmptyClipboard() &&
                         ::SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    if (!success) {
        ::GlobalFree(memory);
    }
    ::CloseClipboard();
    return success;
}

HRESULT CALLBACK OcrDialogCallback(HWND dialog, UINT notification,
                                   WPARAM wParam, LPARAM, LONG_PTR data) {
    if (notification != TDN_BUTTON_CLICKED || wParam != kCopyButtonId) {
        return S_OK;
    }
    const auto* text = reinterpret_cast<const std::wstring*>(data);
    const bool copied = CopyText(dialog, *text);
    if (!copied) {
        ::MessageBoxW(dialog, L"复制失败，请稍后重试。", L"MarkShot",
                      MB_OK | MB_ICONWARNING);
        return S_FALSE;
    }
    // 返回 S_OK 让 Task Dialog 关闭；调用方随后结束本次截图会话。
    return S_OK;
}

}  // namespace

// OCR 与翻译共用复制/关闭行为，仅替换标题、提示和可展开的英文原文。
void ShowTextResultDialog(HWND owner, const OcrTextResult& result,
                          bool translated, const std::wstring& original) {
    const wchar_t* title = translated ? L"MarkShot - 英文译中文" : L"MarkShot - 文字识别";
    if (!result.Succeeded()) {
        ::MessageBoxW(owner, result.error.c_str(), title,
                      MB_OK | MB_ICONWARNING);
        return;
    }

    const std::wstring instruction = translated ? L"翻译完成（英文 → 简体中文）" : result.language.empty()
        ? L"识别完成"
        : L"识别完成（" + result.language + L"）";
    const TASKDIALOG_BUTTON buttons[] = {
        {kCopyButtonId, translated ? L"复制译文" : L"复制文字"},
        {IDCLOSE, L"关闭"},
    };
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = owner;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    config.pszWindowTitle = title;
    config.pszMainInstruction = instruction.c_str();
    config.pszContent = result.text.c_str();
    const std::wstring footer = translated
        ? result.language + L"；复制按钮仅复制中文译文。" : L"可复制全部识别文字。";
    config.pszFooter = footer.c_str();
    if (translated) {
        config.pszExpandedInformation = original.c_str();
        config.pszCollapsedControlText = L"查看英文识别原文";
        config.pszExpandedControlText = L"收起英文原文";
    }
    config.cButtons = static_cast<UINT>(std::size(buttons));
    config.pButtons = buttons;
    config.nDefaultButton = kCopyButtonId;
    config.pfCallback = OcrDialogCallback;
    config.lpCallbackData = reinterpret_cast<LONG_PTR>(&result.text);

    if (FAILED(::TaskDialogIndirect(&config, nullptr, nullptr, nullptr))) {
        // 极旧系统缺少 Task Dialog 时仍然保留可用的复制路径。
        const bool copied = CopyText(owner, result.text);
        ::MessageBoxW(owner, copied ? L"结果已复制到剪贴板。" : L"复制失败，请重试。",
                      title, MB_OK | (copied ? MB_ICONINFORMATION : MB_ICONWARNING));
    }
}

void ShowOcrResultDialog(HWND owner, const OcrTextResult& result) {
    ShowTextResultDialog(owner, result, false, {});
}

namespace {

struct TranslationJob {
    std::wstring original;
    std::future<OcrTextResult> pending;
};

HRESULT CALLBACK TranslationProgress(HWND dialog, UINT notification,
                                     WPARAM, LPARAM, LONG_PTR data) {
    auto* job = reinterpret_cast<TranslationJob*>(data);
    if (notification == TDN_CREATED) {
        ::SendMessageW(dialog, TDM_SET_MARQUEE_PROGRESS_BAR, TRUE, 0);
        ::SendMessageW(dialog, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 30);
        ::SendMessageW(dialog, TDM_ENABLE_BUTTON, IDCANCEL, FALSE);
    } else if (notification == TDN_TIMER &&
               job->pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        ::SendMessageW(dialog, TDM_ENABLE_BUTTON, IDCANCEL, TRUE);
        ::SendMessageW(dialog, TDM_CLICK_BUTTON, IDCANCEL, 0);
    } else if (notification == TDN_BUTTON_CLICKED) {
        // 等待期间不销毁工作数据；网络有超时，任务收口后自动关闭进度窗。
        return job->pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready
            ? S_OK : S_FALSE;
    }
    return S_OK;
}

}  // namespace

void ShowTranslationDialog(const ScreenImage& image) {
    TranslationJob job;
    job.pending = std::async(std::launch::async, [&] {
        OcrTextResult result;
        bool initialized = false;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            initialized = true;
            result = RecognizeText(image, true);
            if (result.Succeeded()) {
                job.original = result.text;
                result = TranslateEnglishToChinese(job.original);
            }
        } catch (...) {
            result = {{}, {}, L"英文识别或翻译失败，请重试。"};
        }
        if (initialized) winrt::uninit_apartment();
        return result;
    });
    // Task Dialog 自带消息循环，等待期间保持窗口响应，不在 UI 线程执行 HTTPS。
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_CALLBACK_TIMER | TDF_SHOW_MARQUEE_PROGRESS_BAR;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.pszWindowTitle = L"MarkShot - 英文译中文";
    config.pszMainInstruction = L"正在识别并翻译，请稍候…";
    config.pszContent = L"英文文字将发送到百度翻译，不上传截图。";
    config.pfCallback = TranslationProgress;
    config.lpCallbackData = reinterpret_cast<LONG_PTR>(&job);
    (void)::TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
    const OcrTextResult result = job.pending.get();
    ShowTextResultDialog(nullptr, result, true, job.original);
}

}  // namespace native_shot
