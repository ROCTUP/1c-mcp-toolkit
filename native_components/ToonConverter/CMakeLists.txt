cmake_minimum_required(VERSION 3.16)
project(ToonConverter LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")


add_library(ctoon STATIC
    third_party/ctoon/src/sources/ctoon.cpp
    third_party/ctoon/src/sources/ctoon_json.cpp
    third_party/ctoon/src/sources/ctoon_toon.cpp
    third_party/ctoon/src/sources/utils.cpp
    third_party/ctoon/src/sources/yyjson.c
)
target_include_directories(ctoon
    PUBLIC  third_party/ctoon/includes
    PRIVATE third_party/ctoon/src/sources)
target_compile_features(ctoon PUBLIC cxx_std_17)
# /Zc:twoPhase- fixes circular type dependency (ctoon::Value <-> tsl::ordered_map)
# that causes C2079 in MSVC 2022 17.14+ with strict two-phase lookup
target_compile_options(ctoon PRIVATE /EHsc /W3 /Zc:twoPhase-)

add_library(ToonConverter SHARED
    src/dllmain.cpp
    src/component.cpp
    src/toon_encoder.cpp
)
target_include_directories(ToonConverter PRIVATE include src)
target_link_libraries(ToonConverter PRIVATE ctoon)
set_target_properties(ToonConverter PROPERTIES
    PREFIX ""
    LINK_FLAGS "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/ToonConverter.def")
target_compile_definitions(ToonConverter PRIVATE
    _WINDOWS WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(ToonConverter PRIVATE /W3 /utf-8 /EHsc)
