#include "OcrEngine.h"

#include <filesystem>
#include <memory>
#include <stdexcept>

namespace native_shot {
namespace {

// 仅绑定 Tesseract 5 的稳定 C API，避免与第三方 DLL 交换 C++/CRT 对象。
struct TesseractApi {
    HMODULE module = nullptr;
    void* (__cdecl* create)() = nullptr;
    void (__cdecl* destroy)(void*) = nullptr;
    int (__cdecl* init)(void*, const char*, const char*, int) = nullptr;
    void (__cdecl* setImage)(void*, const unsigned char*, int, int, int, int) = nullptr;
    void (__cdecl* setPageMode)(void*, int) = nullptr;
    char* (__cdecl* getText)(void*) = nullptr;
    void (__cdecl* deleteText)(const char*) = nullptr;

    ~TesseractApi() { if (module) ::FreeLibrary(module); }

    template<class Function>
    void Bind(Function& function, const char* name) {
        function = reinterpret_cast<Function>(::GetProcAddress(module, name));
        if (!function) throw std::runtime_error("Tesseract C API unavailable");
    }

    void Load(const std::filesystem::path& directory) {
        // 限定依赖搜索到 DLL 自身目录和系统目录，不从工作目录加载同名 DLL。
        module = ::LoadLibraryExW((directory / L"tesseract50.dll").c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) throw std::runtime_error("Tesseract DLL unavailable");
        Bind(create, "TessBaseAPICreate");
        Bind(destroy, "TessBaseAPIDelete");
        Bind(init, "TessBaseAPIInit2");
        Bind(setImage, "TessBaseAPISetImage");
        Bind(setPageMode, "TessBaseAPISetPageSegMode");
        Bind(getText, "TessBaseAPIGetUTF8Text");
        Bind(deleteText, "TessDeleteText");
    }
};

std::filesystem::path RuntimeDirectory() {
    std::wstring executable(32768, L'\0');
    const DWORD length = ::GetModuleFileNameW(nullptr, executable.data(),
                                              static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) throw std::runtime_error("EXE path unavailable");
    executable.resize(length);
    return std::filesystem::path(executable).parent_path() / L"ocr";
}

std::wstring DecodeText(const char* text) {
    const int count = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (!count) throw std::runtime_error("Invalid OCR UTF-8");
    std::wstring result(count, L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, result.data(), count);
    result.resize(count - 1);
    const auto first = result.find_first_not_of(L" \t\r\n");
    return first == std::wstring::npos ? std::wstring{} :
        result.substr(first, result.find_last_not_of(L" \t\r\n") - first + 1);
}

std::string ModelPath(const std::filesystem::path& path) {
    // ponytail: 此发行版限系统代码页路径；任意 Unicode 部署需换支持内存模型初始化的新版 DLL。
    // 此发行版以系统代码页打开模型文件；拒绝替换字符，避免中文路径被错误转为 UTF-8。
    const auto wide = path.wstring();
    const bool utf8 = ::GetACP() == CP_UTF8;
    BOOL substituted = FALSE;
    const DWORD flags = utf8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    BOOL* usedDefault = utf8 ? nullptr : &substituted;
    const int size = ::WideCharToMultiByte(CP_ACP, flags, wide.c_str(), -1,
                                          nullptr, 0, nullptr, usedDefault);
    if (!size || substituted) throw std::runtime_error("Model path encoding unavailable");
    std::string result(size, '\0');
    if (!::WideCharToMultiByte(CP_ACP, flags, wide.c_str(), -1, result.data(), size,
                              nullptr, usedDefault) || substituted) {
        throw std::runtime_error("Model path encoding unavailable");
    }
    result.resize(size - 1);
    return result;
}

}  // namespace

OcrTextResult RecognizeEnglishWithTesseract(const ScreenImage& image) {
    OcrTextResult result;
    try {
        const auto directory = RuntimeDirectory();
        if (!std::filesystem::is_regular_file(directory / L"tessdata/eng.traineddata"))
            return {{}, {}, L"缺少 Tesseract 英文模型，请保留 EXE 旁的 ocr/tessdata/eng.traineddata。"};
        const auto modelPath = ModelPath(directory / L"tessdata");
        TesseractApi api;
        api.Load(directory);
        const std::unique_ptr<void, decltype(api.destroy)> engine(api.create(), api.destroy);
        // OEM_LSTM_ONLY=1；所附 tessdata_fast 模型仅支持 LSTM 引擎。
        if (!engine || api.init(engine.get(), modelPath.c_str(), "eng", 1)) {
            return {{}, {}, L"Tesseract 英文模型初始化失败，请检查 eng.traineddata。"};
        }
        // BGRA 转为灰度，忽略透明通道；原始行跨度允许包含填充字节。
        std::vector<unsigned char> gray(static_cast<size_t>(image.width) * image.height);
        for (int y = 0; y < image.height; ++y) {
            const auto* row = image.pixels.data() + static_cast<size_t>(y) * image.stride;
            for (int x = 0; x < image.width; ++x) {
                const auto* pixel = row + static_cast<size_t>(x) * 4;
                gray[static_cast<size_t>(y) * image.width + x] =
                    static_cast<unsigned char>((29 * pixel[0] + 150 * pixel[1] + 77 * pixel[2]) >> 8);
            }
        }
        api.setImage(engine.get(), gray.data(), image.width, image.height, 1, image.width);
        api.setPageMode(engine.get(), 3); // PSM_AUTO：自动分析选区内的多行和文本块，无需 OSD 模型。
        const std::unique_ptr<char, decltype(api.deleteText)> text(api.getText(engine.get()), api.deleteText);
        if (!text) return {{}, {}, L"Tesseract 英文识别失败。"};
        result.text = DecodeText(text.get());
        result.language = L"English (Tesseract)";
        if (result.text.empty()) result.error = L"未在选区中识别到英文文字。";
    } catch (const std::exception&) {
        result.error = L"Tesseract 英文 OCR 加载或识别失败，请检查 EXE 旁的 ocr 运行库；路径包含特殊字符时请移至纯英文目录。";
    }
    return result;
}

}  // namespace native_shot
