cmake_minimum_required(VERSION 3.16)
project(ScreenCapture LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

include(FetchContent)

FetchContent_Declare(
    miniz
    URL https://github.com/richgel999/miniz/releases/download/3.1.1/miniz-3.1.1.zip
    URL_HASH SHA256=cb28402bb2af93bdc331b60d16807e89727d1712a2d0a7ba0cac79a3e406fe40
)
FetchContent_MakeAvailable(miniz)

add_library(ScreenCapture SHARED
    src/dllmain.cpp
    src/component.cpp
    src/overlay_selection.cpp
    ${miniz_SOURCE_DIR}/miniz.c
)

target_include_directories(ScreenCapture PRIVATE
    include
    src
    ${miniz_SOURCE_DIR}
)

set_target_properties(ScreenCapture PROPERTIES PREFIX "")
if(WIN32)
    target_sources(ScreenCapture PRIVATE src/ScreenCapture.def)
    target_compile_definitions(ScreenCapture PRIVATE
        _WINDOWS WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(ScreenCapture PRIVATE gdi32 user32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb xcb-res xcb-composite xcb-render xcb-shape)
    target_sources(ScreenCapture PRIVATE src/component_linux.cpp src/linux_capture.cpp src/raster.cpp)
    target_link_libraries(ScreenCapture PRIVATE PkgConfig::XCB)
    target_link_options(ScreenCapture PRIVATE -Wl,--no-undefined)
else()
    message(FATAL_ERROR "ScreenCapture supports Windows and Linux/X11 only")
endif()
if(MSVC)
    target_compile_options(ScreenCapture PRIVATE /W3 /utf-8 /EHsc)
else()
    target_compile_options(ScreenCapture PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
#  Unit tests
# ---------------------------------------------------------------------------
# Только правило отбора слоёв: оно не зовёт Win32 и потому проверяется без 1С и без окон.
# Отрисовщик и сборщик остаются за границей тестов сознательно — они и есть Win32.
option(SCREENCAPTURE_TESTS "Build ScreenCapture unit tests" OFF)
if(SCREENCAPTURE_TESTS)
    enable_testing()
    add_executable(screencapture_tests
        tests/overlay_tests.cpp
        src/overlay_selection.cpp
    )
    target_include_directories(screencapture_tests PRIVATE src)
    if(MSVC)
        target_compile_options(screencapture_tests PRIVATE /W3 /utf-8)
    endif()
    add_test(NAME overlay_selection COMMAND screencapture_tests)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(linux_capture_tests tests/linux_capture_tests.cpp)
        target_include_directories(linux_capture_tests PRIVATE src include)
        target_link_libraries(linux_capture_tests PRIVATE PkgConfig::XCB ScreenCapture)
        target_compile_options(linux_capture_tests PRIVATE -Wall -Wextra -Wpedantic)
        add_test(NAME linux_capture COMMAND xvfb-run -a -s "-screen 0 1280x800x24" $<TARGET_FILE:linux_capture_tests>)
        file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/manual-tests)
        add_test(NAME linux_capture_manual COMMAND xvfb-run -a -s "-screen 0 1280x800x24" $<TARGET_FILE:linux_capture_tests> --manual)
        set_tests_properties(linux_capture_manual PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/manual-tests)
        add_test(NAME linux_no_composite COMMAND xvfb-run -a -s "-screen 0 1280x800x24 -extension Composite" $<TARGET_FILE:linux_capture_tests> --no-composite)
        add_test(NAME linux_png COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_png.py ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(linux_png PROPERTIES DEPENDS linux_capture)
        add_test(NAME linux_png_manual COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_png.py ${CMAKE_CURRENT_BINARY_DIR}/manual-tests)
        set_tests_properties(linux_png_manual PROPERTIES DEPENDS linux_capture_manual)
    endif()
endif()
