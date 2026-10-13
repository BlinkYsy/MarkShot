cmake_minimum_required(VERSION 3.20)

cmake_policy(SET CMP0091 NEW)
project(MarkShot VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_SUPPRESS_REGENERATION ON)
add_compile_options(/W4 /permissive- /utf-8 /EHsc)

add_executable(MarkShot WIN32
    src/NativeShot.rc
    src/main.cpp
    src/TrayApplication.cpp
    src/AnnotationDocument.cpp
    src/ScreenImage.cpp
    src/OcrEngine.cpp
    src/TesseractOcr.cpp
    src/OcrDialog.cpp
    src/TranslationEngine.cpp
    src/OverlayWindow.cpp
    src/RenderEngine.cpp
    src/PinWindow.cpp
)

target_include_directories(MarkShot PRIVATE src)
target_compile_definitions(MarkShot PRIVATE
    UNICODE _UNICODE STRICT NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(MarkShot PRIVATE
    d2d1 dwrite comctl32 comdlg32 shell32 user32 gdi32 windowscodecs ole32 windowsapp
    winhttp bcrypt)

# 英文 OCR 的原生 DLL、模型和许可证随每个程序/测试复制，不依赖 PATH 或系统语言包。
function(copy_english_ocr target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tesseract"
            "$<TARGET_FILE_DIR:${target}>/ocr")
    # 使用 VS 可再分发 CRT 实现应用本地部署，不复制开发工具或系统 DLL。
    file(GLOB crt_directories
        "${CMAKE_GENERATOR_INSTANCE}/VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT")
    if(crt_directories)
        list(SORT crt_directories COMPARE NATURAL ORDER DESCENDING)
        list(GET crt_directories 0 crt_directory)
        foreach(dll msvcp140.dll vcruntime140.dll vcruntime140_1.dll)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${crt_directory}/${dll}" "$<TARGET_FILE_DIR:${target}>/ocr/${dll}")
        endforeach()
    else()
        message(WARNING "Tesseract requires the Microsoft VC++ x64 runtime on the target PC")
    endif()
endfunction()
copy_english_ocr(MarkShot)

# 只随 EXE 附带空白模板，绝不复制用户真实密钥。
add_custom_command(TARGET MarkShot POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/MarkShot.translation.example.ini"
        "$<TARGET_FILE_DIR:MarkShot>/MarkShot.translation.example.ini")

if(MSVC)
    target_link_options(MarkShot PRIVATE
        "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
endif()

option(NATIVE_SHOT_TESTS "Build the small non-UI self-check" OFF)
if(NATIVE_SHOT_TESTS)
    enable_testing()
    add_executable(native_shot_tests
        tests/DocumentTest.cpp
        src/AnnotationDocument.cpp
        src/ScreenImage.cpp
        src/OcrEngine.cpp
        src/TesseractOcr.cpp
    )
    target_include_directories(native_shot_tests PRIVATE src)
    target_compile_definitions(native_shot_tests PRIVATE
        UNICODE _UNICODE STRICT NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(native_shot_tests PRIVATE gdi32 windowscodecs ole32 windowsapp)
    add_test(NAME native_shot_tests COMMAND native_shot_tests)
    copy_english_ocr(native_shot_tests)

    add_executable(native_shot_ocr_smoke
        tests/OcrSmokeTest.cpp
        src/OcrEngine.cpp
        src/TesseractOcr.cpp
    )
    target_include_directories(native_shot_ocr_smoke PRIVATE src)
    target_compile_definitions(native_shot_ocr_smoke PRIVATE
        UNICODE _UNICODE STRICT NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(native_shot_ocr_smoke PRIVATE
        gdi32 windowscodecs ole32 windowsapp)
    add_test(NAME native_shot_ocr_smoke COMMAND native_shot_ocr_smoke)
    copy_english_ocr(native_shot_ocr_smoke)
    set_tests_properties(native_shot_ocr_smoke PROPERTIES SKIP_RETURN_CODE 77)

    # 离线检查百度签名、表单编码、JSON 结果及标准版限制，不调用 API、不计费。
    add_executable(markshot_translation_check src/TranslationEngine.cpp)
    target_compile_definitions(markshot_translation_check PRIVATE
        UNICODE _UNICODE STRICT NOMINMAX WIN32_LEAN_AND_MEAN MARKSHOT_TRANSLATION_SELF_TEST)
    target_link_libraries(markshot_translation_check PRIVATE winhttp bcrypt ole32 windowsapp)
    add_test(NAME markshot_translation_check COMMAND markshot_translation_check)
endif()
