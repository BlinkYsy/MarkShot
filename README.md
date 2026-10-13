# MarkShot

## 直接运行

下载仓库后运行 `build/Release/MarkShot.exe`，保留同目录的整个 `ocr` 文件夹。
翻译需将空白模板 `MarkShot.translation.example.ini` 复制为 `MarkShot.translation.ini`，
在本机填写自己的百度凭据。公开仓库不包含真实账号或密钥。

Windows 原生截图标注工具。该子项目参考 EShot 的交互方式，但不依赖 Qt，
只使用 Win32、Direct2D、DirectWrite、GDI 和 WIC。

## 第一阶段功能

- 启动一次后驻留系统托盘，按全局快捷键 `Ctrl+A` 热启动截图；
- 托盘右键可选择“截图”或“退出”，双击托盘图标也可截图；
- 单实例运行，再次执行 EXE 会让已有进程直接进入截图；
- 拖动选择截图区域；
- Direct2D 矢量图标工具栏，悬停时显示中文功能提示；
- 矩形、圆、直线、箭头、画笔、文本、序号和马赛克；
- 使用 Windows 10/11 自带 OCR 识别当前选区，并在结果窗口中一键复制；
- OCR 旁的“英文译中文”使用本地 Tesseract 英文 OCR 和百度大模型文本翻译，弹窗显示中文并可展开英文原文；
- 撤销、重做、保存 PNG、复制到剪贴板和置顶；
- `Esc` 取消，`Enter` 完成，`Ctrl+Z/Y/S/C` 对应常用操作。

## 构建

在 Visual Studio 2022 Developer PowerShell 中运行：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

输出：`build/Release/MarkShot.exe`。

运行后程序不会立即遮罩屏幕，而是驻留系统托盘。按 `Ctrl+A` 开始截图；
截图完成或取消后进程仍在后台。如需彻底关闭，请右键托盘图标并选择“退出”。

测试目标默认不生成。如需验证标注文档和 PNG 编码：

```powershell
cmake -S . -B build-test -A x64 -DNATIVE_SHOT_TESTS=ON
cmake --build build-test --config Debug
ctest --test-dir build-test -C Debug --output-on-failure
```

## 英文译中文配置

1. 在百度翻译开放平台注册开发者，完成开发者认证并开通“大模型文本翻译 API”（产品页面： https://fanyi-api.baidu.com/product/13 ）。
2. 把 `MarkShot.translation.example.ini` 复制到 `MarkShot.exe` 同一目录，
   改名为 `MarkShot.translation.ini`，在 `[Baidu]` 节填写 `AppId` 和 `SecretKey`。
   已有配置文件只需自行增加此节，旧有道凭据不会被读取，也不要公开分发真实密钥。
3. 保留 EXE 旁自动生成的 `ocr` 文件夹；框选后点击 OCR 右侧的翻译按钮。
   英文识别使用内置 Tesseract 和 `eng.traineddata`，不需要 Windows 英文 OCR 语言包。

使用 [百度官方大模型文本翻译 API](https://fanyi-api.baidu.com/product/133)，
调用 `/ait/api/aiTextTranslate`，明确指定 `model_type=llm`、英文到简体中文。
复用 AppId/SecretKey 的 MD5 签名鉴权，单次上限 6000 字符；超长原文提示缩小选区，不自动拆分请求。
大模型返回余额不足错误 `54004` 时，使用相同凭据自动改用 `model_type=nmt` 重试一次；
其他错误不切换，结果窗口标明实际模型。免费额度耗尽但请求仍成功时不会切换。
程序在后台保守按 1 QPS 限速；免费额度和计费以
[百度大模型翻译产品定价](https://fanyi-api.baidu.com/product/132) 及账户控制台为准。
识别在本地完成，只有识别出的英文发送到百度，不上传截图。
复制译文或关闭结果会结束本次截图，托盘继续运行。

迁移电脑时一起复制 EXE、`ocr` 文件夹和配置文件；配置含明文密钥，仅供个人本机使用，
不要公开分发；在新电脑配置自己的密钥更安全。英文模型已随构建附带，不需要另外安装。
翻译需要联网，缺少配置、OCR 文件或服务授权时会提示错误。
第三方运行库来源、版本、许可证和校验值见 `third_party/tesseract/NOTICE.md`。

## 当前边界

第一阶段暂不包含录屏、上传、滚动截图和设置持久化。OCR 使用系统已安装的
Windows OCR 语言；若缺少目标语言，需先在 Windows 语言设置中安装对应语言包。点击置顶后截图选择层
会立即关闭，只保留与选择区域像素尺寸完全一致的置顶图。置顶图可拖动，按 `Esc`、
鼠标左键双击或右键可关闭。
