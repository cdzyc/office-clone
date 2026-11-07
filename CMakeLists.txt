cmake_minimum_required(VERSION 3.16)
project(office-clone LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra)
endif()

# ---------------- 核心引擎（零依赖，纯 C++20）----------------
add_library(oc_core STATIC
    core/Inflate.cpp
    core/Deflate.cpp
    core/ZipReader.cpp
    core/ZipWriter.cpp
    core/PlatformIO.cpp
    core/XmlPull.cpp
    core/Document.cpp
    core/DocxReader.cpp
    core/DocxWriter.cpp
    core/FontCheck.cpp
    core/EditCommands.cpp
    core/Clipboard.cpp
    core/PngDecode.cpp
    core/ImageCache.cpp
    core/Layout.cpp
)
target_include_directories(oc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ---------------- 测试与基准（无 Qt 依赖）----------------
enable_testing()

add_executable(core_tests tests/core_tests.cpp)
target_link_libraries(core_tests PRIVATE oc_core)
add_test(NAME core_tests
         COMMAND core_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/data)

add_executable(m2_tests tests/m2_tests.cpp)
target_link_libraries(m2_tests PRIVATE oc_core)
add_test(NAME m2_tests
         COMMAND m2_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/data)

add_executable(m3_tests tests/m3_tests.cpp)
target_link_libraries(m3_tests PRIVATE oc_core)
add_test(NAME m3_tests
         COMMAND m3_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/data)

add_executable(perf_suite tests/perf_suite.cpp)
target_link_libraries(perf_suite PRIVATE oc_core)

find_package(Python3 COMPONENTS Interpreter)

add_test(NAME interop_verify
         COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_interop.py)

add_executable(oc_bench tests/bench.cpp)
target_link_libraries(oc_bench PRIVATE oc_core)

# ---------------- Qt UI（Windows 桌面）----------------
option(OC_BUILD_QT "Build Qt 6 desktop application" OFF)
if (OC_BUILD_QT)
    find_package(Qt6 REQUIRED COMPONENTS Widgets)
    # 兼容所有 Qt 6 版本（qt_standard_project_setup 需 6.3+）
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    set(CMAKE_AUTOUIC ON)

    add_executable(office-clone WIN32
        app/main.cpp
        app/MainWindow.cpp
        app/LogPanel.cpp
        app/PageView.cpp
    )
    target_link_libraries(office-clone PRIVATE Qt6::Widgets oc_core)
    set_target_properties(office-clone PROPERTIES
        WIN32_EXECUTABLE TRUE
        MACOSX_BUNDLE TRUE)
endif()

# 夹具生成提示
add_custom_command(TARGET core_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo
            "首次运行请先: python3 tests/make_fixtures.py")
