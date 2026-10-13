#include "TranslationEngine.h"

#include <winhttp.h>
#include <bcrypt.h>
#include <objbase.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <string_view>
#include <thread>

namespace native_shot {
namespace {

using winrt::Windows::Data::Json::JsonObject;

void CheckHttp(BOOL success) {
    if (!success) {
        throw std::runtime_error("HTTPS request failed");
    }
}

// WinHTTP 句柄在异常路径上也自动关闭，不放宽系统证书校验。
struct HttpCloser {
    void operator()(void* handle) const noexcept { ::WinHttpCloseHandle(handle); }
};
using HttpHandle = std::unique_ptr<void, HttpCloser>;

std::wstring ConfigPath() {
    std::wstring path(32768, L'\0');
    const DWORD length = ::GetModuleFileNameW(nullptr, path.data(),
                                             static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        throw std::runtime_error("Cannot find executable path");
    }
    path.resize(length);
    path.resize(path.find_last_of(L"\\/") + 1);
    return path + L"MarkShot.translation.ini";
}

std::wstring ReadCredential(const std::wstring& path, const wchar_t* key) {
    std::array<wchar_t, 512> value{};
    const DWORD length = ::GetPrivateProfileStringW(
        L"Baidu", key, L"", value.data(), static_cast<DWORD>(value.size()), path.c_str());
    if (length >= value.size() - 1) {
        throw std::runtime_error("Credential too long");
    }
    return value.data();
}

// 大模型接口单次最多 6000 字符；按 Unicode 字符计数，代理对不计为两个字符。
bool FitsLlmRequest(const std::wstring& text) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        ++count;
        if (text[i] >= 0xD800 && text[i] <= 0xDBFF && i + 1 < text.size() &&
            text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
            ++i;
        }
    }
    return count > 0 && count <= 6000;
}

std::string Md5(const std::string& text) {
    // MD5 仅用于百度规定的请求签名；传输仍严格使用 HTTPS。
    std::array<unsigned char, 16> digest{};
    const NTSTATUS status = ::BCryptHash(
        BCRYPT_MD5_ALG_HANDLE, nullptr, 0,
        reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())),
        static_cast<ULONG>(text.size()), digest.data(), static_cast<ULONG>(digest.size()));
    if (status < 0) throw std::runtime_error("MD5 failed");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : digest) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

std::string EncodeForm(const std::wstring& text) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char byte : winrt::to_string(text)) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
            byte == '.' || byte == '~') {
            result += static_cast<char>(byte);
        } else {
            result += '%';
            result += hex[byte >> 4];
            result += hex[byte & 15];
        }
    }
    return result;
}

std::wstring JoinWrappedLines(const std::wstring& text) {
    std::wistringstream lines(text);
    std::wstring line, result;
    bool paragraphBreak = false;
    // ponytail: 单换行视为排版折行，空白行视为段落；列表/诗歌需保留行结构时再引入布局判断。
    while (std::getline(lines, line)) {
        const auto first = line.find_first_not_of(L" \t\r");
        if (first == std::wstring::npos) {
            paragraphBreak = !result.empty();
            continue;
        }
        if (!result.empty()) result += paragraphBreak ? L"\n\n" : L" ";
        result += line.substr(first, line.find_last_not_of(L" \t\r") - first + 1);
        paragraphBreak = false;
    }
    return result;
}

std::string BuildRequest(const std::wstring& original, const std::wstring& appId,
                         const std::wstring& secret, const std::wstring& salt,
                         bool machine = false) {
    // 签名与 q 必须使用同一份合并后的文字；结果窗口仍展示原始 OCR 文本。
    const auto text = JoinWrappedLines(original);
    if (!FitsLlmRequest(text)) throw std::runtime_error("Text exceeds LLM limit");
    // 先对原始 UTF-8 文本签名，再对表单参数编码；密钥不作为独立参数发送。
    const auto sign = Md5(winrt::to_string(appId + text + salt + secret));
    return "appid=" + EncodeForm(appId) + "&salt=" + EncodeForm(salt) +
        "&sign=" + sign + "&q=" + EncodeForm(text) + "&from=en&to=zh&model_type=" +
        (machine ? "nmt" : "llm");
}

void WaitForRequestSlot() {
    // 保守保留 1 QPS；在后台线程等待，同一进程的所有翻译入口共用限速。
    static std::mutex requestMutex;
    static std::chrono::steady_clock::time_point nextRequest;
    const std::lock_guard lock(requestMutex);
    std::this_thread::sleep_until(nextRequest);
    nextRequest = std::chrono::steady_clock::now() + std::chrono::milliseconds(1100);
}

std::string PostTranslation(const std::string& body) {
    HttpHandle session(::WinHttpOpen(L"MarkShot/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    CheckHttp(session != nullptr);
    CheckHttp(::WinHttpSetTimeouts(session.get(), 10000, 10000, 15000, 30000));
    HttpHandle connection(::WinHttpConnect(session.get(), L"fanyi-api.baidu.com",
                                           INTERNET_DEFAULT_HTTPS_PORT, 0));
    CheckHttp(connection != nullptr);
    HttpHandle request(::WinHttpOpenRequest(connection.get(), L"POST",
        L"/ait/api/aiTextTranslate", nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    CheckHttp(request != nullptr);
    // 禁用重定向，避免签名和识别文字被转发给其他主机。
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS;
    CheckHttp(::WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE,
                                &disabled, sizeof(disabled)));
    WaitForRequestSlot();
    CheckHttp(::WinHttpSendRequest(request.get(),
        L"Content-Type: application/x-www-form-urlencoded\r\nAccept: application/json\r\n",
        static_cast<DWORD>(-1), const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0));
    CheckHttp(::WinHttpReceiveResponse(request.get(), nullptr));
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    CheckHttp(::WinHttpQueryHeaders(request.get(),
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX));
    if (status != 200) throw std::runtime_error("Translation server HTTP error");

    std::string response;
    std::array<char, 8192> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    for (;;) {
        DWORD count = 0;
        CheckHttp(::WinHttpReadData(request.get(), buffer.data(),
                                   static_cast<DWORD>(buffer.size()), &count));
        if (count == 0) break;
        // 6000 字符请求仍只需小型 JSON，限制异常响应的累计字节数和接收时间。
        if (response.size() + count > 1024 * 1024 ||
            std::chrono::steady_clock::now() > deadline) {
            throw std::runtime_error("Translation response limit exceeded");
        }
        response.append(buffer.data(), count);
    }
    return response;
}

OcrTextResult ParseResponse(std::string_view response) {
    OcrTextResult result{{}, L"英文 → 简体中文", {}};
    const auto json = JsonObject::Parse(winrt::to_hstring(response));
    if (json.HasKey(L"error_code")) {
        const auto value = json.GetNamedValue(L"error_code");
        const std::wstring code = value.ValueType() == winrt::Windows::Data::Json::JsonValueType::String
            ? std::wstring(value.GetString().c_str()) : std::wstring(value.Stringify().c_str());
        // 不回显服务端原始消息；仅附加经过校验的数字错误码。
        if (code == L"53001") {
            result.error = L"百度大模型接口无法解析请求参数，请更新 MarkShot 后重试。";
        } else if (code == L"52003" || code == L"58002") {
            result.error = L"百度翻译未授权或已关闭，请确认 AppId 并开通大模型文本翻译 API。";
        } else if (code == L"54001") {
            result.error = L"百度翻译签名校验失败，请检查 AppId 和 SecretKey 是否匹配。";
        } else if (code == L"54003" || code == L"54005" || code == L"59004") {
            result.error = L"百度翻译请求过于频繁，请等待 3 秒后再试。";
        } else if (code == L"59003") {
            result.error = L"百度大模型翻译请求超过 6000 字符，请缩小选区。";
        } else if (code == L"90107") {
            result.error = L"百度开发者认证尚未通过或生效，请在控制台检查认证状态。";
        } else if (code == L"54004") {
            result.error = L"百度翻译额度或余额不足，请在百度控制台检查免费额度和计费设置。";
        } else {
            result.error = L"百度翻译失败，请检查服务授权、网络或控制台状态后重试。";
        }
        if (!code.empty() && code.size() <= 10 &&
            code.find_first_not_of(L"0123456789") == std::wstring::npos) {
            result.error += L"（错误码：" + code + L"）";
        }
        return result;
    }
    // API 按段返回数组；保留所有段落，不只取第一段。
    for (const auto& entry : json.GetNamedArray(L"trans_result")) {
        const auto text = entry.GetObject().GetNamedString(L"dst");
        if (text.empty()) {
            // OCR 的段间空行会返回 src/dst 均为空；只忽略空白原文，不吞掉实际文字翻译失败。
            const auto source = entry.GetObject().GetNamedString(L"src");
            if (std::wstring_view(source.c_str()).find_first_not_of(L" \t\r\n") ==
                std::wstring_view::npos) continue;
            throw std::runtime_error("Empty translation paragraph");
        }
        if (!result.text.empty()) result.text += L'\n';
        result.text += text.c_str();
    }
    if (result.text.empty()) result.error = L"百度未返回译文，请稍后重试。";
    return result;
}

// 仅余额不足触发一次重试；通过注入发送函数，离线测试实际切换流程而不消耗额度。
template<class Send>
OcrTextResult TranslateWithFallback(Send send) {
    auto response = send(false);
    const auto json = JsonObject::Parse(winrt::to_hstring(response));
    bool machine = false;
    if (json.HasKey(L"error_code")) {
        const auto code = json.GetNamedValue(L"error_code");
        machine = code.ValueType() == winrt::Windows::Data::Json::JsonValueType::String
            ? code.GetString() == L"54004"
            : code.ValueType() == winrt::Windows::Data::Json::JsonValueType::Number &&
              code.GetNumber() == 54004;
    }
    if (machine) response = send(true);
    auto result = ParseResponse(response);
    result.language = machine ? L"百度机器翻译（大模型余额不足，已自动切换）" : L"百度大模型文本翻译";
    if (machine && !result.Succeeded()) result.error = L"自动切换机器翻译后仍失败：" + result.error;
    return result;
}

}  // namespace

OcrTextResult TranslateEnglishToChinese(const std::wstring& text) {
    try {
        if (text.empty()) return {{}, {}, L"没有可翻译的英文文字。"};
        // ponytail: 超长原文不自动拆分/重复计费；需要长文翻译时再加分段请求。
        if (!FitsLlmRequest(text)) {
            return {{}, {}, L"百度大模型文本翻译单次最多 6000 字符，请缩小截图选区后重试。"};
        }
        const auto path = ConfigPath();
        const auto appId = ReadCredential(path, L"AppId");
        const auto secret = ReadCredential(path, L"SecretKey");
        if (appId.empty() || secret.empty()) {
            return {{}, {}, L"请在 [Baidu] 配置百度大模型文本翻译 API的 AppId 和 SecretKey：\n" + path +
                L"\n\n参考 MarkShot.translation.example.ini；请勿将密钥发给他人。"};
        }
        GUID guid{};
        winrt::check_hresult(::CoCreateGuid(&guid));
        // 实际大模型服务会将 salt 解析为数字，使用 GUID 的随机整数部分。
        const std::wstring salt = std::to_wstring(guid.Data1);
        return TranslateWithFallback([&](bool machine) {
            return PostTranslation(BuildRequest(text, appId, secret, salt, machine));
        });
    } catch (...) {
        // 不输出异常原文：网络/JSON 错误可能携带敏感内容。
        return {{}, {}, L"百度翻译失败：请检查网络和配置，然后重试。"};
    }
}

}  // namespace native_shot

#ifdef MARKSHOT_TRANSLATION_SELF_TEST
#include <iostream>

int main() {
    winrt::init_apartment();
    using namespace native_shot;
    // 显式检查在 Release 也有效；全程离线，不读取真实密钥、不计费。
    const auto check = [](bool ok) { if (!ok) throw std::runtime_error("Translation self-check failed"); };
    check(Md5("abc") == "900150983cd24fb0d6963f7d28e17f72");
    // 百度官方示例固定向量：验证 UTF-8 原文签名顺序及小写十六进制格式。
    check(Md5("2015063000000001apple143566028812345678") == "f89f9594663708c1605f3d736d01d2d4");
    check(EncodeForm(L"A &中") == "A%20%26%E4%B8%AD");
    check(BuildRequest(L"Hello", L"app", L"secret", L"123", true).ends_with("model_type=nmt"));
    for (const auto* code : {"\"54004\"", "54004"}) {
        int calls = 0;
        const auto result = TranslateWithFallback([&](bool machine) {
            ++calls;
            check(machine == (calls == 2));
            return machine ? std::string(R"({"trans_result":[{"dst":"你好"}]})") :
                std::string("{\"error_code\":") + code + "}";
        });
        check(calls == 2 && result.Succeeded() && result.language.find(L"机器") != std::wstring::npos);
    }
    for (const auto* response : {R"({"error_code":"54001"})", R"({"error_code":"54003"})",
                                 R"({"trans_result":[{"dst":"你好"}]})"}) {
        int calls = 0;
        (void)TranslateWithFallback([&](bool machine) { ++calls; check(!machine); return std::string(response); });
        check(calls == 1);
    }
    int failedCalls = 0;
    const auto failedFallback = TranslateWithFallback([&](bool) {
        ++failedCalls; return std::string(R"({"error_code":"54004"})");
    });
    check(failedCalls == 2 && !failedFallback.Succeeded());
    check(JoinWrappedLines(L" online\r\nattacks. The security\nsolution. ") ==
          L"online attacks. The security solution.");
    check(JoinWrappedLines(L"\nTitle\n \t\n\nFirst line\nsecond line\n") ==
          L"Title\n\nFirst line second line");
    check(JoinWrappedLines(L" \r\n\t\n").empty());
    check(BuildRequest(L"online\nattacks.", L"app", L"secret", L"123") ==
          BuildRequest(L"online attacks.", L"app", L"secret", L"123"));
    check(FitsLlmRequest(std::wstring(6000, L'a')));
    check(FitsLlmRequest(std::wstring(6000, L'中')));
    check(!FitsLlmRequest(std::wstring(6001, L'a')));
    check(!FitsLlmRequest(L""));
    std::wstring emojiText;
    for (int i = 0; i < 6000; ++i) emojiText += L"\U0001F600";
    check(FitsLlmRequest(emojiText));
    check(!FitsLlmRequest(emojiText + L"x"));
    const auto body = BuildRequest(L"apple", L"2015063000000001", L"12345678", L"1435660288");
    check(body == "appid=2015063000000001&salt=1435660288&sign=f89f9594663708c1605f3d736d01d2d4&q=apple&from=en&to=zh&model_type=llm");
    check(BuildRequest(L"A &中", L"app", L"secret", L"salt").find(
        "sign=" + Md5(winrt::to_string(L"appA &中saltsecret"))) != std::string::npos);
    check(ParseResponse("{\"from\":\"en\",\"to\":\"zh\",\"trans_result\":[{\"dst\":\"苹果\"},{\"dst\":\"你好\"}]}").text == L"苹果\n你好");
    // 真实接口会为换行间的空白段落保留空 src/dst，不能使整个多行翻译失败。
    check(ParseResponse(R"({"trans_result":[{"src":"Hello","dst":"你好"},{"src":"","dst":""},{"src":" \t","dst":""},{"src":"World","dst":"世界"}]})").text == L"你好\n世界");
    check(!ParseResponse(R"({"trans_result":[{"src":"","dst":""}]})").Succeeded());
    check(!ParseResponse("{\"error_code\":\"52003\"}").Succeeded());
    check(ParseResponse("{\"error_code\":54004}").error.find(L"额度") != std::wstring::npos);
    check(ParseResponse("{\"error_code\":\"54003\"}").error.find(L"频繁") != std::wstring::npos);
    check(ParseResponse("{\"error_code\":\"59004\"}").error.find(L"频繁") != std::wstring::npos);
    check(ParseResponse("{\"error_code\":\"53001\"}").error.find(L"53001") != std::wstring::npos);
    check(ParseResponse("{\"error_code\":\"unsafe-token\"}").error.find(L"unsafe-token") == std::wstring::npos);
    check(ParseResponse("{\"error_code\":\"90107\"}").error.find(L"认证") != std::wstring::npos);
    check(ParseResponse("{\"error_code\":\"59003\"}").error.find(L"6000") != std::wstring::npos);
    check(!ParseResponse("{\"trans_result\":[]}").Succeeded());
    for (const auto* invalid : {"{bad json}", "{}", "{\"trans_result\":[{\"dst\":\"\"}]}",
                               R"({"trans_result":[{"src":"Hello","dst":""}]})"}) {
        bool rejected = false;
        try { (void)ParseResponse(invalid); } catch (...) { rejected = true; }
        check(rejected);
    }
    bool oversizedRejected = false;
    try { (void)BuildRequest(std::wstring(6001, L'a'), L"app", L"secret", L"salt"); } catch (...) { oversizedRejected = true; }
    check(oversizedRejected);
    check(!TranslateEnglishToChinese(std::wstring(6001, L'a')).Succeeded());
    WaitForRequestSlot();
    const auto start = std::chrono::steady_clock::now();
    WaitForRequestSlot();
    check(std::chrono::steady_clock::now() - start >= std::chrono::seconds(1));
    std::cout << "Baidu LLM translation self-check passed (offline)\n";
    winrt::uninit_apartment();
}
#endif
