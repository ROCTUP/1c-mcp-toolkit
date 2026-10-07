cmake_minimum_required(VERSION 3.16)
project(ScreenCapture LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(FetchContent)

FetchContent_Declare(
    miniz
    URL https://github.com/richgel999/miniz/releases/download/3.1.1/miniz-3.1.1.zip
    URL_HASH SHA256=cb28402bb2af93bdc331b60d16807e89727d1712a2d0a7ba0cac79a3e406fe40
)
FetchContent_MakeAvailable(miniz)

set(SCREENCAPTURE_SOURCES
    src/dllmain.cpp
    src/component.cpp
    ${miniz_SOURCE_DIR}/miniz.c
)

if(UNIX AND NOT APPLE)
    list(APPEND SCREENCAPTURE_SOURCES src/component_linux.cpp)
endif()

add_library(ScreenCapture SHARED ${SCREENCAPTURE_SOURCES})

target_include_directories(ScreenCapture PRIVATE
    include
    src
    ${miniz_SOURCE_DIR}
)

set_target_properties(ScreenCapture PROPERTIES PREFIX "")

if(WIN32)
    set_target_properties(ScreenCapture PROPERTIES
        LINK_FLAGS "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/ScreenCapture.def")
    target_compile_definitions(ScreenCapture PRIVATE
        _WINDOWS WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(ScreenCapture PRIVATE gdi32 user32)
endif()

if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    target_include_directories(ScreenCapture PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(ScreenCapture PRIVATE ${X11_LIBRARIES})
endif()

if(MSVC)
    target_compile_options(ScreenCapture PRIVATE /W3 /utf-8 /EHsc)
else()
    target_compile_options(ScreenCapture PRIVATE -Wall -Wextra -fexceptions)
endif()
